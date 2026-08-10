#!/usr/bin/env bash
# 在 Jetson 上编译 NVIDIA FFmpeg（含 h264_nvv4l2 / h265_nvv4l2 硬件编码器）
# 前置：需要 NVIDIA/ffmpeg 源码（NVIDIA fork，含 nvv4l2 补丁）
#   来源一（GitHub 恢复后）: git clone -b n4.4-nvv4l2 https://github.com/NVIDIA/ffmpeg.git
#   来源二（离线）: 把源码 tar 包放到 /tmp，执行: ./build_nvffmpeg.sh /path/to/ffmpeg-src.tar.gz
set -euo pipefail

SRC="${1:-/tmp/ffmpeg_nv}"
if [ ! -d "$SRC" ]; then
  echo "用法: $0 <ffmpeg_nv_src_dir_or_tarball>"
  echo "或先: git clone -b n4.4-nvv4l2 https://github.com/NVIDIA/ffmpeg.git $SRC"
  exit 1
fi
if [ -f "$SRC" ]; then
  D=$(mktemp -d); tar xzf "$SRC" -C "$D"
  SRC=$(find "$D" -maxdepth 1 -type d -name "ffmpeg*" | head -1)
fi

# 依赖（Jetson Multimedia API SDK 已装则跳过）
apt-get install -y -qq build-essential yasm nasm pkg-config libx264-dev \
  nvidia-l4t-jetson-multimedia-api >/dev/null 2>&1 || true

cd "$SRC"
make distclean >/dev/null 2>&1 || true
./configure \
  --enable-shared --disable-static --enable-pic \
  --enable-nvv4l2 --enable-nvmm \
  --enable-libx264 --enable-gpl \
  --prefix=/usr/local 2>&1 | tail -5
make -j"$(nproc)"
make install
ldconfig

echo "=== 验证 ==="
ffmpeg -hide_banner -encoders 2>/dev/null | grep -E "nvv4l2|libx264" | head
# 安装到 /usr/local/bin 后，若系统 /usr/bin/ffmpeg 仍在 PATH 前面，用全路径调用
/usr/local/bin/ffmpeg -hide_banner -encoders 2>/dev/null | grep nvv4l2
