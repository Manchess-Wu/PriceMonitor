#!/usr/bin/env bash
# PriceMonitor 登录助手
#   自动停止服务 -> 依次打开淘宝/京东登录窗口 -> 保存登录态 -> 重启服务
#
#   bash deploy/login.sh              # 淘宝 + 京东
#   bash deploy/login.sh taobao       # 只登录淘宝
#   bash deploy/login.sh jd           # 只登录京东
#
# 需要图形界面：本机桌面、VNC，或 ssh -X 转发。
# 服务已安装时请用与服务相同的用户执行本脚本，否则登录态权限不对。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${APP_DIR:-/opt/pricemonitor}"
SERVICE_NAME="${SERVICE_NAME:-pricemonitor}"
SITES="${*:-taobao jd}"

# ---------- 定位可执行文件与配置 ----------
if [ -x "$APP_DIR/PriceMonitor" ]; then
    BIN="$APP_DIR/PriceMonitor"
    CONFIG="${CONFIG:-$APP_DIR/config/config.json}"
elif [ -x "$ROOT_DIR/build/PriceMonitor" ]; then
    BIN="$ROOT_DIR/build/PriceMonitor"
    CONFIG="${CONFIG:-$ROOT_DIR/config/config.json}"
else
    echo "!! 找不到 PriceMonitor，请先执行 bash deploy/install.sh" >&2
    exit 1
fi

if [ ! -f "$CONFIG" ]; then
    echo "!! 配置文件不存在: $CONFIG" >&2
    exit 1
fi

# 程序的根目录判定顺序：可执行文件上一级（有 config）-> 可执行文件同级 -> 当前目录
if [ -f "$(dirname "$BIN")/../config/config.json" ]; then
    BASE_DIR="$(cd "$(dirname "$BIN")/.." && pwd)"
elif [ -f "$(dirname "$BIN")/config/config.json" ]; then
    BASE_DIR="$(cd "$(dirname "$BIN")" && pwd)"
else
    BASE_DIR="$(pwd)"
fi

# 统一切到 BASE_DIR 再启动登录窗口。否则在源码目录执行本脚本时，
# 程序会把 ./data 解析到源码目录，登录态就写不到服务读取的 profile 里，
# 表现为“刚扫码登录，采集仍提示登录已失效”。
cd "$BASE_DIR"

# ---------- 读取浏览器配置 ----------
IFS='|' read -r CFG_CHROME CFG_PROFILE CFG_ENABLED < <(python3 - "$CONFIG" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as f:
    cfg = json.load(f)
browser = (cfg.get("crawler") or {}).get("browser") or {}
chrome = browser.get("chrome_path") or "-"
profile = browser.get("profile_dir") or "./data/chrome-profile"
enabled = "1" if browser.get("enabled", True) else "0"
print(f"{chrome}|{profile}|{enabled}")
PY
)

if [ "$CFG_ENABLED" != "1" ]; then
    echo "!! config.json 中 crawler.browser.enabled 为 false，请先改为 true" >&2
    exit 1
fi

# ---------- 选择浏览器可执行文件 ----------
CHROME=""
if [ "$CFG_CHROME" != "-" ] && [ -n "$CFG_CHROME" ]; then
    if command -v "$CFG_CHROME" >/dev/null 2>&1; then
        CHROME="$(command -v "$CFG_CHROME")"
    elif [ -x "$CFG_CHROME" ]; then
        CHROME="$CFG_CHROME"
    fi
fi
if [ -z "$CHROME" ]; then
    for candidate in chromium chromium-browser google-chrome google-chrome-stable; do
        if command -v "$candidate" >/dev/null 2>&1; then
            CHROME="$(command -v "$candidate")"
            break
        fi
    done
fi
if [ -z "$CHROME" ]; then
    echo "!! 未找到 Chromium/Chrome，请安装后重试：" >&2
    echo "   sudo apt-get install -y chromium" >&2
    exit 1
fi
export PM_CHROME_PATH="$CHROME"
echo "==> 使用浏览器: $CHROME"

