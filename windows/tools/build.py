#!/usr/bin/env python3
"""从官方 Weixin.dll 生成补丁（开发用，普通用户不需要）。

用法：python3 build.py <官方 Weixin.dll> [--out 打好补丁的 DLL]

做三件事：编译 runtime/tip.c → 把代码放进新增的两个节 → 改两处指令，写出 patches/<版本>.json。
补丁是确定的：原文件与结果文件的 SHA-256 都登记在 JSON 里，安装时两头都核对。
"""
import argparse, base64, hashlib, json, pathlib, struct, subprocess, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent

TARGET = {
    "version": "4.2.0.53",
    "sha256": "ac6b174d98ee1b69e95c99a7a39c3197c8c167b7c6d32f5b72c7a7659c104825",
    # 被调用的微信函数
    "symbols": {"wx_realloc_assign": 0x1BD90},
    # (RVA, 原始字节, call 到哪个桩, 后面补什么)
    "calls": [
        # mov byte [rsi+0x2c0], 1
        (0xA8B286, "c686c002000001", "thunk_fin", "9090"),
        # mov al,1; cmp byte [rbp-0x20],0; je 22e38cd  →  call; je 22e38cd; nop
        (0x22E38B9, "b001807de000740c", "thunk_parse", "740d90"),
    ],
}

CFLAGS = ("--target=x86_64-pc-windows-msvc -std=c11 -Os -ffreestanding -fno-builtin -fno-jump-tables "
          "-fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector -mno-red-zone "
          "-fno-vectorize -fno-slp-vectorize -fno-ident -fno-addrsig -Wall -Wextra -Werror").split()


def align(v, a):
    return (v + a - 1) & ~(a - 1)


def compile_obj():
    with tempfile.TemporaryDirectory() as t:
        out = pathlib.Path(t) / "tip.obj"
        subprocess.run(["clang", *CFLAGS, "-c", str(ROOT / "runtime/tip.c"), "-o", str(out)], check=True)
        return out.read_bytes()


def link(obj, code_rva, symbols):
    """把 COFF 目标文件排成一段代码 + 一段未初始化数据。返回 (代码, bss 大小, 导出符号)。"""
    machine, nsec, _, symoff, nsym, opt, _ = struct.unpack_from("<HHIIIHH", obj, 0)
    assert machine == 0x8664
    strtab = symoff + 18 * nsym
    secs = []
    for i in range(nsec):
        o = 20 + opt + 40 * i
        name = obj[o:o + 8].rstrip(b"\0").decode()
        if name.startswith("/"):  # 长节名在字符串表里
            q = strtab + int(name[1:])
            name = obj[q:obj.index(b"\0", q)].decode()
        _, _, size, raw, rel, _, nrel, _, ch = struct.unpack_from("<IIIIIIHHI", obj, o + 8)
        secs.append(dict(name=name, size=size, raw=raw, rel=rel, nrel=nrel, ch=ch, align=1 << max(0, ((ch >> 20) & 15) - 1)))
    code, bss, bss_align = bytearray(), 0, 16
    order = [s for s in secs if s["name"] == ".text"] + [s for s in secs if s["name"].startswith(".rdata")]
    for s in order:
        while len(code) % max(s["align"], 1):
            code.append(0xCC)
        s["at"] = code_rva + len(code)
        code += obj[s["raw"]:s["raw"] + s["size"]]
    for s in secs:
        if s["name"] == ".bss" and s["size"]:
            bss = align(bss, max(s["align"], bss_align))
            s["bss"] = bss
            bss += s["size"]
        elif s["name"] not in (".text", ".bss") and not s["name"].startswith(".rdata"):
            assert s["size"] == 0 or s["name"] in (".llvm_addrsig", ".drectve", ".debug$S"), s["name"]
    data_rva = align(code_rva + len(code), 0x1000)
    for s in secs:
        if "bss" in s:
            s["at"] = data_rva + s["bss"]
    syms, i = [], 0
    while i < nsym:
        o = symoff + 18 * i
        if obj[o:o + 4] == b"\0\0\0\0":
            p = strtab + struct.unpack_from("<I", obj, o + 4)[0]
            name = obj[p:obj.index(b"\0", p)].decode()
        else:
            name = obj[o:o + 8].rstrip(b"\0").decode()
        value, sec, _, _, aux = struct.unpack_from("<IhHBB", obj, o + 8)
        syms.append((name, value, sec))
        syms += [None] * aux
        i += 1 + aux
    def addr(k):
        name, value, sec = syms[k]
        if sec > 0:
            return secs[sec - 1]["at"] + value
        return symbols[name]
    for s in order:
        for r in range(s["nrel"]):
            va, k, t = struct.unpack_from("<IIH", obj, s["rel"] + 10 * r)
            assert 4 <= t <= 9, f"不支持的重定位类型 {t}"  # 只允许相对寻址，代码才能放在任意位置
            at = s["at"] + va
            p = at - code_rva
            add = struct.unpack_from("<i", code, p)[0]
            struct.pack_into("<i", code, p, addr(k) + add - (at + 4 + (t - 4)))
    exports = {n: secs[sec - 1]["at"] + v for n, v, sec in (x for x in syms if x)
               if sec > 0 and "at" in secs[sec - 1]}
    return bytes(code), data_rva, bss, exports


