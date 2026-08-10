#!/usr/bin/env bash
# 全面回归测试（自动化可执行项；对应 docs/08-全面测试用例清单.md）
# 用法: sudo ./test_all.sh [http_port] [--destructive]
#   --destructive 才执行会改动数据的用例（删除录像/清空/改名往返等）
set -uo pipefail

PORT="${1:-8000}"
[ "${2:-}" = "--destructive" ] && DESTRUCTIVE=1 || DESTRUCTIVE=0
HOST=127.0.0.1
# api_key 从统一 config.json 读取（旧 config.ini 兜底）
KEY=$(python3 -c "import json,os; p='/opt/camera_server/config.json'; print(json.load(open(p)).get('api_key','') if os.path.exists(p) else '')" 2>/dev/null)
[ -z "$KEY" ] && KEY=$(grep -E '^api_key=' /opt/camera_server/config.ini 2>/dev/null | head -1 | cut -d= -f2 | tr -d '"' | tr -d ' ')
AUTH=(); [ -n "$KEY" ] && AUTH=(-H "X-API-Key: $KEY")
BASE="http://$HOST:$PORT"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
now()  { date +%s%3N; }

# JSON 字段断言: jget <json> <field> -> 值（顶层）；jdata -> data.<field>
jget()  { python3 -c "import json,sys; d=json.loads(sys.argv[1]); print(d.get(sys.argv[2],''))" "$1" "$2" 2>/dev/null; }
jdata() { python3 -c "import json,sys; d=json.loads(sys.argv[1]).get('data',{}); print(d.get(sys.argv[2],''))" "$1" "$2" 2>/dev/null; }

echo "========== 1. REST 基础与鉴权 =========="
H=$(curl -s -m 5 "$BASE/health")
[ "$(jget "$H" code)" = "0" ] && ok "health 免鉴权" || bad "health: $H"
# 记录并临时复位免鉴权开关，保证鉴权基线确定性（结束恢复）
RP_ORIG=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/system/settings" | jdata rest_public)
curl -s -m 5 -X PUT "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"rest_public":false}' "$BASE/api/v1/system/settings" >/dev/null
if [ -n "$KEY" ]; then
  C=$(curl -s -m 5 -o /dev/null -w "%{http_code}" "$BASE/api/v1/cameras")
  [ "$C" = "401" ] && ok "无 Key 访问 cameras -> 401" || bad "无 Key 应 401 实际 $C"
  for HDR in "X-API-Key: $KEY" "Authorization: Bearer $KEY"; do
    R=$(curl -s -m 5 -H "$HDR" "$BASE/api/v1/cameras")
    [ "$(jget "$R" code)" = "0" ] && ok "带鉴权(${HDR%%:*}) 200" || bad "带鉴权失败: $R"
  done
  R=$(curl -s -m 5 -H "X-API-Key: wrong-key" "$BASE/api/v1/cameras")
  [ "$(jget "$R" code)" = "40101" ] && ok "错误 Key -> 40101" || bad "错误 Key 应 40101"
  # 免鉴权开关：开->验证->关（恢复原状）
  curl -s -m 5 -X PUT "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"rest_public":true}' "$BASE/api/v1/system/settings" >/dev/null
  R=$(curl -s -m 5 "$BASE/api/v1/cameras")
  [ "$(jget "$R" code)" = "0" ] && ok "免鉴权开关-开 后无 Key 可访问" || bad "免鉴权未生效"
  curl -s -m 5 -X PUT "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"rest_public":false}' "$BASE/api/v1/system/settings" >/dev/null
  R=$(curl -s -m 5 "$BASE/api/v1/cameras")
  [ "$(jget "$R" code)" = "40101" ] && ok "免鉴权开关-关 恢复 401" || bad "关闭未恢复鉴权"
else
  skip "api_key 为空：跳过鉴权用例"
fi

