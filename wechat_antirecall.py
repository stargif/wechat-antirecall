#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
wechat-antirecall —— macOS 微信 4.x 防撤回补丁工具
=================================================

原理
----
微信 4.x 把消息逻辑从主程序搬进了 `Contents/Resources/wechat.dylib`。
当对方点「撤回」时，服务器推 `revokemsg` 指令，客户端有一个函数负责把撤回
XML 解析成结构体（抽出 `newmsgid` = 删哪条、`replacemsg` = 提示文案），
下游消费者再据此「删本地消息 + 插入撤回提示」。

本工具在 `wechat.dylib` 的 arm64 切片上做**原地等长字节替换**，有三种变体：

  silent  (默认)  把撤回解析/分类函数入口 8 字节换成 `mov w0,#0; ret`
                  → 解析直接返回空，下游拿不到任何输入
                  → 消息留住、不弹「对方撤回了一条消息」（静默）

  keeptip         先还原上面的函数，再让解析器照跑，只把写入结构体的
                  `newmsgid` 改成 0（`str x0` → `str xzr`）
                  → 下游按 id=0 删不到东西，但提示文案照常渲染
                  → 消息留住、仍显示撤回提示（私聊有效；群聊提示依赖 newmsgid，会退化为静默）

  keeptip-content 同 keeptip，另加载一个小模块（runtime/），把撤回的内容写进提示
                  → 例如：撤回了一条消息：「1」（目前仅构建 270416，详见 docs/）

安全设计
--------
* **写入前逐字节校验**：每个补丁点都带 `expected` 原始字节，对不上就整体中止，
  绝不盲写。因此打错微信版本只会报错，不会把微信改坏。
* **切片 SHA-256 门禁**：对已独立复核的构建号，先核对 arm64 切片的 SHA-256。
* **事务式写入**：任何一个补丁点写失败，立即用读到的原始字节回滚全部改动。
* **打补丁前全量备份**：用 `ditto` 整包备份到用户目录，可一键还原。

重签名
------
微信是 App Sandbox + Hardened Runtime 签名，且 amfid 会校验签名。改完
`wechat.dylib` 后必须重签，而且不能简单地 `codesign --deep --sign -`（那会剥掉
entitlements，SIP 开启的机器上微信直接起不来）。本工具按 fzlzjerry/wechat-antirecall
与 zengtianli/WeChatTweak 验证过的流程重签：快照 → 注入两个必需权限 → 剥离 ad-hoc
签名带不了的受限权限（`com.apple.developer.*`）→ 分层次签名 → 复核 → `--verify --strict`。

用法
----
    sudo python3 wechat_antirecall.py info      # 只看状态，不修改任何东西
    sudo python3 wechat_antirecall.py apply     # 打补丁（静默变体 + 全量备份）
    sudo python3 wechat_antirecall.py apply --variant keeptip
    sudo python3 wechat_antirecall.py restore   # 从备份还原
    sudo python3 wechat_antirecall.py verify    # 校验签名

参考实现（补丁点来源）
* zengtianli/WeChatTweak      —— 补丁点数据库 / 重签名流程
* fzlzjerry/wechat-antirecall —— keeptip 思路 / 重签名流程
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import plistlib
import platform
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

__version__ = "1.2.0"

DEFAULT_APP = "/Applications/WeChat.app"
DB_PATH = Path(__file__).resolve().parent / "patches.json"
BACKUP_DIR_NAME = "WeChatAntiRecall"

# ---------------------------------------------------------------- Mach-O 常量

FAT_MAGIC = 0xCAFEBABE
FAT_CIGAM = 0xBEBAFECA
MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
CPU_ARM64 = 0x0100000C
CPU_X86_64 = 0x01000007

ARCH_NAME = {CPU_ARM64: "arm64", CPU_X86_64: "x86_64"}
ARCH_CPU = {v: k for k, v in ARCH_NAME.items()}

# 重签时必须注入的两个权限（详见 Resigner 段注释）
INJECTED_ENTITLEMENTS = {
    "com.apple.security.cs.disable-library-validation": True,
    "com.apple.security.cs.allow-unsigned-executable-memory": True,
}

C_RED, C_GREEN, C_YELLOW, C_BLUE, C_DIM, C_OFF = (
    ("\033[31m", "\033[32m", "\033[33m", "\033[36m", "\033[2m", "\033[0m")
    if sys.stdout.isatty()
    else ("", "", "", "", "", "")
)


class ToolError(Exception):
    """面向用户的、可预期的错误。"""


def info(msg: str) -> None:
    print(f"{C_BLUE}·{C_OFF} {msg}")


def ok(msg: str) -> None:
    print(f"{C_GREEN}✓{C_OFF} {msg}")


def warn(msg: str) -> None:
    print(f"{C_YELLOW}!{C_OFF} {msg}")


