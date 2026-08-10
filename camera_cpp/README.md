# camera_server（C++17 生产核心）

对应设计文档 [docs/05-C++高性能实现与ONVIF设计.md](../docs/05-C++高性能实现与ONVIF设计.md)。

## 当前进度

| 里程碑 | 状态 |
|--------|------|
| M1 多厂商 UVC 设备发现（sysfs 零依赖） | ✅ 已完成 |
| M2 媒体管道（V4L2/lavfi → FFmpeg 解码 → H.264 编码）+ 分段 MP4 + SQLite 索引 + 配额回收 + 抓拍 | ✅ 已完成（本机实测通过） |
| M3 ONVIF（Device/Media/WS-Discovery）+ RTSP（H.264 RTP） | ✅ 已完成（本机实测通过） |
| M4 RESTful（自研 HTTP + JSON，零依赖）+ 鉴权 | ✅ 已完成（本机实测通过） |

## 模块结构

```
src/
├── main.cpp                    # 装配/启动/热插拔线程/信号处理
├── camera_manager.{h,cpp}      # 相机 launch/remove 共用逻辑（启动/热插拔/REST 添加删除）
├── config.{h,cpp}              # 统一 config.json（系统+用户）解析/读写 + 旧 ini/recorder.json 迁移
├── db.{h,cpp}                  # SQLite(WAL) 录像索引 + 操作日志(oplog)
├── device/device_discovery.*   # 多厂商 UVC 发现（sysfs 零依赖）+ UUID
├── media/
│   ├── pipeline.h              # 管道接口（编码帧回调/抓拍/在线判定）
│   ├── ffmpeg_pipeline.cpp     # V4L2/lavfi → 解码 → 缩放 → H.264 编码
│   ├── gst_capture.cpp         # GStreamer 全链路采集（jpegdec+OSD+NVENC）
│   ├── gst_encoder.cpp         # GStreamer nvv4l2 编码后端
│   ├── recorder.{h,cpp}        # 分段 fMP4（IDR 开头/每段可独立解码）+ 索引 + 配额
│   └── v4l2_controls.*         # V4L2 图像控件
├── net/http_server.{h,cpp}     # 自研 HTTP（含流式路由 MJPEG）
├── rest/rest_service.cpp       # RESTful 全部接口
├── rtsp/rtsp_server.cpp        # RTSP H.264 实时流
├── onvif/onvif_service.cpp     # ONVIF SOAP + WS-Discovery
└── web/web_ui.cpp              # 旧简易页（当前以 Node webui 为主）
```

## 构建

```bash
cd camera_cpp
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

依赖：FFmpeg（含 libavdevice）、SQLite3、GStreamer（Jetson NVENC 硬编时必需）。Jetson Xavier NX 上 `encoder: auto` 自动走 GStreamer `nvv4l2h264enc`（NVENC 硬件编码）。

## 运行

```bash
# 列出本机 UVC 摄像头（多厂商）
./build/camera_server --list

# 2 路模拟相机，录 20 秒（无硬件也能跑）
./build/camera_server --config config.mock.json --duration 20 --snapshot

# 真实相机：把 mock=true 去掉，device 填 /dev/cam01...
```

配置文件示例（config.mock.json，**系统 + 用户配置统一在一个 JSON**）：

```json
{
  "data_dir": "/tmp/cam-demo",
  "quota_mb": 200,
  "segment_time_s": 5,
  "encoder": "auto",
  "http_port": 8000,
  "rtsp_port": 8554,
  "api_key": "",
  "autostart": true,
  "nvr_name": "camera_server NVR",
  "rest_public": false,
  "cameras": [
    { "id": 1, "uuid": "", "name": "CAM01", "device": "/dev/video0", "mock": true,
      "width": 1280, "height": 720, "fps": 30, "input_format": "mjpeg",
      "bitrate_kbps": 2000, "gop": 60, "capture": "auto", "osd": true, "enabled": true }
  ]
}
```

> 兼容：传入旧 `config.ini`（或目录下存在 `config.ini`）会自动迁移为同名 `config.json`；
> 旧 `data_dir/recorder.json` 等首次启动自动合并进 `config.json` 并删除。

## Jetson Xavier NX 部署

```bash
# 从 Mac 执行（jetson 需与电脑同网段）
./deploy/jetson/deploy.sh root@<jetson-ip>

# 部署后验证
curl http://<jetson-ip>:8000/api/v1/cameras          # REST
ffprobe -rtsp_transport tcp -i rtsp://<jetson-ip>:8554/CAM01   # RTSP
# ONVIF：用 ONVIF Device Manager 或
curl -X POST http://<jetson-ip>:8000/onvif/device_service \
  -H 'Content-Type: application/soap+xml' \
  -d '<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope"><s:Body><GetDeviceInformation xmlns="http://www.onvif.org/ver10/device/wsdl"/></s:Body></s:Envelope>'