def checksum(data, off):
    """PE 校验和：16 位反码累加 + 文件长度（校验和字段自身按 0 算）。"""
    d = bytearray(data)
    d[off:off + 4] = b"\0\0\0\0"
    if len(d) % 2:
        d.append(0)
    total = sum(struct.unpack(f"<{len(d) // 2}H", d))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return total + len(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("--out")
    a = ap.parse_args()
    src = pathlib.Path(a.dll).read_bytes()
    sha = hashlib.sha256(src).hexdigest()
    assert sha == TARGET["sha256"], f"不是登记的官方文件：{sha}"

    pe = struct.unpack_from("<I", src, 0x3C)[0]
    nsec, optsize = struct.unpack_from("<H", src, pe + 6)[0], struct.unpack_from("<H", src, pe + 20)[0]
    opt = pe + 24
    sec_align, file_align = struct.unpack_from("<II", src, opt + 32)
    size_image, size_headers = struct.unpack_from("<II", src, opt + 56)
    table = opt + optsize
    secs = [struct.unpack_from("<8sIIIIIIHHI", src, table + 40 * i) for i in range(nsec)]
    def off(rva):
        for _, vs, va, rs, ro, *_ in secs:
            if va <= rva < va + rs:
                return ro + rva - va
        raise ValueError(hex(rva))

    end = table + 40 * nsec
    assert end + 80 <= size_headers and not any(src[end:end + 80]), "节表后面没有空位"
    code_rva = align(size_image, sec_align)
    code, data_rva, bss, exports = link(compile_obj(), code_rva, TARGET["symbols"])
    raw_off, raw = align(len(src), file_align), code + b"\0" * (align(len(code), file_align) - len(code))

    edits = []
    def edit(o, new, old=None):
        cur = src[o:o + len(new)]
        assert old is None or cur == old, f"0x{o:x}: {cur.hex()}"
        edits.append((o, cur, new))
    for rva, old, sym, tail in TARGET["calls"]:
        old = bytes.fromhex(old)
        new = b"\xe8" + struct.pack("<i", exports[sym] - (rva + 5)) + bytes.fromhex(tail)
        assert len(new) == len(old)
        edit(off(rva), new, old)
    edit(end, struct.pack("<8sIIIIIIHHI", b".wxar", len(code), code_rva, len(raw), raw_off, 0, 0, 0, 0, 0x60000020)
         + struct.pack("<8sIIIIIIHHI", b".wxad", bss, data_rva, 0, 0, 0, 0, 0, 0, 0xC0000080))
    edit(opt + 56, struct.pack("<I", align(data_rva + bss, sec_align)))
    edit(pe + 6, struct.pack("<H", nsec + 2))

    out = bytearray(src)
    for o, _, new in edits:
        out[o:o + len(new)] = new
    out += b"\0" * (raw_off - len(out)) + raw
    ck = opt + 64
    edit(ck, struct.pack("<I", checksum(out, ck)))
    out[ck:ck + 4] = edits[-1][2]

    patch = {
        "version": TARGET["version"],
        "file": "Weixin.dll",
        "size": len(src),
        "sha256": sha,
        "patched_size": len(out),
        "patched_sha256": hashlib.sha256(out).hexdigest(),
        "edits": [{"offset": o, "expected": old.hex(), "data": new.hex()} for o, old, new in edits],
        "append": {"offset": raw_off, "data": base64.b64encode(raw).decode()},
    }
    dst = ROOT / "patches" / f"{TARGET['version']}.json"
    dst.write_text(json.dumps(patch, indent=2) + "\n", encoding="utf-8")
    if a.out:
        pathlib.Path(a.out).write_bytes(out)
    print(f"代码 {len(code)} 字节 @0x{code_rva:x}，数据 {bss} 字节 @0x{data_rva:x}")
    for k in ("thunk_fin", "thunk_parse", "on_finalized", "on_parsed"):
        print(f"  {k} 0x{exports[k]:x}")
    for o, old, new in edits:
        print(f"  改 0x{o:x}  {old.hex()[:32]} → {new.hex()[:32]}")
    print("写出", dst.name, patch["patched_sha256"])


if __name__ == "__main__":
    main()
