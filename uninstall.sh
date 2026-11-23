#!/bin/bash
# 一键还原：把 install.sh 保存的原版微信放回去。
#
#   ./uninstall.sh                                  # 还原 /Applications/WeChat.app
#   ./uninstall.sh --app "/Applications/微信2.app"   # 还原指定的微信
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd -P)"
WORK="$HOME/Library/Application Support/WeChatAntiRecall"
APP=/Applications/WeChat.app
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
    --yes|-y)  YES=1; shift ;;
    -h|--help) sed -n '2,5p' "$HERE/uninstall.sh" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "不认识的参数：$1（用 --help 看用法）" ;;
  esac
done

[ "$(id -u)" -ne 0 ] || die "不要加 sudo 运行，需要密码时脚本会自己问"
case "$APP" in /*) ;; *) die "--app 请写完整路径" ;; esac
while :; do case "$APP" in ?*/) APP="${APP%/}" ;; ?*/.) APP="${APP%/.}" ;; *) break ;; esac; done
[ ! -L "$APP" ] || die "${APP} 是软链接，请用 --app 指定它指向的真正位置"
REAL="$(canon "$APP")" || die "找不到这个位置：${APP}"
APP="$REAL"

# 原版 = 打补丁前保存、签名仍然有效的那份；找最早的（后面的可能已经打过补丁）
intact() { /usr/bin/codesign --verify --deep --strict "$1" 2>/dev/null; }
signer() { /usr/bin/codesign -dv "$1" 2>&1 | sed -n 's/^TeamIdentifier=//p'; }
# PlistBuddy 在文件不存在时会往 stdout 打提示，所以先判断
build()  { [ -f "$1/Contents/Info.plist" ] || return 0; /usr/libexec/PlistBuddy -c "Print :CFBundleVersion" "$1/Contents/Info.plist" 2>/dev/null || true; }

NOW=""
if [ -e "$APP" ]; then
  NOW="$(build "$APP")"
  if [ "$(signer "$APP")" = 5A4RE8SF68 ] && intact "$APP"; then
    ok "现在就是官方原版（腾讯签名有效），不用还原"
    exit 0
  fi
fi

ORIG=""
for s in "$WORK/Originals/"*.source; do
  [ -f "$s" ] || continue
  b="${s%.source}"
  src="$(cat "$s")"
  [ "$src" = "$APP" ] || [ "$src" -ef "$APP" ] || continue
  [ -d "$b" ] && [ ! -L "$b" ] && intact "$b" || continue
  [ -z "$NOW" ] || [ "$(build "$b")" = "$NOW" ] || continue
  # 有腾讯签名的优先，其次最早的一份
  if [ "$(signer "$b")" = 5A4RE8SF68 ]; then ORIG="$b"; break; fi
  [ -n "$ORIG" ] || ORIG="$b"
done
[ -n "$ORIG" ] || die "没有找到 ${APP} 对应的原版备份（备份都在 ${WORK}/Originals）。
  可以直接删掉现在的微信，从 App Store 或官网重新安装（聊天记录不会丢）"

# 按命令行和按文件各查一遍。ps 在非 UTF-8 环境下会把中文路径转义，所以固定 locale；
# 按文件查只认进程名叫 WeChat 的，别的程序（验签、杀毒、索引）读到这个文件不算
running() {
  LC_ALL=en_US.UTF-8 ps -axo command= |
    WXAR_P="$APP/Contents/" LC_ALL=C awk 'index($0, ENVIRON["WXAR_P"]) == 1 { f = 1 } END { exit !f }' && return 0
  lsof -Fc "$APP/Contents/MacOS/WeChat" 2>/dev/null | grep -qx cWeChat
}
! running || die "微信还在运行。请切到它按 ⌘Q 完全退出（只关窗口不算），再重新运行"

if [ "$(signer "$ORIG")" = 5A4RE8SF68 ]; then KIND="官方原版（腾讯签名）"; else KIND="安装前的版本（不是腾讯签名）"; fi
say "将还原为${KIND}：${ORIG}"
if [ "$YES" = 0 ]; then
  printf '继续？(y/N) '
  read -r ans
  case "$ans" in y|Y|yes) ;; *) die "已取消" ;; esac
fi

if { [ ! -e "$APP" ] || [ -O "$APP" ]; } && [ -O "$ORIG" ] && [ -w "$(dirname "$APP")" ]; then NEED_SUDO=0; else NEED_SUDO=1; fi
priv() { if [ "$NEED_SUDO" = 1 ]; then sudo "$@"; else "$@"; fi; }
[ "$NEED_SUDO" = 0 ] || { say "替换微信需要管理员密码"; sudo -v || die "没有拿到管理员权限，没有改动"; }

mkdir -p "$WORK/Removed" 2>/dev/null || die "无法写入 ${WORK}。请先运行：sudo chown -R \"\$USER\" \"${WORK}\""
OLD="$WORK/Removed/$(basename "$APP")-$(date +%Y%m%d-%H%M%S).bak"

# 和安装一样，一口气做完，期间不响应 Ctrl-C 和关窗口
# 返回值：0 成功；10 没动；11 失败但已放回；12 失败且没能放回
SWAP='
trap "" INT TERM HUP
app=$1 orig=$2 old=$3
if [ -e "$app" ]; then mv "$app" "$old" || exit 10; fi
[ ! -e "$app" ] && mv "$orig" "$app" && exit 0
[ ! -e "$old" ] && exit 10
[ ! -e "$app" ] && mv "$old" "$app" && exit 11
exit 12
'
trap '' INT TERM HUP
rc=0
priv /bin/bash -c "$SWAP" swap "$APP" "$ORIG" "$OLD" || rc=$?
trap - INT TERM HUP
case "$rc" in
  0)  ;;
  11) die "还原失败，已放回原来的微信" ;;
  12) die "还原失败。原版在：${ORIG}
  打过补丁的在：${OLD}，请手动把其中一个移回 ${APP}" ;;
  *)  [ -e "$APP" ] || die "还原中断，请重新运行 ./uninstall.sh"
      die "没有拿到管理员权限，或系统不允许移动微信（没有改动）。
  请到 系统设置 → 隐私与安全性 → App 管理，打开「终端」的开关，完全退出终端后重试" ;;
esac
rm -f "$ORIG.source"
/usr/bin/codesign --verify --deep --strict "$APP" || die "已还原，但验签没通过，建议重新安装微信"
ok "已还原为${KIND}，签名有效"
[ ! -e "$OLD" ] || echo "  打过补丁的版本移到了：${OLD}（确认没问题后可以删除）"