def die(msg: str, code: int = 1):
    print(f"{C_RED}✗ {msg}{C_OFF}", file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------- 进程工具


def run(cmd, check: bool = False):
    p = subprocess.run(cmd, capture_output=True)
    if check and p.returncode != 0:
        raise ToolError(
            f"命令失败({p.returncode}): {' '.join(str(c) for c in cmd)}\n"
            f"{p.stdout.decode(errors='replace')}{p.stderr.decode(errors='replace')}"
        )
    return p.returncode, p.stdout, p.stderr


def run_text(cmd, check: bool = False) -> str:
    _, out, err = run(cmd, check=check)
    return (out + err).decode(errors="replace")


def console_user() -> str | None:
    """当前登录 GUI 的用户名（即使在 root 下、没有 SUDO_USER 也能拿到）。"""
    rc, out, _ = run(["/usr/bin/stat", "-f", "%Su", "/dev/console"])
    name = out.decode(errors="replace").strip() if rc == 0 else ""
    return name or None


def _real_name() -> str | None:
    for cand in (os.environ.get("SUDO_USER"), console_user()):
        if cand and cand != "root":
            return cand
    return None


def real_user_home() -> Path:
    """sudo / GUI 提权之下 `~` 是 /var/root，这里取真实登录用户的家目录。"""
    if os.geteuid() == 0:
        name = _real_name()
        if name:
            try:
                import pwd

                return Path(pwd.getpwnam(name).pw_dir)
            except Exception:
                pass
    return Path.home()


def real_user() -> str | None:
    return _real_name() if os.geteuid() == 0 else None


def chown_to_user(path: Path, recursive: bool = True) -> None:
    u = real_user()
    if not u or os.geteuid() != 0:
        return
    try:
        import grp
        import pwd

        pw = pwd.getpwnam(u)
        gid = pw.pw_gid
        for root, dirs, files in os.walk(path) if recursive else ():
            for n in dirs + files:
                try:
                    os.chown(os.path.join(root, n), pw.pw_uid, gid, follow_symlinks=False)
                except OSError:
                    pass
        os.chown(path, pw.pw_uid, gid, follow_symlinks=False)
    except Exception:
        pass


# ---------------------------------------------------------------- Mach-O 解析


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def _u32be(b: bytes, o: int) -> int:
    return struct.unpack_from(">I", b, o)[0]


def _u64(b: bytes, o: int) -> int:
    return struct.unpack_from("<Q", b, o)[0]


class Slice:
    __slots__ = ("cpu", "offset", "size")

    def __init__(self, cpu: int, offset: int, size: int):
        self.cpu, self.offset, self.size = cpu, offset, size

    @property
    def arch(self) -> str:
        return ARCH_NAME.get(self.cpu, f"cpu:0x{self.cpu:x}")


def parse_slices(fh, file_size: int) -> list[Slice]:
    fh.seek(0)
    head = fh.read(8)
    if len(head) < 8:
        raise ToolError("文件太小，不是有效的 Mach-O")

    magic = _u32be(head, 0)
    if magic in (FAT_MAGIC, FAT_CIGAM):
        swapped = magic == FAT_CIGAM
        nfat = _u32be(head, 4) if not swapped else struct.unpack("<I", head[4:8])[0]
        table_end = 8 + nfat * 20
        if table_end > file_size:
            raise ToolError("fat header 越界")
        slices = []
        fh.seek(0)
        table = fh.read(table_end)
        for i in range(nfat):
            o = 8 + i * 20
            cpu = _u32be(table, o) if not swapped else _u32(table, o)
            off = _u32be(table, o + 8) if not swapped else _u32(table, o + 8)
            size = _u32be(table, o + 12) if not swapped else _u32(table, o + 12)
            if off < table_end or size < 32 or off + size > file_size:
                raise ToolError("fat 切片越界")
            slices.append(Slice(cpu, off, size))
        ordered = sorted(slices, key=lambda s: s.offset)
        for a, b in zip(ordered, ordered[1:]):
            if a.offset + a.size > b.offset:
                raise ToolError("fat 切片重叠")
        return slices

    fh.seek(0)
    hdr = fh.read(32)
    if _u32(hdr, 0) != MH_MAGIC_64:
        raise ToolError(f"不是 64 位 Mach-O（magic=0x{_u32(hdr, 0):08x}）")
    return [Slice(_u32(hdr, 4), 0, file_size)]


def resolve_va(fh, slice_: Slice, va: int, length: int) -> tuple[int, str]:
    """把 __TEXT 里的虚拟地址换算成文件偏移。返回 (file_offset, segment_name)。"""
    fh.seek(slice_.offset)
    hdr = fh.read(32)
    if len(hdr) < 32 or _u32(hdr, 0) != MH_MAGIC_64:
        raise ToolError(f"[{slice_.arch}] 切片头无效")
    if _u32(hdr, 4) != slice_.cpu:
        raise ToolError(f"[{slice_.arch}] cputype 不一致")

    ncmds = _u32(hdr, 16)
    sizeofcmds = _u32(hdr, 20)
    cmds_start = slice_.offset + 32
    cmds_end = cmds_start + sizeofcmds
    if cmds_end > slice_.offset + slice_.size:
        raise ToolError(f"[{slice_.arch}] load commands 越界")

    fh.seek(cmds_start)
    blob = fh.read(sizeofcmds)
    off = 0
    for _ in range(ncmds):
        if off + 8 > len(blob):
            raise ToolError(f"[{slice_.arch}] load command 截断")
        cmd = _u32(blob, off)
        cmdsize = _u32(blob, off + 4)
        if cmdsize < 8 or off + cmdsize > len(blob):
            raise ToolError(f"[{slice_.arch}] load command 长度非法")
        if cmd == LC_SEGMENT_64 and cmdsize >= 72:
            seg = blob[off + 8: off + 72]
            name = seg[:16].split(b"\0")[0].decode(errors="replace")
            vmaddr = _u64(seg, 16)
            vmsize = _u64(seg, 24)
            fileoff = _u64(seg, 32)
            filesize = _u64(seg, 40)
            if vmaddr <= va < vmaddr + vmsize:
                delta = va - vmaddr
                if delta + length > filesize:
                    raise ToolError(
                        f"[{slice_.arch}] VA 0x{va:x} 落在 {name} 的非文件映射区"
                    )
                return slice_.offset + fileoff + delta, name
        off += cmdsize
    raise ToolError(f"[{slice_.arch}] VA 0x{va:x} 不在任何 segment 内")


def read_at(fh, offset: int, length: int) -> bytes:
    fh.seek(offset)
    data = fh.read(length)
    if len(data) != length:
        raise ToolError(f"读取 {length} 字节失败 @0x{offset:x}")
    return data


# ---------------------------------------------------------------- 补丁点状态

PRISTINE, PATCHED, UNKNOWN = "pristine", "patched", "unknown"


class PatchPoint:
    """一个补丁点在某次具体运行中的解析结果。"""

    def __init__(self, arch, va, file_offset, original, patch, expected):
        self.arch = arch
        self.va = va
        self.file_offset = file_offset
        self.original = original   # 当前磁盘上的字节
        self.patch = patch         # 目标字节
        self.expected = expected   # 允许的原始字节集合

    @property
    def is_modification(self) -> bool:
        """True 表示这个点确实会被本变体改写（目标值不属于任何允许的原始值）。
        个别补丁点的作用只是「还原/保持原样」（例如 keeptip 先把被静默补丁改掉的
        函数序言还原回来），那些点 patch 本身就落在 expected 里。"""
        return self.patch not in self.expected

    @property
    def state(self) -> str:
        if self.patch in self.expected:
            # 本变体要求这里保持/回到原始值
            return PRISTINE if self.original in self.expected else UNKNOWN
        if self.original == self.patch:
            return PATCHED
        if self.original in self.expected:
            return PRISTINE
        return UNKNOWN

    @property
    def needs_write(self) -> bool:
        return self.original != self.patch


def plan_points(fh, file_size: int, slices: list[Slice], entries: list[dict]) -> list[PatchPoint]:
    points: list[PatchPoint] = []
    for entry in entries:
        asm = bytes.fromhex(entry["asm"])
        expected = [bytes.fromhex(x) for x in entry.get("expected", [])]
        if not asm:
            raise ToolError("补丁内容为空")
        if any(len(e) != len(asm) for e in expected):
            raise ToolError(f"0x{entry['addr']} 的 expected 与补丁长度不一致")
        if not expected:
            raise ToolError(
                f"0x{entry['addr']} 没有原始字节校验（expected 为空）—— "
                f"本工具拒绝应用无校验的补丁点"
            )
        va = int(entry["addr"], 16)
        arch = entry.get("arch", "arm64")
        cpu = ARCH_CPU.get(arch)
        matched = False
        for s in slices:
            if s.cpu != cpu:
                continue
            off, _seg = resolve_va(fh, s, va, len(asm))
            cur = read_at(fh, off, len(asm))
            points.append(PatchPoint(arch, va, off, cur, asm, expected))
            matched = True
        if not matched:
            raise ToolError(f"二进制里没有 {arch} 切片，无法应用 0x{va:x}")
    return points


# ---------------------------------------------------------------- 备份 / 还原


def backup_root(override: str | None = None) -> Path:
    if override:
        return Path(override).expanduser()
    return real_user_home() / "Library" / "Application Support" / BACKUP_DIR_NAME


_BACKUP_OVERRIDE: str | None = None


def backups_for(build: str) -> list[Path]:
    root = backup_root(_BACKUP_OVERRIDE) / "Backups"
    if not root.is_dir():
        return []
    return sorted(
        (p for p in root.iterdir() if p.name.startswith(f"WeChat-{build}-") and p.is_dir()),
        key=lambda p: p.name,
        reverse=True,
    )


def make_backup(app: Path, build: str) -> Path:
    root = backup_root(_BACKUP_OVERRIDE) / "Backups"
    root.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    dest = root / f"WeChat-{build}-{stamp}.app"
    info(f"整包备份 → {dest}")
    t0 = time.time()
    rc, out, err = run(["/usr/bin/ditto", str(app), str(dest)])
    if rc != 0:
        raise ToolError(f"备份失败：{err.decode(errors='replace')}")
    # 记一份元数据，便于 restore / 排错
    meta = {
        "created": stamp,
        "build": build,
        "source": str(app),
        "wechat_dylib_sha256_fat": sha256_file(app / "Contents/Resources/wechat.dylib"),
    }
    (dest.parent / f"{dest.name}.json").write_text(
        json.dumps(meta, indent=2, ensure_ascii=False)
    )
    # chown 放在最后，保证 .app 与 .json 都归真实用户所有
    chown_to_user(root)
    chown_to_user(root.parent, recursive=False)  # 否则以后不带 sudo 就写不进这个目录
    ok(f"备份完成（{time.time() - t0:.1f}s，{dir_size(dest) / 1e9:.2f} GB）")
    return dest


def sha256_file(path: Path, chunk: int = 1 << 22) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def sha256_slice(path: Path, arch: str) -> str:
    """把某个架构切片抽出来做 SHA-256（用于和已复核的构建号比对）。"""
    with tempfile.TemporaryDirectory() as td:
        thin = Path(td) / "thin"
        rc, _, err = run(["/usr/bin/lipo", "-thin", arch, "-output", str(thin), str(path)])
        if rc != 0:
            raise ToolError(f"lipo 提取 {arch} 切片失败：{err.decode(errors='replace')}")
        return sha256_file(thin)


def dir_size(path: Path) -> int:
    total = 0
    for root, _dirs, files in os.walk(path):
        for f in files:
            try:
                total += os.lstat(os.path.join(root, f)).st_size
            except OSError:
                pass
    return total


# ---------------------------------------------------------------- 重签名
#
# 微信 4.x 是 App Sandbox + Hardened Runtime，主程序、WeChatHelper、WeChatAppEx
# 以及内部每个 helper 都带腾讯 Team ID 的 entitlements。
#
#   * `codesign --deep --sign -` 会把根 bundle 的 entitlements 剥掉，而嵌套代码
#     还留着 → 开着 SIP 的机器上 amfid 在启动时直接杀掉进程（"闪退"）。
#   * 所以必须：先把每个 code object 的 entitlements 快照下来，给每个非空 profile
#     注入两个 ad-hoc 签名也能生效的权限，并剔除 ad-hoc 签名背不动的受限权限
#     （com.apple.developer.*，App Store 版微信主程序带 team-identifier）。
#   * 再用 --preserve-metadata=identifier,flags,runtime（绝不用 requirements，
#     它绑定原始 Developer ID）分层签名。
#   * 签完复核，漂移的按「最深层优先」显式补签；仍漂移就大声失败。
#
#   --force-library-entitlements 是 macOS 15+ 才有的开关：否则给 dylib 指定
#   entitlements 会被忽略。


def is_code_object(path: Path) -> bool:
    if path.is_dir():
        return path.suffix.lower() in (".app", ".appex", ".bundle", ".framework",
                                       ".plugin", ".service", ".xpc")
    if not path.is_file() or path.is_symlink():
        return False
    if path.suffix.lower() in (".dylib", ".so"):
        return True
    return os.access(path, os.X_OK)


def code_objects(app: Path) -> list[Path]:
    out = [app]
    seen = {str(app)}
    for root, dirs, files in os.walk(app):
        dirs[:] = [d for d in dirs if ".cstemp" not in d]
        for name in list(dirs) + files:
            p = Path(root) / name
            if p.is_symlink():
                continue
            if ".cstemp" in name:
                continue
            if not is_code_object(p):
                continue
            key = str(p)
            if key in seen:
                continue
            seen.add(key)
            out.append(p)
    return out


def inspect_entitlements(path: Path):
    """None = 不是签名代码对象（跳过）；b'' = 已签名但无 entitlements；否则为 plist 字节。"""
    rc, out, _ = run(["/usr/bin/codesign", "-d", "--entitlements", "-", "--xml", str(path)])
    if rc != 0:
        return None
    return out if out.strip() else b""


def inject_entitlements(plist: bytes) -> bytes:
    if not plist:
        return plist
    try:
        d = plistlib.loads(plist)
    except Exception:
        return plist
    if not isinstance(d, dict):
        return plist
    for k, v in INJECTED_ENTITLEMENTS.items():
        d[k] = v
    for k in [k for k in d if k.startswith("com.apple.developer.")]:
        del d[k]
    return plistlib.dumps(d, fmt=plistlib.FMT_XML)


def plists_equal(a: bytes, b: bytes) -> bool:
    try:
        return plistlib.loads(a) == plistlib.loads(b)
    except Exception:
        return a == b


def remove_signing_leftovers(path: Path) -> None:
    targets = []
    if path.is_dir():
        for root, dirs, files in os.walk(path):
            targets += [Path(root) / n for n in dirs + files if ".cstemp" in n]
    else:
        parent = path.parent
        targets = [p for p in parent.glob(path.name + ".cstemp*")] if parent.is_dir() else []
    for t in targets:
        try:
            os.chflags(t, 0)
        except OSError:
            pass
        try:
            if t.is_dir():
                shutil.rmtree(t, ignore_errors=True)
            else:
                t.unlink()
            info(f"清理残留签名临时文件 {t.name}")
        except OSError:
            pass


def _codesign(url: Path, deep: bool, entitlements: bytes | None) -> None:
    args = ["/usr/bin/codesign", "--force"]
    if deep:
        args.append("--deep")
    args.append("--preserve-metadata=identifier,flags,runtime")
    args += ["--sign", "-"]
    tmpdir = None
    try:
        if entitlements:
            tmpdir = tempfile.mkdtemp(prefix="wxar-ent-")
            ent = Path(tmpdir) / "entitlements.plist"
            ent.write_bytes(entitlements)
            if int(platform.mac_ver()[0].split(".")[0] or 0) >= 15:
                args.append("--force-library-entitlements")
            args += ["--entitlements", str(ent)]
        args.append(str(url))
        rc, out, err = run(args)
        if rc != 0:
            remove_signing_leftovers(url)
            raise ToolError(
                f"codesign 失败：{' '.join(args)}\n"
                f"{(out + err).decode(errors='replace').strip()}"
            )
    finally:
        if tmpdir:
            shutil.rmtree(tmpdir, ignore_errors=True)


def sign_code_object(url: Path, entitlements: bytes | None) -> None:
    try:
        _codesign(url, deep=False, entitlements=entitlements)
    except ToolError:
        # 只读/被锁的文件：签一份临时副本再 cp -p 回来
        tmpdir = tempfile.mkdtemp(prefix="wxar-sign-")
        try:
            tmp = Path(tmpdir) / url.name
            if url.is_dir():
                shutil.copytree(url, tmp, symlinks=True)
            else:
                shutil.copy2(url, tmp)
            _codesign(tmp, deep=False, entitlements=entitlements)
            rc, _, err = run(["/bin/cp", "-p", str(tmp), str(url)])
            if rc != 0:
                raise ToolError(f"回写签名副本失败：{err.decode(errors='replace')}")
        finally:
            shutil.rmtree(tmpdir, ignore_errors=True)


def resign(app: Path, patched_binaries: list[Path]) -> None:
    remove_signing_leftovers(app)

    info("快照所有 code object 的 entitlements …")
    snapshot: list[tuple[Path, bytes | None]] = []
    for cand in code_objects(app):
        cur = inspect_entitlements(cand)
        if cur is None:
            continue
        snapshot.append((cand, inject_entitlements(cur) if cur else cur))
    with_ent = sum(1 for _, p in snapshot if p)
    info(f"共 {len(snapshot)} 个签名对象，其中 {with_ent} 个带 entitlements")

    wanted = {str(p): e for p, e in snapshot}

    # 1) 先签被改过的嵌套二进制，让它在 bundle 级 --deep 之前就有合法 ad-hoc 签名
    for b in patched_binaries:
        if str(b) == str(app):
            continue
        sign_code_object(b, wanted.get(str(b)))

    # 2) 根 bundle：不带 --entitlements，避免把根 profile 也盖到本来没有的嵌套代码上
    _codesign(app, deep=True, entitlements=None)

    # 3) 复核；漂移的按最深层优先显式补签
    drifted = check_drift(snapshot)
    if drifted:
        warn(f"{len(drifted)} 个对象的 entitlements 在 --deep 之后漂移，正在显式补签")
        restore_entitlements(snapshot)
        drifted = check_drift(snapshot)
        if drifted:
            raise ToolError(
                "重签名后仍有对象丢失/改变了 entitlements（sandbox / 摄像头 / 麦克风 / "
                "app-group 配置），拒绝报告成功：\n  " + "\n  ".join(drifted)
            )

    # 4) 只清 com.apple.quarantine（绝不能 xattr -c：codesign 把非 Mach-O 文件的
    #    签名存在 com.apple.cs.* 扩展属性里，清掉会变成“未签名”）
    run(["/usr/bin/xattr", "-dr", "com.apple.quarantine", str(app)])

    # 5) 最后再校验，确保留在磁盘上的就是这个状态
    rc, out, err = run(
        ["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)]
    )
    if rc != 0:
        raise ToolError(
            "codesign --verify --deep --strict 未通过：\n"
            f"{(out + err).decode(errors='replace').strip()}"
        )
    ok("codesign --verify --deep --strict 通过")


def check_drift(snapshot) -> list[str]:
    bad = []
    for path, want in snapshot:
        cur = inspect_entitlements(path)
        if cur is None:
            bad.append(str(path))
            continue
        if want and cur and plists_equal(want, cur):
            continue
        if not want and not cur:
            continue
        bad.append(str(path))
    return sorted(bad)


def restore_entitlements(snapshot) -> None:
    profiled = [str(p) for p, e in snapshot if e]
    todo = [
        (p, e)
        for p, e in snapshot
        if e or any(q.startswith(str(p) + "/") for q in profiled)
    ]
    todo.sort(key=lambda pe: len(pe[0].parts), reverse=True)
    for path, ent in todo:
        sign_code_object(path, ent)





# ---------------------------------------------------------------- 微信 App 信息


def app_info(app: Path) -> dict:
    plist = app / "Contents/Info.plist"
    if not plist.is_file():
        raise ToolError(f"{app} 看起来不是微信 App（缺 Contents/Info.plist）")
    with open(plist, "rb") as f:
        d = plistlib.load(f)
    return {
        "path": app,
        "bundle_id": d.get("CFBundleIdentifier", ""),
        "short_version": d.get("CFBundleShortVersionString", ""),
        "build": str(d.get("CFBundleVersion", "")),
        "executable": d.get("CFBundleExecutable", "WeChat"),
        "dylib": app / "Contents/Resources/wechat.dylib",
    }


def wechat_running(app: Path) -> list[str]:
    rc, out, _ = run(["/usr/bin/pgrep", "-fl", str(app)])
    if rc != 0 or not out.strip():
        return []
    me = os.getpid()
    hits = []
    for line in out.decode(errors="replace").splitlines():
        parts = line.strip().split(None, 1)
        if not parts:
            continue
        try:
            pid = int(parts[0])
        except ValueError:
            continue
        if pid == me:
            continue
        hits.append(line.strip())
    return hits


def ensure_can_modify(app: Path) -> None:
    """修改 /Applications 下的 App 需要 root；工作区里的副本则不需要。"""
    if os.geteuid() == 0:
        return
    if os.access(app, os.W_OK) and os.access(app.parent, os.W_OK):
        return
    die(f"需要 root：{app} 当前用户不可写（要改 App 并重签名，请用 sudo 运行）")


def app_management_help(path: Path, app: Path) -> str:
    return f"""无法写入 {path}
（Operation not permitted）

这多半不是普通权限问题，而是 macOS 的「App 管理」保护：凡是 root 拥有的
App bundle，任何进程（包括 root）都不能修改，除非该进程已被授权。

先确认是不是这个原因 —— 在终端里跑：

    python3 -c "open('{path}','r+b').close(); print('OK')"

若报 Operation not permitted，就是这个原因。解决办法任选其一：

  A. 系统设置 → 隐私与安全性 → App 管理 → 把「终端」打开
     （列表里没有就点左下角 + ，添加 /System/Applications/Utilities/Terminal.app）
     改完必须完全退出终端再重新打开，然后重跑本命令。

  B. 把 App 所有权改成自己，直接绕开这个授权：

       sudo chown -R "$USER":staff "{app}"

     然后不要再加 sudo，重跑本命令。

详见 README 的「常见问题」。"""


def ensure_writable_binary(path: Path, app: Path) -> None:
    """真正试着以读写方式打开目标文件。

    Unix 权限位看不出「App 管理」这类 System Policy 拦截 —— 即使 euid=0、
    os.access() 返回 True，open(..., 'r+b') 仍可能是 EPERM。只有真开一次才知道。
    打开本身不写入任何字节，不会改动文件、不会破坏签名。
    """
    try:
        with open(path, "r+b"):
            pass
    except PermissionError:
        die(app_management_help(path, app))
    except OSError as e:
        die(f"无法打开 {path} 进行写入：{e}")


def ensure_not_running(app: Path) -> None:
    hits = wechat_running(app)
    if hits:
        raise ToolError(
            "请先完全退出微信（含 WeChatAppEx / WeChatHelper），当前仍在运行：\n  "
            + "\n  ".join(hits)
        )


# ---------------------------------------------------------------- 补丁点选择


def load_db() -> dict:
    if not DB_PATH.is_file():
        raise ToolError(f"找不到补丁数据库：{DB_PATH}")
    with open(DB_PATH, "r", encoding="utf-8") as f:
        return json.load(f)


VARIANT_TARGET = {
    "silent": "revoke",
    "keeptip": "revoke-keeptip",
    "keeptip-content": "revoke-keeptip-content",
}


def short_hex(b: bytes) -> str:
    h = b.hex()
    return h if len(h) <= 16 else h[:16] + "…"


def extra_files(app: Path, target: dict) -> list[tuple[Path, Path]]:
    """补丁要求一并放进 App 的文件：[(源, 目标)]。源文件必须与登记的 SHA-256 一致。"""
    out = []
    for f in target.get("files", []):
        src = Path(__file__).resolve().parent / f["src"]
        if not src.is_file():
            raise ToolError(f"缺少补丁所需文件：{src}")
        got = sha256_file(src)
        if got != f["sha256"]:
            raise ToolError(f"{src} 的 SHA-256 与补丁库登记的不一致（{got}），拒绝安装")
        out.append((src, app / f["dst"]))
    return out


def arm64_uuid(path: Path) -> str | None:
    """arm64 切片的 LC_UUID：重签名不会改它，重新编译才会。读不出来返回 None。"""
    try:
        with open(path, "rb") as f:
            for s in parse_slices(f, path.stat().st_size):
                if s.arch != "arm64":
                    continue
                hdr = read_at(f, s.offset, 32)
                blob = read_at(f, s.offset + 32, _u32(hdr, 20))
                off = 0
                for _ in range(_u32(hdr, 16)):
                    cmd, size = _u32(blob, off), _u32(blob, off + 4)
                    if cmd == LC_UUID:
                        return blob[off + 8: off + 24].hex()
                    if size < 8:
                        break
                    off += size
    except (OSError, ToolError, struct.error, IndexError):
        pass
    return None


def file_outdated(src: Path, dst: Path) -> bool:
    """App 里的附带文件缺失、版本不同或签名损坏，都要重新安装。"""
    if not dst.is_file():
        return True
    want = arm64_uuid(src)
    if want is None or arm64_uuid(dst) != want:
        return True
    rc, _, _ = run(["/usr/bin/codesign", "--verify", "--strict", str(dst)])
    return rc != 0


def signature_error(app: Path) -> str:
    """签名有效返回空串，否则返回 codesign 的报错。"""
    rc, out, err = run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)])
    return "" if rc == 0 else ((out + err).decode(errors="replace").strip() or "未知错误")


