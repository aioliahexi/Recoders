#!/usr/bin/env bash
# 软件调试自检脚本：设备 / 硬编 / 服务 / 协议 四查
# 用法（Jetson 上，插好摄像头后）: sudo ./debug_check.sh [http_port]
set -uo pipefail
PORT="${1:-8000}"
HOST=127.0.0.1
# 从 config.ini 读取 api_key（若配置了鉴权）
# api_key 从统一 config.json 读取（旧 config.ini 兜底）
KEY=$(python3 -c "import json,os; p='/opt/camera_server/config.json'; print(json.load(open(p)).get('api_key','') if os.path.exists(p) else '')" 2>/dev/null)
[ -z "$KEY" ] && KEY=$(grep -E '^api_key=' /opt/camera_server/config.ini 2>/dev/null | head -1 | cut -d= -f2 | tr -d '"' | tr -d ' ')
AUTH=(); [ -n "$KEY" ] && AUTH=(-H "Authorization: Bearer $KEY")
PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

echo "========== 1. USB 摄像头设备 =========="
CAM=$(/opt/camera_server/build/camera_server --list 2>/dev/null || true)
echo "$CAM"
echo "$CAM" | grep -q "cam \|0 路" && ok "设备发现命令正常" || bad "设备发现命令异常"
if echo "$CAM" | grep -qE "cam /dev/video"; then
  ok "发现 UVC 摄像头"
else
  bad "未发现 UVC 摄像头（检查 USB/供电/枚举）"
fi

echo "========== 2. 硬件编码（NVENC）=========="
NV=$(cat /sys/kernel/debug/bpmp/debug/clk/nvenc/rate 2>/dev/null || echo 0)
echo "  nvenc 时钟: $NV Hz"
[ "${NV:-0}" -gt 0 ] && ok "NVENC 可用" || bad "NVENC 时钟为 0"
gst-inspect-1.0 nvv4l2h264enc >/dev/null 2>&1 && ok "GStreamer nvv4l2h264enc 插件在" || bad "nvv4l2h264enc 缺失"

echo "========== 3. 服务状态 =========="
systemctl is-active camera_server >/dev/null 2>&1 && ok "camera_server systemd active" || bad "camera_server 未运行"
ss -tln 2>/dev/null | grep -q ":${PORT} " && ok "HTTP :$PORT 监听" || bad "HTTP :$PORT 未监听"
ss -tln 2>/dev/null | grep -q ":8554 " && ok "RTSP :8554 监听" || bad "RTSP :8554 未监听"

echo "========== 4. REST =========="
H=$(curl -s --max-time 5 "http://$HOST:$PORT/health") || H=""
echo "$H" | grep -q '"code":0' && ok "health" || bad "health"
CAMS=$(curl -s "${AUTH[@]}" --max-time 5 "http://$HOST:$PORT/api/v1/cameras") || CAMS=""
if echo "$CAMS" | grep -q '"code":0'; then
  ok "cameras 接口"
  echo "$CAMS" | python3 -c "
import json,sys
try:
    for i in json.load(sys.stdin)['data']['items']:
        print(f\"   {i['name']}: enc={i['encoder']} rec={i['recording']} online={i['online']}\")
except Exception as e: print('   解析失败', e)"
else
  bad "cameras 接口"
fi
PH=$(curl -s "${AUTH[@]}" --max-time 8 -X POST "http://$HOST:$PORT/api/v1/cameras/1/photo" -H 'Content-Type: application/json' -d '{"quality":90}') || PH=""
echo "$PH" | grep -q '"code":0' && ok "抓拍" || bad "抓拍"
REC=$(curl -s "${AUTH[@]}" --max-time 5 "http://$HOST:$PORT/api/v1/cameras/1/recordings?page=1&page_size=3") || REC=""
echo "$REC" | grep -q '"items":\[\]' && bad "录像列表为空" || ok "录像列表有数据"

echo "========== 5. RTSP =========="
# 用真实相机名（相机可自定义名称，RTSP 流路径跟随名称）
RTSP_NAME=$(echo "$CAMS" | python3 -c "
import json,sys
try:
    items=json.load(sys.stdin)['data']['items']
    print(items[0]['name'] if items else '')
except Exception: print('')")
[ -z "$RTSP_NAME" ] && RTSP_NAME="CAM01"
R=$(timeout 8 ffprobe -v error -rtsp_transport tcp -i "rtsp://$HOST:8554/$RTSP_NAME" \
     -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 2>&1) || R=""
echo "$R" | grep -q "codec_name=h264" && ok "RTSP 拉流($RTSP_NAME) $R" || bad "RTSP 拉流失败"

echo "========== 6. ONVIF =========="
SOAP='<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/></s:Body></s:Envelope>'
ONV=$(curl -s "${AUTH[@]}" --max-time 5 -X POST "http://$HOST:$PORT/onvif/device_service" -H 'Content-Type: application/soap+xml' --data "$SOAP") || ONV=""
echo "$ONV" | grep -q "GetDeviceInformationResponse" && ok "ONVIF GetDeviceInformation" || bad "ONVIF"

echo ""
echo "=============================================="
echo " 结果: PASS=$PASS  FAIL=$FAIL"
[ "$FAIL" -eq 0 ] && echo " 全部通过 ✅" || echo " 存在失败项，见上方 [FAIL]"
echo "=============================================="
exit $FAIL
