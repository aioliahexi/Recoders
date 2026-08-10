#!/usr/bin/env bash
# 配置 mDNS（Avahi）：让 macOS/Linux 局域网通过 Bonjour 发现本机 NVR
# 用法: sudo ./setup_mdns.sh
set -euo pipefail

echo "==> 1/3 安装 avahi-daemon"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq avahi-daemon avahi-utils

echo "==> 2/3 写入 /etc/avahi/services/camera-server.service"
cat > /etc/avahi/services/camera-server.service <<'SVC'
<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">camera_server NVR on %h</name>
  <!-- Web 管理界面（macOS Safari 书签 Bonjour 可发现） -->
  <service>
    <type>_http._tcp</type>
    <port>8081</port>
    <txt-record>path=/</txt-record>
    <txt-record>type=camera_server</txt-record>
  </service>
  <!-- RTSP 实时流（VLC/QuickTime 可发现） -->
  <service>
    <type>_rtsp._tcp</type>
    <port>8554</port>
    <txt-record>path=rtsp://%h:8554/</txt-record>
  </service>
  <!-- 自定义服务类型（camera_server） -->
  <service>
    <type>_camera-server._tcp</type>
    <port>8000</port>
    <txt-record>api=/api/v1/cameras</txt-record>
  </service>
  <!-- SMB 共享（macOS Finder 网络栏可见，浏览录像/照片） -->
  <service>
    <type>_smb._tcp</type>
    <port>445</port>
    <txt-record>path=/data</txt-record>
  </service>
</service-group>
SVC

echo "==> 3/3 启用并启动 avahi-daemon"
systemctl enable avahi-daemon >/dev/null 2>&1 || true
systemctl restart avahi-daemon

echo "==> 完成。macOS 上可用以下命令验证："
echo "  dns-sd -B _camera-server._tcp   # 自定义服务"
echo "  dns-sd -B _http._tcp            # Web 界面"
echo "  dns-sd -B _rtsp._tcp            # RTSP 流"
echo "（Safari 书签 -> Bonjour 可发现 Web 界面）"