def has_backup_of(app: Path, build: str) -> bool:
    """是否已有这个 App（按路径认，不只看构建号）的备份。"""
    for b in backups_for(build):
        try:
            meta = json.loads((b.parent / f"{b.name}.json").read_text())
        except (OSError, ValueError):
            continue
        if meta.get("source") == str(app):
            return True
    return False


def clean_staging_leftovers(files) -> None:
    """清掉上次中断留下的临时文件，避免被签进包里。"""
    for _src, dst in files:
        new, old = (dst.with_name(dst.name + x) for x in (".wxar-new", ".wxar-old"))
        try:
            if old.exists() and not dst.exists():
                os.replace(old, dst)
            old.unlink(missing_ok=True)
            new.unlink(missing_ok=True)
        except OSError as e:
            warn(f"清理残留临时文件失败：{e}")


def variant_targets(db: dict, build: str, variant: str, multi_instance: bool):
    builds = db.get("builds", {})
    if build not in builds:
        raise ToolError(
            f"补丁库没有构建号 {build}。\n"
            f"  可选：{'、'.join(sorted(builds, key=lambda x: -int(x)) if builds else [])}"
        )
    targets = builds[build].get("targets", {})
    want = []
    revoke_id = VARIANT_TARGET[variant]
    if revoke_id not in targets:
        have = [v for v, t in VARIANT_TARGET.items() if t in targets]
        raise ToolError(f"构建 {build} 不支持 {variant}，可以用：{'、'.join(have) or '无'}（--variant 指定）")
    want.append(revoke_id)
    siblings = [t for v, t in VARIANT_TARGET.items() if v != variant]
    if multi_instance:
        if "multiInstance" not in targets:
            raise ToolError(f"构建 {build} 没有 multiInstance 补丁点")
        want.append("multiInstance")
    return targets, want, siblings