echo "========== 2. 相机列表/详情/配置/控件/发现 =========="
CAMS=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras")
if echo "$CAMS" | grep -q '"code":0'; then
  N=$(echo "$CAMS" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['data']['items']))")
  [ "$N" -ge 1 ] && ok "相机列表 ($N 路)" || bad "无相机"
  ID=$(echo "$CAMS" | python3 -c "import json,sys; print(json.load(sys.stdin)['data']['items'][0]['id'])")
  D=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/$ID")
  for f in id name uuid online fps fps_actual resolution encoder device; do
    echo "$D" | grep -q "\"$f\"" && ok "相机详情含 $f" || bad "相机详情缺 $f"
  done
  CFG=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/$ID/config")
  for f in name width height fps bitrate_kbps gop input_format encoder uuid; do
    echo "$CFG" | grep -q "\"$f\"" && ok "config 含 $f" || bad "config 缺 $f"
  done
  CTL=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/$ID/controls")
  echo "$CTL" | grep -q '"items"' && ok "controls 返回" || bad "controls 失败: $CTL"
  DISC=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/system/discover")
  echo "$DISC" | grep -q '"code":0' && ok "设备发现接口" || bad "discover 失败: $DISC"
  echo "  (相机ID=$ID)"
else
  bad "cameras 接口: $CAMS"
fi

echo "========== 3. 抓拍与照片 =========="
PH=$(curl -s -m 5 -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"quality":85}' "$BASE/api/v1/cameras/1/photo")
[ "$(jget "$PH" code)" = "0" ] && ok "抓拍" || bad "抓拍: $PH"
LATEST=$(curl -s -m 5 -o /tmp/_t_latest.jpg -w "%{http_code}" "${AUTH[@]}" "$BASE/api/v1/cameras/1/photo/latest")
[ "$LATEST" = "200" ] && [ -s /tmp/_t_latest.jpg ] && ok "photo/latest 200 非空" || bad "latest 失败"
PL=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/1/photos?page=1&page_size=5")
if echo "$PL" | grep -q '"code":0'; then
  T=$(echo "$PL" | python3 -c "import json,sys; print(json.load(sys.stdin)['data']['total'])")
  [ "$T" -ge 0 ] && ok "照片列表 total=$T" || bad "照片列表异常"
  # 临时照片删除（安全：只删自己创建的）
  cp /tmp/_t_latest.jpg "/data/camera/photos/$(python3 -c "import json,sys; print(json.loads('''$CAMS''')['data']['items'][0]['name'])")/_test_del.jpg" 2>/dev/null
  DEL=$(curl -s -m 5 -X DELETE "${AUTH[@]}" "$BASE/api/v1/cameras/1/photos/_test_del.jpg")
  [ "$(jget "$DEL" code)" = "0" ] && ok "照片删除" || bad "照片删除: $DEL"
else
  bad "照片列表: $PL"
fi

echo "========== 4. 录像（fMP4/Range/列表） =========="
REC=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/1/recordings?page=1&page_size=3")
if echo "$REC" | grep -q '"items":\[\]'; then
  skip "暂无录像，跳过录像用例"
else
  RID=$(echo "$REC" | python3 -c "import json,sys; print(json.load(sys.stdin)['data']['items'][0]['id'])")
  ok "录像列表有数据 (rid=$RID)"
  HDR=$(curl -s -m 5 -D - -o /tmp/_t_seg.bin "${AUTH[@]}" -H "Range: bytes=0-8191" "$BASE/api/v1/recordings/$RID/download")
  echo "$HDR" | grep -qi "HTTP/1.1 206" && ok "Range 下载 206" || bad "Range 应 206"
  echo "$HDR" | grep -qi "Accept-Ranges" && ok "Accept-Ranges 头" || bad "缺 Accept-Ranges"
  head -c 8192 /tmp/_t_seg.bin | grep -q "moov" && ok "fMP4 moov 前置（录制中可播）" || bad "段文件头无 moov"
  # 当天(本地日期)录像窗口查询：回放页默认当天应能查到最新录像
  DAY0=$(python3 -c "import datetime; d=datetime.datetime.now().replace(hour=0,minute=0,second=0,microsecond=0); print(int(d.timestamp()*1000))")
  DAYREC=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/1/recordings?start=$DAY0&end=$((DAY0+86400000))&page=1&page_size=1")
  echo "$DAYREC" | grep -q '"items":\[\]' && skip "当天窗口暂无录像" || ok "当天(本地日期)录像窗口查询有数据"
