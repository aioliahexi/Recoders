#!/usr/bin/env bash
# 生成 udev 规则：把 UVC 摄像头绑定为稳定的 /dev/cam01, cam02...
# 多厂商通用（按 USB 序列号优先，缺失则按物理端口 ID_PATH）
# 用法（在 Jetson 上）: sudo ./make_udev_rules.sh
set -euo pipefail
RULES=/etc/udev/rules.d/99-camera-server.rules
: > "$RULES"
idx=1
for dev in /sys/class/video4linux/video*; do
  [ -e "$dev" ] || continue
  name=$(basename "$dev")
  [ "${name:0:5}" = "video" ] || continue
  # 只处理 UVC 视频采集（index=0），跳过编码器等 M2M 节点
  [ "$(cat "$dev/index" 2>/dev/null)" = "0" ] || continue
  # 找到 USB 设备目录（含 idVendor）
  usbdev=$(cd "$dev/device" 2>/dev/null && while [ ! -e idVendor ]; do cd .. || break; done && pwd 2>/dev/null) || continue
  [ -n "$usbdev" ] || continue
  serial=$(cat "$usbdev/serial" 2>/dev/null || true)
  idpath=$(cat "$dev/device/../uevent" 2>/dev/null | grep ID_PATH= | cut -d= -f2 || true)
  if [ -n "$serial" ]; then
    # 序列号优先：SUBSYSTEMS=="usb" ATTRS{serial}=="..."
    echo "SUBSYSTEM==\"video4linux\", ATTR{index}==\"0\", SUBSYSTEMS==\"usb\", ATTRS{serial}==\"$serial\", SYMLINK+=\"cam$idx\"" >> "$RULES"
  elif [ -n "$idpath" ]; then
    echo "SUBSYSTEM==\"video4linux\", ATTR{index}==\"0\", ENV{ID_PATH}==\"$idpath\", SYMLINK+=\"cam$idx\"" >> "$RULES"
  fi
  echo "  cam$idx -> $name (serial=$serial idpath=$idpath)"
  idx=$((idx+1))
done
if [ "$(wc -l < "$RULES")" -gt 0 ]; then
  udevadm control --reload-rules && udevadm trigger
  echo "==> 规则已生成: $RULES"
else
  echo "==> 未发现 UVC 摄像头（插上后重跑）"
fi