# ---------------------------------------------------------------- 命令实现


def cmd_info(args) -> None:
    app = Path(args.app)
    ai = app_info(app)
    print(f"微信 App      : {ai['path']}")
    print(f"bundle id     : {ai['bundle_id']}")
    print(f"版本 / 构建号 : {ai['short_version']} / {ai['build']}")

    if not ai["dylib"].is_file():
        die(f"缺少 {ai['dylib']}")
    print(f"wechat.dylib  : {ai['dylib'].stat().st_size / 1e6:.0f} MB (fat)")

    db = load_db()
    v = db.get("verified", {}).get(ai["build"])
    if v:
        info("该构建号已被独立复核，正在核对 arm64 切片 SHA-256 …")
        got = sha256_slice(ai["dylib"], v["arch"])
        if got == v["slice_sha256"]:
            ok(f"{v['arch']} 切片 SHA-256 匹配（{got[:16]}…）")
        else:
            warn(f"{v['arch']} 切片 SHA-256 不匹配：\n    期望 {v['slice_sha256']}\n    实际 {got}")
    else:
        warn(f"补丁库未独立复核构建号 {ai['build']}，仅能依赖逐点字节校验")

    builds = db.get("builds", {})
    if ai["build"] not in builds:
        warn(f"补丁库没有 {ai['build']} 的补丁点")
        return

    print()
    info("补丁点现状：")
    fh = open(ai["dylib"], "rb")
    try:
        size = os.fstat(fh.fileno()).st_size
        slices = parse_slices(fh, size)
        for tid, t in builds[ai["build"]]["targets"].items():
            print(f"  {C_DIM}[{tid}]{C_OFF} {t.get('binary','')}")
            for p in plan_points(fh, size, slices, t["entries"]):
                color = {PRISTINE: C_YELLOW, PATCHED: C_GREEN, UNKNOWN: C_RED}[p.state]
                label = {
                    PRISTINE: "待修改（当前为原始字节）",
                    PATCHED: "已符合目标状态",
                    UNKNOWN: "未知字节（apply 会拒绝并中止）",
                }[p.state]
                same = "" if p.is_modification else " (本变体保持/还原此点)"
                print(
                    f"    0x{p.va:<9x} {p.arch:<7} 当前 {short_hex(p.original)} "
                    f"→ 目标 {short_hex(p.patch)}  {color}{label}{C_OFF}{same}"
                )
    finally:
        fh.close()

    rc, _, _ = run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)])
    print()
    if rc == 0:
        ok("当前签名校验通过")
    else:
        warn("当前签名校验未通过（可能已被改过但没重签）")