fi

echo "========== 5. 存储/系统/日志/服务 =========="
ST=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/system/storage")
for f in total_bytes free_bytes used_percent recordings_count quota_bytes; do
  echo "$ST" | grep -q "\"$f\"" && ok "storage 含 $f" || bad "storage 缺 $f"
done
STAT=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/system/stats")
echo "$STAT" | grep -q '"cameras"' && ok "stats" || bad "stats: $STAT"
SET=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/system/settings")
echo "$SET" | grep -q '"nvr_name"' && ok "settings get" || bad "settings get"
# 统一配置：config.json 存在且含 system+用户字段（旧 config.ini/recorder.json 应已迁移）
if [ -f /opt/camera_server/config.json ]; then
  ok "统一配置 config.json 存在"
  python3 -c "import json; d=json.load(open('/opt/camera_server/config.json')); assert 'data_dir' in d and 'http_port' in d and 'api_key' in d and 'nvr_name' in d and 'cameras' in d, '字段缺失'"     && ok "config.json 含 system+用户字段" || bad "config.json 字段缺失"
  ls /opt/camera_server/config.ini >/dev/null 2>&1 && bad "旧 config.ini 未清理" || ok "旧 config.ini 已迁移清理"
else
  bad "缺少统一 config.json（应已从 config.ini 迁移）"
