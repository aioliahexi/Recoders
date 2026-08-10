#!/usr/bin/env bash
# 在 Mac 上生成离线部署包：camera_server_bundle.tar.gz
# Jetson 无网络时：把包拷过去，解压后运行 install_offline.sh
set -euo pipefail
SRC_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="/tmp/camera_server_bundle.tar.gz"
mkdir -p /tmp/camera_bundle
rsync -a --exclude '.git' --exclude 'build' --exclude '.venv' --exclude '*.o' --exclude '.DS_Store' \
  "$SRC_DIR/" /tmp/camera_bundle/
cp "$(dirname "$0")/install_offline.sh" /tmp/camera_bundle/install_offline.sh
chmod +x /tmp/camera_bundle/install_offline.sh
tar -czf "$OUT" -C /tmp/camera_bundle .
echo "生成完成: $OUT ($(du -h "$OUT" | cut -f1))"
echo "拷贝到 Jetson 后: tar xzf camera_server_bundle.tar.gz && sudo ./install_offline.sh"
