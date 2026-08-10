# 摄像机系统（USB Camera + Jetson Xavier NX）

基于 **NVIDIA Jetson Xavier NX + USB UVC 摄像头** 的多路摄像、本地录像、网络 API 系统：
C++17 高性能核心（`camera_cpp/`）+ Node.js NVR Web 管理面，已在 Jetson 真机接入 2 路 1080p 摄像头完成联调。

## 目标平台与现状（2026-08 实测）

| 项 | 状态 |
|----|------|
| 平台 | Jetson Xavier NX（192.168.0.117，JetPack R35.6.2 / Ubuntu 20.04） |
| 相机 | 2 台 DHZJ-240627-K（Sunplus 1bcf:2281，无序列号），/dev/video0 + /dev/video2 |
| 采集编码 | GStreamer 全链路：v4l2src→jpegdec(多线程软解)→OSD叠加→nvvidconv→`nvv4l2h264enc`(NVENC 硬编)，2×1920×1080@30fps |
| 录像 | 分段 **fMP4**（moov 前置+按关键帧/2s 分片，录制中也可播）+ SQLite 索引 + 配额回收 |
| 协议 | RESTful :8000 / RTSP :8554 / ONVIF / MJPEG 实时流 / Web :8081（Node.js 海康风格） |
| 自检 | `debug_check.sh` 13/13 通过 |

> RK3568/RK3588 为早期选型调研（`docs/01~03`），当前实现与真机验证均以 **Jetson Xavier NX** 为准。

## 主要功能

- **录像**：多路独立编码参数、**1 分钟一段（按开始时间命名）**、IDR 开头可独立播放、UTC 时间戳、断线自动重连、配额循环回收
- **OSD 画面叠加**：时间戳（`%Y-%m-%d %H:%M:%S`）+ 相机名，录制/RTSP/预览统一，`[camera] osd=true/false`
- **照片事件标签**：抓拍接口支持 `tag`/`source` 参数，事件标签存 **SQLite photos 表**（文件名保持纯净 `<ts>.jpg`）；照片浏览页可按标签筛选（`?tag=`）、标签列表 `GET .../photos/tags`；照片仅在**手动或事件触发**时生成（无自动抓拍），索引与磁盘自动同步（外部删除清理孤儿行）
- **实时预览**：MJPEG multipart 流（~15fps，<100ms），浏览器原生播放，失败降级快照
- **相机管理**：名称自定义（持久化）、**UUID 设备绑定**、**热插拔**（新相机自动注册、拔线显示未连接且历史可浏览、插回自动恢复、无序列号换口自动重绑定）、手动添加/刷新/删除
- **Web 管理面**：实时预览 / 录像回放 / 图片浏览 / 相机配置（含添加删除）/ 存储清理 / 系统设置（服务控制+NVR名+**运行日志**）/ ONVIF
- **局域网发现**：mDNS（Avahi 广播 `_camera-server._tcp`/`_http._tcp`/`_rtsp._tcp`/`_smb._tcp`）；**macOS Finder 网络栏直接浏览录像/照片**（SMB 共享 /data，`setup_samba.sh` + `setup_mdns.sh`）
- **NFS 共享**：`/data` 局域网免认证直接读取录像/照片
- **操作日志**：系统动作（删照片/清数据/改名/录像启停/服务启停等）入 SQLite `oplog`，可查询

## 快速开始（Jetson 部署）

```bash
# 1) 部署（Mac 与 Jetson 同网段）
./camera_cpp/deploy/jetson/deploy.sh root@<jetson-ip>

# 2) 配置真实相机（capture=gst 全链路 + encoder=auto 走 NVENC）
#    统一配置：系统 + 用户（NVR 名称/相机）都在一个 config.json
cp /opt/camera_server/deploy/jetson/config.real.example.json /opt/camera_server/config.json
#    按实际填 device=/dev/videoN
systemctl restart camera_server

# 3) 验证
curl http://<jetson-ip>:8000/api/v1/cameras        # REST（含 fps_actual 实测帧率）
http://<jetson-ip>:8081                            # Web 管理面
bash /opt/camera_server/deploy/jetson/debug_check.sh   # 13 项快速自检
bash /opt/camera_server/deploy/jetson/test_all.sh         # 56 项全面回归（--destructive 含改名/删除等破坏性用例）

# 4) 可选：NFS 共享 / mDNS 发现
sudo /opt/camera_server/deploy/jetson/setup_nfs.sh
sudo /opt/camera_server/deploy/jetson/setup_mdns.sh
```

