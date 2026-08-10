#!/usr/bin/env bash
# 配置 NFS 共享：将 data_dir 目录（默认 /data）共享给局域网，免账号密码直接读取录像/照片
# 用法: sudo ./setup_nfs.sh [共享目录] [网段]
set -euo pipefail
SHARE="${1:-/data}"
SUBNET="${2:-192.168.0.0/24}"

echo "==> 1/3 安装 nfs-kernel-server"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq nfs-kernel-server

echo "==> 2/3 写入 /etc/exports：${SHARE} ${SUBNET}(rw,sync,no_subtree_check,no_root_squash,insecure)"
# 避免重复行
grep -qF "${SHARE} ${SUBNET}" /etc/exports 2>/dev/null || \
  echo "${SHARE} ${SUBNET}(rw,sync,no_subtree_check,no_root_squash,insecure)" >> /etc/exports
exportfs -ra

echo "==> 3/3 启用并启动 nfs-server"
systemctl enable nfs-server >/dev/null 2>&1 || true
systemctl restart nfs-server

echo "==> 完成，导出列表："
exportfs -v
echo
echo "外部挂载示例（Mac/Linux）："
echo "  mount -t nfs <jetson-ip>:${SHARE} /mnt/nfs    # 录像在 <mount>/camera/recordings/，照片在 <mount>/camera/photos/"
echo "  # 或直接 Finder：Go -> Connect to Server -> nfs://<jetson-ip>:${SHARE}"
