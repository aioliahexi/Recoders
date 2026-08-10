#!/usr/bin/env bash
# Jetson Xavier NX 部署脚本：同步源码 -> 装依赖 -> 构建 -> systemd 安装
# 用法: ./deploy/jetson/deploy.sh <ssh_host>  例如 ./deploy/jetson/deploy.sh root@192.168.1.100
set -euo pipefail

HOST="${1:?用法: deploy.sh <ssh_host>}"
APP_DIR=/opt/camera_server
SRC_DIR="$(cd "$(dirname "$0")/../.." && pwd)"

echo "==> 1/5 同步源码到 ${HOST}:${APP_DIR}"
ssh "$HOST" "mkdir -p ${APP_DIR} && rm -rf ${APP_DIR}/src ${APP_DIR}/deploy"
rsync -az --delete \
  --exclude '.git' --exclude 'build' --exclude '.venv' --exclude '*.o' --exclude 'config.ini' --exclude 'config.nvgst.ini' --exclude 'config.json' \
  "$SRC_DIR/" "$HOST:${APP_DIR}/"

echo "==> 2/5 安装依赖（JetPack 5.x / Ubuntu 20.04）"
ssh "$HOST" "apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  build-essential cmake pkg-config ffmpeg \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libavdevice-dev \
  libsqlite3-dev >/dev/null"

echo "==> 3/5 构建"
ssh "$HOST" "cd ${APP_DIR} && rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build -j\$(nproc)"

echo "==> 4/5 写入配置（按实际摄像头修改 ${APP_DIR}/config.json）"
ssh "$HOST" "test -f ${APP_DIR}/config.json || test -f ${APP_DIR}/config.ini || cp ${APP_DIR}/deploy/jetson/config.jetson.example.json ${APP_DIR}/config.json"

echo "==> 5/5 安装 systemd 服务并启动"
ssh "$HOST" "sed 's|/opt/camera_server|${APP_DIR}|g' ${APP_DIR}/deploy/camera_server.service > /etc/systemd/system/camera_server.service && \
  systemctl daemon-reload && systemctl enable camera_server && systemctl restart camera_server"

echo "==> 部署完成。验证:"
echo "  ssh ${HOST} 'systemctl status camera_server'"
echo "  curl http://<jetson-ip>:8000/api/v1/cameras"
echo "  ffprobe -rtsp_transport tcp -i rtsp://<jetson-ip>:8554/CAM01"