def cmd_apply(args) -> None:
    global _BACKUP_OVERRIDE
    _BACKUP_OVERRIDE = args.backup_dir
    app = Path(args.app)
    ai = app_info(app)
    build = ai["build"]

    db = load_db()
    if build not in db.get("builds", {}):
        die(f"补丁库没有构建号 {build}，拒绝操作")

    v = db.get("verified", {}).get(build)
    if v:
        info(f"核对 {v['arch']} 切片 SHA-256 …")
        got = sha256_slice(ai["dylib"], v["arch"])
        if got == v["slice_sha256"]:
            ok("与已独立复核的官方二进制完全一致")
        else:
            warn("切片 SHA-256 与已复核的官方二进制不一致。")
            warn("  常见原因：这份微信已经被本工具打过补丁（改字节必然改哈希）。")
            warn("  继续以逐点字节校验为准——只要有任何一个补丁点字节不是已知状态，立即中止。")
    else:
        warn(f"构建号 {build} 未经独立复核，仅依赖逐点字节校验")

    targets, want, siblings = variant_targets(db, build, args.variant, args.multi_instance)

    # ---- 打开并规划（只读，任何校验失败都在写之前中止）
    binaries: dict[Path, list[PatchPoint]] = {}
    sibling_points: list[PatchPoint] = []
    fh = open(ai["dylib"], "rb")
    try:
        size = os.fstat(fh.fileno()).st_size
        slices = parse_slices(fh, size)
        for tid in want:
            t = targets[tid]
            binpath = app / t.get("binary", "Contents/Resources/wechat.dylib")
            binaries.setdefault(binpath, []).extend(
                plan_points(fh, size, slices, t["entries"])
            )
        for sib in (targets[t] for t in siblings if t in targets):
            sibpath = app / sib.get("binary", "Contents/Resources/wechat.dylib")
            if sibpath == ai["dylib"]:
                sibling_points += plan_points(fh, size, slices, sib["entries"])
    finally:
        fh.close()

    # ★ 安全闸：任何补丁点的当前字节既不是「允许的原始字节」也不是「目标字节」，
    #   说明这不是我们认识的那份 wechat.dylib —— 整体中止，绝不盲写。
    unknown = [
        p for pl in binaries.values() for p in pl if p.state == UNKNOWN
    ]
    if unknown:
        die(
            "以下补丁点的字节不是已知状态，拒绝写入：\n  "
            + "\n  ".join(
                f"0x{p.va:x} [{p.arch}] 当前 {short_hex(p.original)} "
                f"(允许的原始值: {'/'.join(short_hex(e) for e in p.expected)}，"
                f"目标值: {short_hex(p.patch)})"
                for p in unknown
            )
            + "\n这通常意味着微信构建号与补丁库不匹配，或文件已被其它工具改过。"
        )

    # 变体互斥检查：另一个变体独占的补丁点必须是原始状态
    want_vas = {(str(binpath), p.va) for binpath, pl in binaries.items() for p in pl}
    leftover = [
        p for p in sibling_points
        if (str(ai["dylib"]), p.va) not in want_vas and p.state != PRISTINE
    ]
    if leftover:
        die(
            "另一个变体的补丁点不是原始字节（上次装的是别的变体，或被其它工具改过），"
            "拒绝在混合状态下继续：\n  "
            + "\n  ".join(f"0x{p.va:x} 当前 {short_hex(p.original)}" for p in leftover)
            + "\n请先运行 restore 还原，再重新 apply。"
        )

    files = [x for tid in want for x in extra_files(app, targets[tid])]
    new_files = [(a, b) for a, b in files if file_outdated(a, b)]
    sig_err = signature_error(app)

    # ---- 汇总计划
    to_write = [(pl, p) for pl in binaries.values() for p in pl if p.needs_write]
    already = [(pl, p) for pl in binaries.values() for p in pl if not p.needs_write]

    print()
    info(f"变体：{C_GREEN}{args.variant}{C_OFF}"
         + ("  + multiInstance" if args.multi_instance else ""))
    for pl, p in to_write:
        print(f"  改 0x{p.va:<9x} {p.arch:<7} {short_hex(p.original)} → {short_hex(p.patch)}")
    for pl, p in already:
        print(f"  {C_DIM}已是目标状态，跳过 0x{p.va:x}{C_OFF}")
    for _src, dst in new_files:
        print(f"  {'换' if dst.exists() else '加'} {dst.relative_to(app)}")
    if not to_write and not new_files:
        if not sig_err:
            ok("所有补丁点都已是目标状态，无需改动")
            return
        warn("补丁点都已是目标状态，但当前签名无效（可能上次中断了），将重新签名")

    if args.dry_run:
        ok("--dry-run：已校验通过，未写入任何字节")
        return

    ensure_can_modify(app)
    ensure_not_running(app)
    # 关键：真正试开一次，把 macOS「App 管理」这类权限位看不出的拦截提前暴露出来，
    # 而不是等到写完一半才报错
    ensure_writable_binary(ai["dylib"], app)

    if not args.yes:
        print()
        ans = input(f"确认修改 {app} 并重签名？(y/N) ").strip().lower()
        if ans not in ("y", "yes"):
            die("已取消", 0)

    # 各变体的附带文件都查一遍：上次中断的可能是另一个变体
    clean_staging_leftovers([
        (None, app / f["dst"]) for t in targets.values() for f in t.get("files", [])
    ])
    if sig_err:  # 残留文件本身就会让签名失效，清理后重新判断
        sig_err = signature_error(app)

    # ---- 备份
    if not args.no_backup and sig_err:
        # 签名已坏的 App 不能成为「最新备份」，否则 restore 会把坏的还原回来；
        # 但也不能在一份备份都没有的情况下动手
        if not has_backup_of(app, build):
            die(f"{app} 当前签名无效，且没有它的备份，拒绝修改：\n{sig_err}\n"
                "若提示 detritus / FinderInfo，多半是 Finder 颜色标签或自定义图标，去掉后再试；"
                "否则请先重装微信。")
        warn("当前签名无效，跳过备份（沿用之前的备份）")
    elif not args.no_backup:
        make_backup(app, build)
    else:
        warn("--no-backup：未做整包备份，出问题只能手工修复")

    # ---- 事务式写入：先把附带文件放成临时文件，再改字节，最后一步改名就位
    info("写入补丁字节 …")
    written: list[tuple[PatchPoint, bytes]] = []
    # 附带文件：tmp=临时文件 dst=目标 old=旧文件暂存 had=原来就有
    staged: list[dict] = []
    try:
        for src, dst in new_files:
            f = {"tmp": dst.with_name(dst.name + ".wxar-new"), "dst": dst,
                 "old": dst.with_name(dst.name + ".wxar-old"),
                 "had": dst.exists()}
            staged.append(f)
            shutil.copyfile(src, f["tmp"])
            os.chmod(f["tmp"], 0o755)
        with open(ai["dylib"], "r+b") as wf:
            for _pl, p in to_write:
                written.append((p, p.original))
                wf.seek(p.file_offset)
                wf.write(p.patch)
            wf.flush()
            os.fsync(wf.fileno())
            # 回读确认
            for _pl, p in to_write:
                wf.seek(p.file_offset)
                if wf.read(len(p.patch)) != p.patch:
                    raise ToolError(f"回读校验失败 @0x{p.va:x}")
        for f in staged:
            if f["had"]:
                os.replace(f["dst"], f["old"])
            os.replace(f["tmp"], f["dst"])
    except BaseException as e:  # 含 Ctrl-C：字节改了却没重签，微信会打不开，必须回滚
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        warn(f"写入中断（{str(e) or type(e).__name__}），正在回滚 …")
        failed = []
        try:
            if written:
                with open(ai["dylib"], "r+b") as wf:
                    for p, orig in written:
                        wf.seek(p.file_offset)
                        wf.write(orig)
                    wf.flush()
                    os.fsync(wf.fileno())
        except OSError as e2:
            failed.append(e2)
        for f in staged:
            try:
                if f["old"].exists():
                    os.replace(f["old"], f["dst"])
                elif not f["had"]:
                    f["dst"].unlink(missing_ok=True)
                f["tmp"].unlink(missing_ok=True)
            except OSError as e2:
                failed.append(e2)
        if failed:
            for e2 in failed:
                warn(f"回滚失败：{e2}")
            die("写入失败，且未能完全回滚。请重跑同一条命令，或用 restore 从备份还原")
        ok("已回滚全部改动")
        die("写入失败，微信已恢复到写入前的状态")
    # 从这里到重签名完成之间不响应 Ctrl-C
    old_sigint = signal.signal(signal.SIGINT, signal.SIG_IGN)
    for f in staged:
        try:
            f["old"].unlink(missing_ok=True)
        except OSError as e:
            warn(f"清理 {f['old'].name} 失败：{e}")
        ok(f"已放入 {f['dst'].relative_to(app)}")
    if to_write:
        ok(f"写入 {len(to_write)} 处并回读校验通过")

    # ---- 重签名
    info("重签名（保留 entitlements）…")
    try:
        # 附带文件可能是刚放进来的，先签它，再签 wechat.dylib
        resign(app, [dst for _src, dst in files] + [ai["dylib"]])
    except ToolError as e:
        print()
        warn("重签名失败。可以从备份还原：")
        for b in backups_for(build)[:1]:
            warn(f"  sudo python3 {Path(__file__).name} restore --backup {b}")
        die(str(e))
    finally:
        signal.signal(signal.SIGINT, old_sigint)

    print()
    ok("完成。现在可以打开微信。")
    print(f"  验证方式：让对方发一条消息后撤回，消息应当仍然留在聊天里。")
    print(f"  当前变体：{args.variant}" + {
        "silent": "（静默：不显示撤回提示）",
        "keeptip": "（留提示：私聊显示提示，群聊为静默）",
        "keeptip-content": "（留提示并显示内容：私聊有效，只认本次启动后收到或加载过的消息）",
    }[args.variant])


