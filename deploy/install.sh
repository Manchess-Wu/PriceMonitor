#!/usr/bin/env bash
# PriceMonitor 一键构建 + 安装脚本（Debian / Ubuntu / Raspberry Pi OS）
#   ./deploy/install.sh          构建并安装
#   ./deploy/install.sh --deps   仅安装编译依赖（需 root）
set -euo pipefail

APP_DIR="/opt/pricemonitor"
SERVICE_USER="pricemonitor"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${1:-}" == "--deps" ]]; then
    echo "==> 安装编译依赖"
    sudo apt-get update
    sudo apt-get install -y build-essential cmake pkg-config git \
        libcurl4-openssl-dev libsqlite3-dev libssl-dev zlib1g-dev uuid-dev \
        libjsoncpp-dev libbrotli-dev nlohmann-json3-dev
    echo "==> 安装 Chromium（淘宝采集需要真实浏览器执行页面 JS）"
    sudo apt-get install -y chromium || sudo apt-get install -y chromium-browser || \
        echo "!! Chromium 安装失败，请手动安装并在 config.json 中设置 crawler.browser.chrome_path"
    echo "==> Lexbor 需从源码构建（官方仓库）"
    if [ ! -f /usr/local/lib/liblexbor.so ] && ! ldconfig -p | grep -q liblexbor; then
        tmp="$(mktemp -d)"
        git clone --depth 1 https://github.com/lexbor/lexbor.git "$tmp/lexbor"
        cmake -S "$tmp/lexbor" -B "$tmp/lexbor/build" -DCMAKE_BUILD_TYPE=Release \
            -DLEXBOR_BUILD_TESTS=OFF -DLEXBOR_BUILD_EXAMPLES=OFF
        cmake --build "$tmp/lexbor/build" -j"$(nproc)"
        sudo cmake --install "$tmp/lexbor/build"
        sudo ldconfig
    fi
    echo "==> Drogon 需从源码构建（>= 1.8）"
    if ! pkg-config --exists drogon 2>/dev/null && [ ! -d /usr/local/lib/cmake/Drogon ]; then
        tmp="$(mktemp -d)"
        git clone --depth 1 --recurse-submodules https://github.com/drogonframework/drogon.git "$tmp/drogon"
        cmake -S "$tmp/drogon" -B "$tmp/drogon/build" -DCMAKE_BUILD_TYPE=Release \
            -DBUILD_EXAMPLES=OFF -DBUILD_CTL=OFF -DBUILD_TESTING=OFF \
            -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_SQLITE=OFF \
            -DBUILD_REDIS=OFF -DBUILD_YAML_CONFIG=OFF
        cmake --build "$tmp/drogon/build" -j"$(nproc)"
        sudo cmake --install "$tmp/drogon/build"
        sudo ldconfig
    fi
    exit 0
fi

echo "==> 编译 PriceMonitor"
cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT_DIR/build" -j"$(nproc)"

echo "==> 创建运行用户与目录"
if ! id "$SERVICE_USER" &>/dev/null; then
    sudo useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
fi
sudo mkdir -p "$APP_DIR/config" "$APP_DIR/web" "$APP_DIR/data"
sudo install -m 755 "$ROOT_DIR/build/PriceMonitor" "$APP_DIR/PriceMonitor"
sudo cp -r "$ROOT_DIR/web/." "$APP_DIR/web/"
if [ ! -f "$APP_DIR/config/config.json" ]; then
    sudo install -m 644 "$ROOT_DIR/config/config.example.json" "$APP_DIR/config/config.json"
    echo "已生成 $APP_DIR/config/config.json，请填写淘宝 Cookie 与通知配置后重启服务。"
fi
sudo chown -R "$SERVICE_USER":"$SERVICE_USER" "$APP_DIR/data" "$APP_DIR/config"

echo "==> 安装 systemd 服务"
sudo install -m 644 "$ROOT_DIR/deploy/pricemonitor.service" /etc/systemd/system/pricemonitor.service
sudo systemctl daemon-reload
sudo systemctl enable pricemonitor
# 必须 restart：enable --now 对已运行的服务不会重新加载新二进制
sudo systemctl restart pricemonitor
sudo systemctl --no-pager status pricemonitor || true

echo "完成。Web 管理界面: http://<设备IP>:8080"
echo
echo "下一步：执行登录助手，扫码登录淘宝和京东（各一次）"
echo "   bash $ROOT_DIR/deploy/login.sh"
echo "登录态保存在 $APP_DIR/data/chrome-profile，之后每小时自动采集。"