# ---------- 配置目录 ----------
if [[ "$CFG_PROFILE" = /* ]]; then
    PROFILE_DIR="$CFG_PROFILE"
else
    PROFILE_DIR="$BASE_DIR/${CFG_PROFILE#./}"
fi
mkdir -p "$PROFILE_DIR"
chmod 700 "$PROFILE_DIR" 2>/dev/null || true

# ---------- 图形环境检查 ----------
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
    cat >&2 <<'MSG'
!! 当前没有图形界面，无法扫码登录。可选：
   1) 本机桌面 / VNC 上执行本脚本
   2) 通过 SSH 转发：ssh -X user@设备IP，再执行 bash deploy/login.sh
   3) 在另一台同系统机器登录后，把整个 profile 目录拷过来（保持属主一致）
MSG
    exit 1
fi

# ---------- 服务控制 ----------
has_systemd() {
    command -v systemctl >/dev/null 2>&1 &&
        [ "$(ps -p 1 -o comm= 2>/dev/null)" = "systemd" ] &&
        systemctl list-unit-files 2>/dev/null | grep -q "^${SERVICE_NAME}.service"
}

as_root() {
    if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi
}

SERVICE_STOPPED=0
if has_systemd; then
    echo "==> 停止 $SERVICE_NAME 服务（避免占用浏览器配置目录）"
    as_root systemctl stop "$SERVICE_NAME" || true
    SERVICE_STOPPED=1
else
    pkill -x PriceMonitor 2>/dev/null || true
    sleep 1
fi

cleanup_chrome() {
    # 只清理使用本配置目录的浏览器，避免误杀其他 Chrome
    pkill -TERM -f -- "--user-data-dir=$PROFILE_DIR" 2>/dev/null || true
    sleep 2
    pkill -KILL -f -- "--user-data-dir=$PROFILE_DIR" 2>/dev/null || true
    rm -f "$PROFILE_DIR"/SingletonLock "$PROFILE_DIR"/SingletonCookie \
        "$PROFILE_DIR"/SingletonSocket 2>/dev/null || true
}

trap 'cleanup_chrome' EXIT

# ---------- 依次登录 ----------
for site in $SITES; do
    case "$site" in
        taobao|tmall|jd) ;;
        *) echo "!! 未知站点: $site（可选 taobao / tmall / jd）" >&2; exit 1 ;;
    esac

    cleanup_chrome
    echo
    echo "==> 打开 $site 登录窗口"
    if [ "$site" = "jd" ]; then
        echo "    请扫码或输入账号密码登录京东，登录成功后关闭浏览器窗口"
    elif [ "$site" = "tmall" ]; then
        echo "    请扫码登录天猫（天猫 Cookie 独立于淘宝，监控天猫商品必须单独登录）"
        echo "    登录成功后关闭浏览器窗口"
    else
        echo "    请用手机淘宝扫码登录，登录成功后关闭浏览器窗口"
    fi

    DISPLAY="${DISPLAY:-}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}" \
        XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}" \
        "$BIN" --login "$site" "$CONFIG"

    sleep 1
    cleanup_chrome

    if [ -f "$PROFILE_DIR/Default/Cookies" ] || [ -f "$PROFILE_DIR/Default/Network/Cookies" ]; then
        echo "==> $site 登录态已保存到 $PROFILE_DIR"
    else
        echo "!! 未检测到 Cookie 文件，$site 可能没有登录成功" >&2
    fi
done

trap - EXIT

# ---------- 权限与重启 ----------
if [ "$(id -u)" -eq 0 ] && id "$SERVICE_NAME" >/dev/null 2>&1; then
    chown -R "$SERVICE_NAME":"$SERVICE_NAME" "$PROFILE_DIR"
fi

echo
if [ "$SERVICE_STOPPED" = "1" ]; then
    echo "==> 启动 $SERVICE_NAME 服务"
    as_root systemctl start "$SERVICE_NAME"
    as_root systemctl --no-pager status "$SERVICE_NAME" | head -n 8 || true
else
    echo "==> 后台启动 PriceMonitor"
    cd "$BASE_DIR"
    nohup ./PriceMonitor >/dev/null 2>&1 &
    sleep 2
    if pgrep -x PriceMonitor >/dev/null; then
        echo "已启动（PID $(pgrep -x PriceMonitor | head -n 1)）"
    else
        echo "!! 启动失败，请手动运行 $BIN 查看输出" >&2
    fi
fi

echo
echo "完成。打开 http://<设备IP>:8080 添加商品，首次采集即可看到价格。"
echo "登录态保存在: $PROFILE_DIR"