def cmd_restore(args) -> None:
    global _BACKUP_OVERRIDE
    _BACKUP_OVERRIDE = args.backup_dir
    app = Path(args.app)
    ai = app_info(app)
    build = ai["build"]

    target = Path(args.backup) if args.backup else None
    if target is None:
        cands = backups_for(build)
        if not cands:
            die(f"没有构建号 {build} 的备份。可手工从 Time Machine / App Store 重装。")
        target = cands[0]
    if not target.is_dir():
        die(f"备份不存在：{target}")

    print(f"将用备份覆盖 {app}")
    print(f"  备份：{target}")
    if not args.yes:
        ans = input("确认还原？(y/N) ").strip().lower()
        if ans not in ("y", "yes"):
            die("已取消", 0)

    ensure_can_modify(app)
    ensure_not_running(app)
    # 替换 App 同样受「App 管理」保护 —— 提前试开一次，顺便给出准确指引
    probe = next((p for p in app.rglob("Info.plist") if "Contents" in str(p)), None)
    if probe:
        ensure_writable_binary(probe, app)
    staging = app.with_name(app.name + ".restore-staging")
    if staging.exists():
        shutil.rmtree(staging)
    info("复制备份到临时目录 …")
    rc, _, err = run(["/usr/bin/ditto", str(target), str(staging)])
    if rc != 0:
        die(f"复制备份失败：{err.decode(errors='replace')}")
    info("替换 App …")
    old = app.with_name(app.name + ".replaced")
    if old.exists():
        shutil.rmtree(old)
    os.rename(app, old)
    try:
        os.rename(staging, app)
    except OSError as e:
        warn(f"替换失败（{e}），正在还原原 App")
        os.rename(old, app)
        shutil.rmtree(staging, ignore_errors=True)
        die("还原失败路径已回滚，微信保持原样")
    if old.exists() and not shutil.rmtree(old, ignore_errors=True) and old.exists():
        warn(f"旧 App 未能删除，残留在 {old}（系统保护所致）")
        warn(f"  确认新 App 能正常启动后，可执行： sudo rm -rf \"{old}\"")
    ok(f"已还原为备份 {target.name} 的状态")
    print("  用 info 可查看它打了哪些补丁；要回到官方原版，请用 --backup 指定打补丁前的备份。")