fi
SVC=$(curl -s -m 5 "http://127.0.0.1:8081/api/v1/system/service")
echo "$SVC" | grep -q '"active"' && ok "服务状态(Node)" || bad "服务状态: $SVC"
LOG=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/system/logs?page=1&page_size=5")
echo "$LOG" | grep -q '"items"' && ok "运行日志查询" || bad "logs: $LOG"
CLR=$(curl -s -m 5 -X POST "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"scope":"xxx"}' "$BASE/api/v1/system/clear")
[ "$(jget "$CLR" code)" = "40003" ] && ok "清空-非法 scope 拦截" || bad "清空防护: $CLR"

echo "========== 6. 网络：RTSP / ONVIF / NFS / mDNS =========="
RTSP_NAME=$(echo "$CAMS" | python3 -c "import json,sys; d=json.load(sys.stdin)['data']['items']; print(d[0]['name'] if d else 'CAM01')")
RT=$(timeout 8 ffprobe -v error -rtsp_transport tcp -i "rtsp://$HOST:8554/$RTSP_NAME" -show_entries stream=codec_name -of default=noprint_wrappers=1 2>&1)
echo "$RT" | grep -q "h264" && ok "RTSP 拉流($RTSP_NAME)" || bad "RTSP 拉流失败"
OV=$(curl -s -m 5 -X POST "$BASE/onvif/device_service" -H 'Content-Type: application/soap+xml' -d '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/></s:Body></s:Envelope>')
echo "$OV" | grep -qi "GetDeviceInformationResponse" && ok "ONVIF GetDeviceInformation" || bad "ONVIF: $(echo "$OV" | head -c 100)"
if command -v showmount >/dev/null 2>&1; then
  showmount -e "$HOST" 2>/dev/null | grep -q "/data" && ok "NFS 导出可见" || bad "NFS 导出未配置"
else
  skip "无 showmount"
fi
if command -v avahi-browse >/dev/null 2>&1; then
  if [ "$(systemctl is-active avahi-daemon 2>/dev/null)" = "active" ] && ls /etc/avahi/services/*.service >/dev/null 2>&1; then
    ok "avahi-daemon 运行且已配置服务"
    FOUND=0
    for i in 1 2 3; do
      timeout 8 avahi-browse -t _camera-server._tcp > /tmp/_t_mdns.txt 2>/dev/null || true
      if grep -qi "camera_server" /tmp/_t_mdns.txt; then FOUND=1; break; fi
      sleep 2
    done
    [ "$FOUND" = "1" ] && ok "mDNS _camera-server 广播可发现" || bad "mDNS 广播未发现(可手动 dns-sd -B _camera-server._tcp)"
  else
    bad "avahi-daemon 未运行或未配置服务"
  fi
else
  skip "无 avahi-browse"
fi

echo "========== 7. debug_check 13 项基线 =========="
cd "$(dirname "$0")" && ./debug_check.sh 2>&1 | grep -E "PASS|FAIL" > /tmp/_t_dc.txt || true
DCP=0; DCF=99
for i in 1 2 3; do
  cd "$(dirname "$0")" && ./debug_check.sh 2>&1 | grep -E "\[PASS\]|\[FAIL\]" > /tmp/_t_dc.txt || true
  DCP=$(grep -c "\[PASS\]" /tmp/_t_dc.txt); DCF=$(grep -c "\[FAIL\]" /tmp/_t_dc.txt)
  [ "$DCF" = "0" ] && break
  sleep 3
done
if [ "$DCF" = "0" ]; then
  ok "debug_check $DCP/$((DCP+DCF))"
else
  bad "debug_check 有 FAIL:"
  grep "\[FAIL\]" /tmp/_t_dc.txt | sed 's/^/    /'
fi

if [ "$DESTRUCTIVE" = "1" ]; then
  echo "========== 8. 破坏性用例（--destructive） =========="
  # 改名往返（目录迁移/DB 同步/OSD 联动）
  NAME0=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/1/config" | python3 -c "import json,sys; print(json.load(sys.stdin)['data']['name'])")
  curl -s -m 5 -X PUT "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"name":"回归测试名"}' "$BASE/api/v1/cameras/1/config" >/dev/null
  sleep 2
  R=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/1/config")
  [ "$(jdata "$R" name)" = "回归测试名" ] && ok "改名生效" || bad "改名失败($(jdata "$R" name))"
  ls -d "/data/camera/recordings/回归测试名" >/dev/null 2>&1 && ok "录像目录迁移" || bad "录像目录未迁移"
  curl -s -m 5 -X PUT "${AUTH[@]}" -H 'Content-Type: application/json' -d "{\"name\":\"$NAME0\"}" "$BASE/api/v1/cameras/1/config" >/dev/null
  sleep 2
  R2=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/1/config")
  [ "$(jdata "$R2" name)" = "$NAME0" ] && ok "改名恢复" || bad "改名恢复失败($(jdata "$R2" name))"
  # 录像删除（真删一条）
  RID2=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/cameras/2/recordings?page=1&page_size=1" | python3 -c "import json,sys; d=json.load(sys.stdin)['data']['items']; print(d[0]['id'] if d else '')")
  if [ -n "$RID2" ]; then
    curl -s -m 5 -X DELETE "${AUTH[@]}" "$BASE/api/v1/recordings/$RID2" >/dev/null
    R=$(curl -s -m 5 "${AUTH[@]}" "$BASE/api/v1/recordings/$RID2/download")
    [ "$(jget "$R" code)" = "40401" ] && ok "录像删除生效" || bad "录像删除未生效"
  else
    skip "无录像可删"
  fi
else
  echo "========== 8. 破坏性用例 =========="
  echo "  [SKIP] 需 --destructive（含改名往返/录像删除/清空等会改动数据的用例）"
fi

# 恢复免鉴权开关原状
if [ -n "$KEY" ] && [ "$RP_ORIG" = "true" ]; then
  curl -s -m 5 -X PUT "${AUTH[@]}" -H 'Content-Type: application/json' -d '{"rest_public":true}' "$BASE/api/v1/system/settings" >/dev/null
fi

echo
echo "=============================================="
echo " 结果: PASS=$PASS FAIL=$FAIL SKIP=$SKIP"
[ "$FAIL" = "0" ] && echo " 全部通过 ✅" || echo " 存在失败项，见上方 [FAIL] ⚠️"
echo "=============================================="