配置：**单一 `config.json`**（`/opt/camera_server/config.json`）同时管理系统配置
（data_dir/配额/分段/编码器/端口/api_key）与用户配置（`nvr_name`/`rest_public`/`cameras[]`）；
运行时改名/热插拔注册/免鉴权开关均原子写回同一文件，Web（Node）也从该文件读 `api_key`/`data_dir`。
旧 `config.ini` 与 `data_dir/recorder.json`（及更早 3 个 json）**首次启动自动迁移并入并删除**；
模板见 `deploy/jetson/config.jetson.example.json`、`config.real.example.json`。

---

# RESTful API 接口文档

## 通用约定

- 端口：**8000**（经 Node 代理 :8081 时路径不变，自动注入鉴权）
- 鉴权：`Authorization: Bearer <api_key>` 或 `X-API-Key: <api_key>`（api_key 在统一 `config.json`，空则关闭）
  - 豁免：`/health`、`/onvif/*`、Web 页面（`/`、`/index.html`）
  - **调试开关**：Web 系统页"NVR 名称 / 调试"勾选"启用免鉴权 RESTful 接口"（或 `PUT /api/v1/system/settings` 设 `rest_public:true`）后，所有 REST 免鉴权；持久化到统一 `config.json`，重启保留
- 响应格式统一：

```json
{ "code": 0, "message": "ok", "data": { } }
```

| code | 含义 |
|---|---|
| 0 | 成功 |
| 40001 | 相机不存在 |
| 40002 | 设备被占用 / 相机正在录像 / 未在录像 |
| 40003 | 参数无效（JSON 解析失败等） |
| 40101 | 认证失败 |
| 40401 | 记录/文件不存在 |
| 40501 | 方法不允许 |
| 50002 | 执行失败 |

- 时间戳均为 **UTC 毫秒**（epoch ms）；录像/照片文件名为 UTC 时间。

## 1. 健康检查

`GET /health` — 免鉴权
```json
{"code":0,"data":{"status":"ok","time":"..."}}
```

## 2. 相机管理

### 2.1 相机列表
`GET /api/v1/cameras`
```json
{"code":0,"data":{"items":[
  {"id":1,"name":"外部相机","device":"/dev/video0","online":true,"recording":true,
   "encoder":"gst-cap(jpegdec+NVENC)","resolution":"1920x1080","fps":30,
   "fps_actual":30.0,"bitrate_kbps":3000,"uuid":"b558d3ee-..."}]}}
```
> `online`：3 秒内有编码帧输出即在线（拔线自动变 false）；`fps_actual`：实测帧率（4s 窗口，停帧归零）。

### 2.2 相机详情
`GET /api/v1/cameras/{id}` — 同 2.1 单条。

### 2.3 添加相机（手动注册）
`POST /api/v1/cameras`
```json
{"device":"/dev/video0","name":"大门","width":1920,"height":1080,"fps":30,
 "bitrate_kbps":3000,"gop":60,"input_format":"mjpeg","capture":"gst","osd":true}
```
> 仅 `device` 必填；自动建目录/写入统一 `config.json`/开始录像；设备被占用返回 40002。
> 未注册设备列表见 `GET /api/v1/system/discover`。

### 2.4 删除相机（保留数据）
`DELETE /api/v1/cameras/{id}` — 停止录像、从系统/统一 `config.json` 移除，**录像/照片数据保留**。

### 2.5 设备发现（添加相机用）
`GET /api/v1/system/discover` — 返回未注册的 USB 相机：
```json
{"code":0,"data":{"items":[
  {"device":"/dev/videoN","uuid":"...","product":"...","manufacturer":"...","usb":"1-2.3",
   "width":1920,"height":1080,"fps":30,"input_format":"mjpeg"}]}}
```

### 2.6 相机参数配置（热重配）
`GET /api/v1/cameras/{id}/config`
```json
{"code":0,"data":{"name":"外部相机","device":"/dev/video0","width":1920,"height":1080,
 "fps":30,"bitrate_kbps":3000,"gop":60,"input_format":"mjpeg","encoder":"gst-cap(...)","uuid":"..."}}
```
`PUT /api/v1/cameras/{id}/config` — 同结构，改动即停流→重开→续录（画面短暂中断）；改名自动迁移目录并同步 DB 路径。

