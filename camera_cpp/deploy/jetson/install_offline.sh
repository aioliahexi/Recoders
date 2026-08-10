#!/usr/bin/env bash
# Jetson Xavier NX 离线安装脚本（解压包后执行）
# 用法: sudo ./install_offline.sh
set -euo pipefail
APP_DIR=/opt/camera_server
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "==> 1/4 检查/安装依赖（Jetson JetPack 5.x, Ubuntu 20.04）"
if command -v apt-get >/dev/null; then
  apt-get update -qq || true
  DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    build-essential cmake pkg-config ffmpeg \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev \
    libsqlite3-dev >/dev/null || true
fi

echo "==> 2/4 安装源码到 ${APP_DIR}"
mkdir -p "$APP_DIR"
rsync -a --delete --exclude 'build' "$HERE/" "$APP_DIR/" 2>/dev/null || cp -a "$HERE/." "$APP_DIR/"

echo "==> 3/4 构建"
cd "$APP_DIR"
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build -j"$(nproc)"

echo "==> 4/4 配置 + systemd"
test -f "$APP_DIR/config.json" || test -f "$APP_DIR/config.ini" || cp "$APP_DIR/deploy/jetson/config.jetson.example.json" "$APP_DIR/config.json"
sed "s|/opt/camera_server|$APP_DIR|g" "$APP_DIR/deploy/camera_server.service" > /etc/systemd/system/camera_server.service
systemctl daemon-reload
systemctl enable camera_server
systemctl restart camera_server

echo "==> 完成。"
echo "  查看:  systemctl status camera_server"
echo "  相机:  $APP_DIR/build/camera_server --config $APP_DIR/config.json --list"
echo "  修改配置后:  systemctl restart camera_server"