```

- Jetson 上 `encoder: auto` 自动走 GStreamer `nvv4l2h264enc`（NVENC 硬编）；配合 `capture=gst` 实测 2×1080p@30fps；
- 配置模板：`deploy/jetson/config.jetson.example.ini`；先 `camera_server --list` 确认 `/dev/videoN`。

## 本机集成测试结果（mock，2026-08）

| 项 | 结果 |
|---|---|
| REST health/cameras/photo/photo-latest | ✅ |
| 录像列表/下载（含 Range 206）/删除 | ✅ |
| 存储状态/系统状态 | ✅ |
| API key 鉴权（Bearer / X-API-Key，错误 key 401，ONVIF 豁免） | ✅ |
| ONVIF GetDeviceInformation/GetProfiles/GetStreamUri/GetSnapshotUri | ✅ |
| WS-Discovery 单播 Probe | ✅（组播在当前热点网络被屏蔽，局域网/Linux 正常） |
| RTSP 拉流（ffprobe/ffmpeg，tcp） | ✅ 1280x720@30fps 可解码 |
| 双路分段录像（每段 IDR 开头可独立播放） | ✅ 8/8 段可解码 |
| 配额循环覆盖 | ✅ |

## NVR Web 管理面（Node.js 前端 + C++ 后端）

架构：**C++ 后端**（媒体/录像/RTSP/ONVIF/REST，:8000） + **Node.js 前端**（webui/，:8081）：
- Node 服务零依赖（仅 node:http），负责海康风格 NVR 界面 + 代理 `/api/*`、`/onvif/*` 到 C++ 后端（自动注入 API Key，浏览器无需带鉴权头）；
- 页面：**实时预览 / 录像回放（浏览器直接播 mp4，支持拖动）/ 图片浏览 / 相机配置（分辨率/帧率/码率热重配 + V4L2 亮度对比度等控件）/ 存储管理 / 系统设置 / ONVIF 开关**；
- **相机添加/删除**：相机配置页"添加相机"弹窗列出检测到的 USB 设备一键注册、支持刷新与删除（数据保留）；实时预览页离线相机卡片可直接删除；
- **mDNS 局域网发现**：`deploy/jetson/setup_mdns.sh` 配置 Avahi 广播 `_camera-server._tcp`（API:8000）、`_http._tcp`（Web:8081）、`_rtsp._tcp`（流:8554），macOS `dns-sd -B _camera-server._tcp` / Safari Bonjour 可直接发现；
- **实时预览 = MJPEG 实时视频流**（`GET /api/v1/cameras/<id>/mjpeg`，multipart/x-mixed-replace，~15fps，延迟 <100ms，浏览器原生 `<img>` 播放，无需插件）；gst 采集链路从叠加分支降采样 jpegenc（含 OSD），`osd=false` 时退化为相机 MJPG 直通零转码；流不可用时前端自动降级为 1.5s 快照轮询；切换页面时自动断开/重建连接，返回页面无需刷新浏览器；
- **OSD 画面叠加**：录制与预览统一叠加**时间戳 + 相机名称**（gst 链 `clockoverlay %Y-%m-%d %H:%M:%S` + `textoverlay`，NVENC 前），`[camera] osd=true/false` 每路可开关；
- **相机名称自定义 + UUID 绑定**：Web 相机配置改名即持久化（统一配置文件 `config.json`：系统+用户一体，旧 `recorder.json` 及更早 3 个 json 自动迁移并删除），自动迁移录像/照片目录并同步 DB 路径，重启不丢失；名称同时用于 OSD 显示与 **RTSP 流路径**（`rtsp://<ip>:8554/<相机名>`）；
- **设备 UUID + 热插拔**：有序列号的相机 UUID 由 `VID:PID+序列号` 派生（**换 USB 口不变**）；无序列号的含物理端口。启动按 UUID 绑定设备恢复自定义名；**热插拔监控**：新 USB 相机自动注册（建目录/管道/写入统一 config.json，Web 每 5s 自动刷新），拔线保留配置并显示**未连接**（历史录像/照片仍可浏览），插回同 UUID 自动恢复信号，设备换 `/dev/videoN` 自动重配；**无序列号相机换 USB 口**（新 UUID 但同型号）且恰好一台同型号离线 → 自动重绑定（保持原配置/名称/历史，不产生重复相机）；录像索引记录 `camera_uuid`；
- **运行日志**：系统页动作（删照片/清理数据/改名/参数/录像启停/抓拍/ONVIF/NVR 名称/服务启动停止重启）记录入 SQLite `oplog` 表（Node 服务动作写 `oplog_node.jsonl`），`GET /api/v1/system/logs` 合并查询，系统设置页展示最近 30 条；
- **录像回放**：录像为**分段 fMP4**（moov 前置 + 按关键帧/2s 分片），录制中的片段也可实时播放；HTTP Range 拖动进度；
- **图片浏览**：照片缩略图 + 时间/事件标签 + 删除；**相机配置**：并行加载 + V4L2 控件 5s 缓存，切换相机秒开；
- **存储管理**：二次确认清空录像 / 清空全部（录像+照片）；**系统设置**：camera_server 启动/停止/重启（Node 侧 systemctl）、状态刷新、**NVR 名称自定义**（`GET/PUT /api/v1/system/settings`）；
- 访问：`http://<jetson-ip>:8081`（登录 Key 即后端 `api_key`，不匹配时出现登录框）；
- systemd：`camera_server.service`（C++）+ `camera-web.service`（Node），均开机自启。

```bash
# 重新部署前端
rsync -az camera_cpp/webui/ nvda:/opt/camera_server/webui/
ssh nvda 'systemctl restart camera-web'
```

## 真实摄像头联调结果（2026-08-09，Sunplus 1bcf:2281 ×2）

已接 2 台物理摄像头（DHZJ-240627-K，经 Fresco Logic USB Hub，usb=1-2.2 与 1-2.4）：
- 可采集节点：CAM01=/dev/video0、CAM02=/dev/video2（每台相机的第二节点 video1/video3 无采集格式）；
- 实测两路均 nvv4l2(gst) 硬编录像、RTSP 双路可拉流、抓拍正常、debug_check 13/13 全通过；
- **设备发现修复**：`fs::canonical` 正确解析 sysfs 符号链接（此前相对路径解析错误导致 --list 返回 0）；
- **gst 硬编链路修复**（真实相机下 nvv4l2h264enc 无输出）：
  1. appsrc 改非 live + PTS 从 0 开始（NVIDIA 编码器对巨大初始 PTS 卡死）；
  2. 每段加 queue（消除 "Pipeline construction is invalid, please add queues"）；
  3. 首关键帧包剥离内联 SPS/PPS（avcC 已有，避免 mov muxer Invalid argument）；
  4. bus 错误日志（静默错误不再被吞）；
- **验证**：录像 30fps 硬编持续增长、已完成段可播放（17s/1.33Mbps）、RTSP 可拉流、V4L2 控件 14 项可调、debug_check 13/13 PASS；
- 注意：`/dev/video0` 与 `/dev/video1` 是**同一台相机的两个节点**，video1 无采集格式；当前只有 1 路真实相机，接入第 2 台后配置 `enabled=true` 即可。

## 1080p@30 提速（2026-08-09）

**问题**：2×1080p 软解只有 15-20fps（FFmpeg 单线程 jpegdec 是瓶颈，~100% CPU/路）。

**方案**：新增 GStreamer 全链路采集模式 `capture=gst`（v4l2src→jpegdec(多线程)→nvvidconv→NVENC→appsink）：
- **实测 2×1080p = 30fps、CPU 207% → 113%**（每路 ~56%）；
- 录像可播放（2.98Mbps@1080p）、RTSP 10s=300 帧、抓拍正常、debug_check 13/13；
- 另优化：FFmpeg 路径跳过 swscale 零拷贝直推（207%→193%）；
- 说明：nvjpeg C API 在本机（JetPack 5.6.2）无可用头文件/库；GStreamer nvjpegdec 插件实测更慢，故采用 jpegdec 多线程软解 + NVENC 硬编。

## Jetson 硬件编码（nvv4l2）

Jetson 上 FFmpeg 二进制不带 `h264_nvv4l2`（NVIDIA fork 需自行编译，且本网络 GitHub 不可达），
但 **GStreamer `nvv4l2h264enc` 硬件编码可用**。camera_server 已内置 GStreamer 编码后端：

```ini
encoder=auto    # Jetson 上 auto 自动探测: ffmpeg nvv4l2 > GStreamer nvv4l2 > libx264（实际走 GStreamer nvv4l2h264enc）
encoder=nvgst   # 强制 GStreamer nvv4l2h264enc（NVENC 硬件编码）
```

- 依赖：`nvidia-l4t-jetson-multimedia-api`（已装 35.6.5）+ GStreamer dev；
- CMake 自动检测 GStreamer 并启用（Mac/非 Jetson 编译时自动禁用，代码可移植）；
- 已验证：NVENC 时钟工作、HW 录像可播放、HW RTSP 流可解码、SPS/PPS 自动提取；
- **断线自动重连**：输入（USB 相机）断开后每 3 秒重试，摄像头热插拔无需重启服务；
- 说明：已接入 2 台真实 USB 摄像头（/dev/video0、/dev/video2），配置见 `/opt/camera_server/config.json`（`capture=gst`）。

## NFS 共享（外部直接读取录像/照片，免账号密码）

```bash
# 在 Jetson 上一键配置（默认共享 /data 给 192.168.0.0/24，无认证）
sudo /opt/camera_server/deploy/jetson/setup_nfs.sh

# 外部（Mac/Linux）挂载
mount -t nfs <jetson-ip>:/data /mnt/nfs
# 录像在 <mount>/camera/recordings/<相机名>/，照片在 <mount>/camera/photos/<相机名>/
# Mac Finder：前往 -> 连接服务器 -> nfs://<jetson-ip>:/data
```

> 说明：NFS 为 IP 网段白名单认证（免密码），默认仅允许局域网 192.168.0.0/24；如需扩大范围改 `/etc/exports` 后 `exportfs -ra`。
>
> **macOS Finder 浏览**：`setup_samba.sh` 提供 SMB 共享（guest，`smb://<ip>` 以游客连接），配合 `setup_mdns.sh` 的 `_smb._tcp` 广播，Finder「前往 → 网络」即可看到设备并浏览录像/照片。

## Jetson 软件调试工作流（插好摄像头后）

```bash
# 0) 插上摄像头，确认枚举
lsusb; ls /dev/video*

# 1) 一键生成稳定设备名（多厂商通用，按序列号/物理端口绑定 -> /dev/cam01...）
sudo /opt/camera_server/deploy/jetson/make_udev_rules.sh
# 或直接用系统自带稳定路径：ls /dev/v4l/by-id/ 里选对应的 -video-index0

# 2) 改真实配置（模板已备好）
cp /opt/camera_server/deploy/jetson/config.real.example.json /opt/camera_server/config.json
vi /opt/camera_server/config.json         # 按实际 /dev/cam0x 填 device
systemctl restart camera_server

# 3) 四查自检（设备/硬编/服务/协议，输出 PASS/FAIL）
sudo /opt/camera_server/deploy/jetson/debug_check.sh

# 4) 手动抽查
curl http://<ip>:8000/api/v1/cameras
ffprobe -rtsp_transport tcp -i rtsp://<ip>:8554/CAM01
```


## Jetson Xavier NX 真机集成测试结果（2026-08，tegra-ubuntu / JetPack R35.6.2）

环境：aarch64，Ubuntu 20.04，cmake 3.16，FFmpeg 4.2（Ubuntu 版，无 nvv4l2 → 用 libx264 软编）。
部署：`rsync` 同步源码 → `cmake` 构建 → systemd 服务（`camera_server.service`，开机自启）。

| 项 | 结果 |
|---|---|
| 编译（GCC 9 + FFmpeg 4.2） | ✅（修复 cmake≥3.16、`<mutex>` 显式包含、`av_err2str` 临时数组问题） |
| REST health/cameras/photo/photo-latest/recordings/download | ✅ 从 Mac 访问 192.168.0.117:8000 |
| 录像落盘 | ✅ 96 段 / 109MB @ /data/camera/recordings，下载可播放（5.999s） |
| ONVIF Device/Media/GetStreamUri | ✅ |
| WS-Discovery 单播 + **组播** | ✅ 局域网组播 239.255.255.250:3702 正常响应 |
| RTSP 实时流 | ✅ 从 Mac ffprobe 拉到 1280x720@30fps |
| systemd | ✅ active，8000/8554 监听 |

> 说明：早期（未接摄像头）用 mock 双路验证全服务栈；现已接入 2 台真实 UVC 摄像头并完成真机联调（见上文「真实摄像头联调结果」与「1080p@30 提速」）。

## M2 关键设计（已在实现中验证）

- **每路相机独立编码参数建流**：分辨率/码率/SPS 不同，Recorder 按 camera_id 区分，避免串流；
- **分段以 IDR 开头**：轮转时先封旧段再开新段，关键帧作为新段首帧，每段可独立解码播放；
- **编码器 GLOBAL_HEADER**：SPS/PPS 进 avcC，各段自含参数；
- **时间戳统一 UTC**：文件名、DB 索引均为 UTC epoch 毫秒，无 8 小时时区偏差；
- **mock 实时节流**：lavfi 源按已编码帧数节流到目标帧率（真实相机天然实时）。