### 2.7 V4L2 图像控件
`GET /api/v1/cameras/{id}/controls` → `{items:[{id,name,min,max,step,default,value,menu}]}`
`PUT /api/v1/cameras/{id}/controls` — body `{"id":<控件id>,"value":<值>}`。

### 2.8 录像启停
`POST /api/v1/cameras/{id}/record/start` | `.../record/stop`

## 3. 抓拍与照片

| 接口 | 说明 |
|---|---|
| `POST /api/v1/cameras/{id}/photo` | 抓拍一帧，body `{"quality":90,"tag":"入侵报警","source":"event"}`；tag/source 存 SQLite photos 表，写 `<ts>.jpg` 并更新 `latest.jpg` |
| `GET /api/v1/cameras/{id}/photo/latest` | 最新抓拍图（image/jpeg） |
| `GET /api/v1/cameras/{id}/photos?page=1&page_size=24&tag=<标签>` | 照片列表 `{total,items:[{name,url,ts_ms,tag,source}]}`，支持按事件标签筛选 |
| `GET /api/v1/cameras/{id}/photos/tags` | 事件标签列表 `{tags:[...]}`（供筛选下拉） |
| `GET /api/v1/cameras/{id}/photos/<文件名>.jpg` | 照片文件 |
| `DELETE /api/v1/cameras/{id}/photos/<文件名>.jpg` | 删除照片（文件+索引） |

#### 3.1 带事件标签抓拍（事件触发）

事件触发或手动抓拍时，抓拍接口支持传入**事件标签**与**来源**（存 SQLite `photos` 表，文件名保持纯净 `<ts>.jpg`）：

```bash
curl -X POST -H "X-API-Key: camera2026" -H 'Content-Type: application/json' \
  -d '{"quality":90,"tag":"入侵报警","source":"event"}' \
  http://192.168.0.117:8000/api/v1/cameras/1/photo
```

响应：
```json
{"code":0,"data":{"camera_id":1,"name":"1786381576752.jpg","tag":"入侵报警",
 "source":"event","path":"/data/camera/photos/外部相机/latest.jpg",
 "url":"/api/v1/cameras/1/photo/latest"}}
```

- `tag`：事件标签（如 入侵报警/车辆闯入），存 `photos` 表；不传默认"手动抓拍"
- `source`：来源标识 `manual`（手动抓拍）/ `event`（事件触发）；**系统无自动抓拍**，照片仅在手动或事件触发时生成
- 按标签筛选：`GET /api/v1/cameras/1/photos?tag=<标签>`；标签列表：`GET /api/v1/cameras/1/photos/tags`
- 索引与磁盘自动同步：外部删除（NFS/清理）后自动清理孤儿索引行、新文件自动回填

## 4. 录像

### 4.1 录像列表
`GET /api/v1/cameras/{id}/recordings?start=<ms>&end=<ms>&page=1&page_size=20`
```json
{"code":0,"data":{"total":300,"page":1,"page_size":20,"items":[
  {"id":53636,"camera_id":1,"start_ms":1786374243000,"end_ms":1786374355000,
   "duration_s":112.3,"size_bytes":38005282,"url":"/api/v1/recordings/53636/download"}]}}
```

### 4.2 录像下载 / 播放
`GET /api/v1/recordings/{id}/download` — `video/mp4`，支持 `Range`（206 分段），浏览器 `<video>` 直接播放与拖动。

### 4.3 删除录像
`DELETE /api/v1/recordings/{id}`

## 5. 存储 / 系统状态

`GET /api/v1/system/storage`
```json
{"code":0,"data":{"mount":"/data/camera","total_bytes":...,"free_bytes":...,
 "used_percent":30.0,"recordings_count":726,"recordings_bytes":...,"quota_bytes":...}}
```
`GET /api/v1/system/stats` — `{cameras:[...], uptime_ms}`

## 6. 系统设置 / 服务 / 日志

| 接口 | 说明 |
|---|---|
| `GET/PUT /api/v1/system/settings` | NVR 名称 + 免鉴权开关：`{"nvr_name":"录像系统","rest_public":true}`（持久化到统一 config.json） |
| `GET /api/v1/system/service` | 服务状态（Node 侧 systemctl）：`{active,state,substate,since}` |
| `POST /api/v1/system/service` | 服务动作，body `{"action":"start\|stop\|restart"}`（Node 侧执行 systemctl） |
| `GET /api/v1/system/logs?page=1&page_size=30` | 操作日志（DB oplog + Node jsonl 合并）：`{total,items:[{ts_ms,camera_id,action,detail,result}]}` |
| `POST /api/v1/system/clear` | 清空数据，body `{"scope":"recordings\|all"}`（二次确认后调用；严格校验防误删） |

