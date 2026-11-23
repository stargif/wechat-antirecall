#!/bin/bash
# 一键安装：先在副本上打补丁、重签名、验签，全部通过后才替换正式的微信。
# 原来的微信会完整保留，./uninstall.sh 可一键还原。
#
#   ./install.sh                                  # 装到 /Applications/WeChat.app
#   ./install.sh --app "/Applications/微信2.app"   # 指定别的微信
#   ./install.sh --variant keeptip                # 指定效果（默认自动选最好的）
#   ./install.sh --check                          # 只检查能不能装，不改任何东西
set -euo pipefail
export PYTHONIOENCODING=utf-8

cd "$(dirname "$0")"
TOOL="$PWD/wechat_antirecall.py"
PY=/usr/bin/python3
WORK="$HOME/Library/Application Support/WeChatAntiRecall"
APP=/Applications/WeChat.app
VARIANT=""
CHECK=0
YES=0

say()  { printf '\033[36m·\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m✓\033[0m %s\n' "$*"; }
die()  { printf '\033[31m✗ %s\033[0m\n' "$*" >&2; exit 1; }

# 路径规范化：去掉多余斜杠和上级目录里的软链接，并取磁盘上的真实大小写
canon() {
  local d e
  d="$(cd "$(dirname "$1")" 2>/dev/null && pwd -P)" || return 1
  if [ -e "$1" ]; then
    for e in "${d%/}"/*; do
      if [ ! -L "$e" ] && [ "$e" -ef "$1" ]; then printf '%s\n' "$e"; return 0; fi
    done
  fi
  printf '%s/%s\n' "${d%/}" "$(basename "$1")"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --app)     [ $# -ge 2 ] || die "--app 后面要跟微信的路径"; APP="$2"; shift 2 ;;
    --variant) [ $# -ge 2 ] || die "--variant 后面要跟 keeptip-content / keeptip / silent"; VARIANT="$2"; shift 2 ;;
    --check)   CHECK=1; shift ;;
    --yes|-y)  YES=1; shift ;;
    -h|--help) sed -n '2,8p' "$PWD/install.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "不认识的参数：$1（用 --help 看用法）" ;;
  esac
done

# ---------------------------------------------------------------- 环境检查

[ "$(id -u)" -ne 0 ] || die "不要加 sudo 运行，需要密码时脚本会自己问"
[ "$(sysctl -n hw.optional.arm64 2>/dev/null || echo 0)" = 1 ] || die "只支持 Apple Silicon（M 系列）的 Mac"
xcode-select -p >/dev/null 2>&1 || die "缺少命令行工具，请先运行：xcode-select --install"
case "$APP" in /*) ;; *) die "--app 请写完整路径" ;; esac
[ ! -L "${APP%/}" ] || die "${APP} 是软链接，请用 --app 指定它指向的真正位置"
APP="$(canon "$APP")" || die "找不到微信：${APP}（用 --app 指定路径）"
[ -f "$APP/Contents/Info.plist" ] || die "找不到微信：${APP}（用 --app 指定路径）"

plist() { /usr/libexec/PlistBuddy -c "Print :$1" "$APP/Contents/Info.plist" 2>/dev/null || true; }
BUILD="$(plist CFBundleVersion)"
say "微信 $(plist CFBundleShortVersionString)（构建号 ${BUILD}）：${APP}"

if [ -z "$VARIANT" ]; then
  VARIANT="$("$PY" - "$BUILD" <<'P'
import json, sys
t = json.load(open("patches.json", encoding="utf-8"))["builds"].get(sys.argv[1], {}).get("targets", {})
print(next((v for v, k in (("keeptip-content", "revoke-keeptip-content"), ("keeptip", "revoke-keeptip"),
                           ("silent", "revoke")) if k in t), ""))
P
)"
  [ -n "$VARIANT" ] || die "还不支持构建号 ${BUILD}。支持的列表：${PY} wechat_antirecall.py list"
fi
say "效果：${VARIANT}"

if ! out="$("$PY" "$TOOL" apply --app "$APP" --variant "$VARIANT" --dry-run 2>&1)"; then
  printf '%s\n' "$out" >&2
  case "$out" in
    *另一个变体*) die "已经装了别的效果，请先运行 ./uninstall.sh 还原，再重新安装" ;;
    *) die "检查没通过，没有改动任何东西" ;;
  esac
fi
case "$out" in *无需改动*) ok "已经装好了，不用再装"; exit 0 ;; esac
ok "补丁点全部核对通过"
[ "$CHECK" = 0 ] || { ok "可以安装（--check 只检查，没有改动任何东西）"; exit 0; }

# 按命令行和按文件各查一遍。ps 在非 UTF-8 环境下会把中文路径转义，所以固定 locale；
# 按文件查只认进程名叫 WeChat 的，别的程序（验签、杀毒、索引）读到这个文件不算
running() {
  LC_ALL=en_US.UTF-8 ps -axo command= |
    WXAR_P="$APP/Contents/" LC_ALL=C awk 'index($0, ENVIRON["WXAR_P"]) == 1 { f = 1 } END { exit !f }' && return 0
  lsof -Fc "$APP/Contents/MacOS/WeChat" 2>/dev/null | grep -qx cWeChat
}
! running || die "微信还在运行。请切到它按 ⌘Q 完全退出（只关窗口不算），再重新运行"

if [ "$YES" = 0 ]; then
  printf '将替换 %s，并重新签名（会变成非腾讯签名，可能需要重新登录）。继续？(y/N) ' "$APP"
  read -r ans
  case "$ans" in y|Y|yes) ;; *) die "已取消" ;; esac
fi

# ---------------------------------------------------------------- 在副本上打补丁

NAME="$(basename "$APP")"
STAGE_DIR="$WORK/staging"
STAGE="$STAGE_DIR/$NAME"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR" "$WORK/Originals" 2>/dev/null ||
  die "无法写入 ${WORK}。如果以前用 sudo 跑过，请先运行：sudo chown -R \"\$USER\" \"${WORK}\""
trap 'rm -rf "$STAGE_DIR"' EXIT

say "制作副本 …"
cp -cR "$APP" "$STAGE" 2>/dev/null || { rm -rf "$STAGE"; /usr/bin/ditto "$APP" "$STAGE"; }
# 副本必须是真正的目录：要是复制成了软链接，后面就会改到正式的微信
[ -d "$STAGE" ] && [ ! -L "$STAGE" ] || die "制作副本失败，正式的微信没有改动"
chmod -R u+w "$STAGE"
say "给副本打补丁并重签名（约半分钟）…"
"$PY" "$TOOL" apply --app "$STAGE" --variant "$VARIANT" --no-backup --yes >"$WORK/install.log" 2>&1 ||
  { tail -20 "$WORK/install.log" >&2; die "副本打补丁失败，正式的微信没有改动"; }
/usr/bin/codesign --verify --deep --strict "$STAGE" || die "副本验签失败，正式的微信没有改动"
ok "副本就绪，签名有效"

# ---------------------------------------------------------------- 替换

if [ -O "$APP" ] && [ -w "$(dirname "$APP")" ]; then NEED_SUDO=0; else NEED_SUDO=1; fi
priv() { if [ "$NEED_SUDO" = 1 ]; then sudo "$@"; else "$@"; fi; }
[ "$NEED_SUDO" = 0 ] || { say "替换微信需要管理员密码"; sudo -v || die "没有拿到管理员权限，微信没有改动"; }
! running || die "微信又被打开了，请退出后重新运行（微信没有改动）"

STAMP="$(date +%Y%m%d-%H%M%S)"
ORIG="$WORK/Originals/$NAME-$BUILD-$STAMP.bak"
printf '%s\n' "$APP" >"$ORIG.source"

# 替换只有几秒，放在一个进程里一口气做完，期间不响应 Ctrl-C 和关窗口；失败就地还原。
# 返回值：0 成功；10 没动；11 失败但已还原；12 失败且没能还原
SWAP='
trap "" INT TERM HUP
app=$1 orig=$2 stage=$3 failed=$4
mv "$app" "$orig" || exit 10
/usr/bin/ditto "$stage" "$app" && /usr/bin/codesign --verify --deep --strict "$app" && exit 0
if [ -e "$app" ]; then mv "$app" "$failed" || exit 12; fi
[ ! -e "$app" ] && mv "$orig" "$app" && exit 11
exit 12
'
trap '' INT TERM HUP
rc=0
priv /bin/bash -c "$SWAP" swap "$APP" "$ORIG" "$STAGE" "$WORK/Originals/$NAME-失败-$STAMP.bak" || rc=$?
trap - INT TERM HUP
case "$rc" in
  0)  ;;
  12) die "替换失败，也没能自动还原。原版微信在：${ORIG}
  请运行 ./uninstall.sh，或手动把它移回 ${APP}" ;;
  11) rm -f "$ORIG.source"; die "替换失败，已还原，微信保持原样" ;;
  *)  if [ ! -e "$APP" ] && [ -d "$ORIG" ]; then die "替换中断。原版微信在：${ORIG}，请运行 ./uninstall.sh 还原"; fi
      rm -f "$ORIG.source"
      die "没有拿到管理员权限，或系统不允许移动微信（微信没有改动）。
  请到 系统设置 → 隐私与安全性 → App 管理，打开「终端」的开关，完全退出终端后重试" ;;
esac

ok "安装完成：${VARIANT}"
echo "  原来的微信保存在：${ORIG}"
echo "  打开微信后，让别人私聊发一条消息再撤回，看看效果。"
echo "  如果弹出访问数据的窗口，点「允许」；想还原就运行 ./uninstall.sh"
