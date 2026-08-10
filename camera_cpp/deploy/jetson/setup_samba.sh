#!/usr/bin/env bash
# 配置 Samba 共享 /data（完全免密码：guest only + force user=root）
# macOS Finder 网络栏直接浏览/读写录像与照片
# 用法: sudo ./setup_samba.sh
set -euo pipefail

echo "==> 1/3 安装 samba"
export DEBIAN_FRONTEND=noninteractive
apt-get install -y -qq samba

echo "==> 2/3 写入完整 /etc/samba/smb.conf（完全免密 guest only）"
[ -f /etc/samba/smb.conf ] && cp /etc/samba/smb.conf /etc/samba/smb.conf.bak
cat > /etc/samba/smb.conf <<'SMB'
[global]
   workgroup = WORKGROUP
   server string = camera_server NVR
   security = user
   map to guest = Bad User
   guest account = nobody
   ntlm auth = yes
   server min protocol = SMB2
   client min protocol = SMB2

[data]
   comment = camera_server data (recordings/photos)
   path = /data
   browseable = yes
   read only = no
   guest ok = yes
   guest only = yes
   force user = root
   create mask = 0666
   directory mask = 0777
SMB

echo "==> 3/3 启动 smbd/nmbd"
systemctl enable smbd nmbd >/dev/null 2>&1 || true
systemctl restart smbd nmbd

echo "==> 完成（完全免密，guest 可读写 /data）。macOS 使用："
echo "  Finder -> 前往 -> 网络（或 Cmd+K -> smb://<jetson-ip>），以游客身份连接"
echo "  共享 [data] = /data（录像 camera/recordings/<相机名>/，照片 camera/photos/<相机名>/）"
echo "  若想出现在 Finder 网络栏，需同时启用 mDNS：sudo ./setup_mdns.sh"