## 7. ONVIF / RTSP / 实时流

| 协议 | 地址 |
|---|---|
| ONVIF | `POST/GET http://<ip>:8000/onvif/device_service`（SOAP，WS-Security，免 API Key） |
| RTSP | `rtsp://<ip>:8554/<相机名>`（如 `rtsp://192.168.0.117:8554/外部相机`） |
| MJPEG 实时流 | `GET http://<ip>:8000/api/v1/cameras/{id}/mjpeg`（multipart/x-mixed-replace，`<img>` 直接播放） |

## 8. curl 示例

```bash
KEY="camera2026"; H="http://192.168.0.117:8000"

# 相机列表 / 详情
curl -H "X-API-Key: $KEY" $H/api/v1/cameras
curl -H "X-API-Key: $KEY" $H/api/v1/cameras/1

# 添加 / 删除相机
curl -X POST -H "X-API-Key: $KEY" -H 'Content-Type: application/json' \
  -d '{"device":"/dev/video3","name":"大门"}' $H/api/v1/cameras
curl -X DELETE -H "X-API-Key: $KEY" $H/api/v1/cameras/3

# 抓拍 / 照片
curl -X POST -H "X-API-Key: $KEY" -H 'Content-Type: application/json' \
  -d '{"quality":90,"tag":"入侵报警","source":"event"}' $H/api/v1/cameras/1/photo
curl -H "X-API-Key: $KEY" "$H/api/v1/cameras/1/photos?page=1&page_size=24"

# 录像列表 / 下载（Range）
curl -H "X-API-Key: $KEY" "$H/api/v1/cameras/1/recordings?page=1&page_size=20"
curl -H "X-API-Key: $KEY" -H "Range: bytes=0-1048575" $H/api/v1/recordings/53636/download -o part.mp4

# 配置 / 控件
curl -H "X-API-Key: $KEY" $H/api/v1/cameras/1/config
curl -X PUT -H "X-API-Key: $KEY" -H 'Content-Type: application/json' \
  -d '{"bitrate_kbps":4000}' $H/api/v1/cameras/1/config
curl -H "X-API-Key: $KEY" $H/api/v1/cameras/1/controls

# 系统
curl -H "X-API-Key: $KEY" $H/api/v1/system/storage
curl -H "X-API-Key: $KEY" $H/api/v1/system/logs
curl -X POST -H "X-API-Key: $KEY" -d '{"action":"restart"}' http://127.0.0.1:8081/api/v1/system/service
```

---

## 文档索引

| 文档 | 内容 |
|------|------|
| [docs/07-当前实现与运维手册.md](docs/07-当前实现与运维手册.md) | **当前实现**：部署/配置/热插拔/UUID/OSD/NFS/mDNS/运维排查 |
| [docs/08-全面测试用例清单.md](docs/08-全面测试用例清单.md) | **93 项全面回归测试用例**（A 自检 / B 编码 / C REST鉴权 / D 相机管理 / E Web / F 数据存储 / G 网络 / H 性能稳定 / I 配置运维）+ 历史问题索引 |
| [docs/01-硬件选型方案.md](docs/01-硬件选型方案.md) | 硬件选型（早期 RK 调研，设计参考） |
| [docs/02-软件架构与API设计.md](docs/02-软件架构与API设计.md) | 软件架构与 API 设计（早期设计） |
| [docs/03-OV9734与12路方案.md](docs/03-OV9734与12路方案.md) | OV9734、RK3568/RK3588、12 路专项（调研） |
| [docs/04-Jetson-Xavier-NX-4路Demo方案.md](docs/04-Jetson-Xavier-NX-4路Demo方案.md) | Jetson Xavier NX 4 路 Demo 方案 |
| [docs/05-C++高性能实现与ONVIF设计.md](docs/05-C++高性能实现与ONVIF设计.md) | C++ 架构、多厂商 USB 识别、ONVIF 设计 |
| [docs/06-Jetson官方方案JPS评估.md](docs/06-Jetson官方方案JPS评估.md) | Jetson 官方 JPS 方案评估 |