def cmd_verify(args) -> None:
    app = Path(args.app)
    ai = app_info(app)
    rc, out, err = run(
        ["/usr/bin/codesign", "--verify", "--deep", "--strict", "--verbose=2", str(app)]
    )
    text = (out + err).decode(errors="replace").strip()
    if rc == 0:
        ok(f"签名校验通过（{ai['short_version']} / {ai['build']}）")
    else:
        warn("签名校验未通过：")
        print(text)
        sys.exit(2)


def cmd_list(args) -> None:
    db = load_db()
    verified = db.get("verified", {})
    print(f"{'构建号':<10} {'状态':<12} 补丁点")
    print("-" * 70)
    for b in sorted(db.get("builds", {}), key=lambda x: -int(x)):
        tgts = "、".join(db["builds"][b].get("targets", {}))
        mark = f"{C_GREEN}已复核{C_OFF}" if b in verified else f"{C_DIM}未复核{C_OFF}"
        print(f"{b:<10} {mark:<21} {tgts}")


# ---------------------------------------------------------------- 入口


def main() -> None:
    p = argparse.ArgumentParser(
        prog="wechat_antirecall",
        description="macOS 微信 4.x 防撤回补丁工具（原地字节补丁 + 保留 entitlements 重签名）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("用法\n----")[-1],
    )
    p.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_app(sp):
        sp.add_argument("--app", default=DEFAULT_APP, help=f"微信 App 路径（默认 {DEFAULT_APP}）")

    sp = sub.add_parser("info", help="查看版本、切片哈希与补丁点现状（只读）")
    add_app(sp)
    sp.set_defaults(func=cmd_info)

    sp = sub.add_parser("apply", help="打补丁")
    add_app(sp)
    sp.add_argument("--variant", choices=list(VARIANT_TARGET), default="silent",
                    help="silent=消息留住且无提示（默认）；keeptip=消息留住且显示撤回提示；"
                         "keeptip-content=同 keeptip，且提示里显示撤回的内容")
    sp.add_argument("--multi-instance", action="store_true", help="同时打客户端多开补丁")
    sp.add_argument("--dry-run", action="store_true", help="只校验不写入")
    sp.add_argument("--no-backup", action="store_true", help="跳过整包备份（不推荐）")
    sp.add_argument("--backup-dir", help="备份目录（默认 ~/Library/Application Support/WeChatAntiRecall）")
    sp.add_argument("--yes", action="store_true", help="跳过确认")
    sp.set_defaults(func=cmd_apply)

    sp = sub.add_parser("restore", help="从备份还原")
    add_app(sp)
    sp.add_argument("--backup", help="指定备份目录（默认取最新一份）")
    sp.add_argument("--backup-dir", help="备份根目录（与 apply 一致）")
    sp.add_argument("--yes", action="store_true", help="跳过确认")
    sp.set_defaults(func=cmd_restore)

    sp = sub.add_parser("verify", help="校验微信 App 签名")
    add_app(sp)
    sp.set_defaults(func=cmd_verify)

    sp = sub.add_parser("list", help="列出补丁库收录的构建号")
    sp.set_defaults(func=cmd_list)

    args = p.parse_args()
    try:
        args.func(args)
    except ToolError as e:
        die(str(e))
    except KeyboardInterrupt:
        die("已中断", 130)


if __name__ == "__main__":
    main()
