# 摄像机系统软件架构与 RESTful API 设计
> [!NOTE]
> **设计/调研文档（历史）**：本文档为早期设计与选型参考，**不代表当前实现**。
> 当前已实现并部署的系统见 [README.md](../README.md) 与 [07-当前实现与运维手册.md](07-当前实现与运维手册.md)。


## 1. 需求分解

| 编号 | 需求 | 说明 |
|------|------|------|
| R1 | 多路同时录像 | 多路 USB 摄像头并行采集、编码、落盘 |
| R2 | 本地硬盘存储 | 本地 SSD 存储，配额管理与循环覆盖 |
| R3 | 网络通信 | 设备提供 RESTful API（HTTP/JSON），供上位机/云端调用 |
| R4 | 拍照 API | 调用接口抓拍一张照片并返回 |
| R5 | 录像 API | 开始/停止录像、查询/下载历史录像 |
| R6 | 可运维 | 状态查询、健康检查、异常自恢复 |

## 2. 总体架构

```mermaid
flowchart TB
    subgraph 设备端（Jetson 板卡）
      subgraph 采集层
        V4L2[V4L2 采集服务<br/>uvcvideo 驱动 /dev/videoN]
      end
      subgraph 媒体处理层
        DEC[JPEG 硬件解码 / GPU 格式转换]
        ENC[NVIDIA H.264 硬件编码]
      end
      subgraph 应用层
        REC[录像服务<br/>MP4 分段写入]
        SNAP[抓拍服务]
        MGR[存储管理<br/>配额/循环覆盖]
        API[RESTful API 服务]
        MON[监控 / 日志 / 告警]
      end
      V4L2 --> DEC --> ENC --> REC
      ENC --> SNAP
      REC --> DISK[(本地硬盘 /data)]
      DISK --> MGR
      MGR --> API
      SNAP --> API
      API --> DB[(SQLite 索引)]
    end
    CLIENT[上位机 / 客户端 / 云端] -- HTTPS/REST --> API
    RTSP[RTSP 实时预览（可选）] --> CLIENT
```

**关键设计原则**：媒体链路全走 Jetson 硬件/GPU（V4L2 → nvjpeg 解码 → nvvidconv 转换 → nvv4l2h264enc 编码），应用层只做调度与业务，保证多路并发 CPU 占用极低。

## 3. 软件模块设计

### 3.1 采集模块（V4L2）

- 每路摄像头一个采集线程/进程（多进程隔离更稳，推荐）。
- 通过 `uvcvideo` 驱动打开 `/dev/videoN`，支持热插拔与 udev 规则绑定（按 USB 序列号/端口号固定设备名，如 `/dev/cam01`）。
- 采集格式优先级：**相机 H.264 直出 > MJPEG > YUY2**（按摄像头能力探测）。
- 掉线自动重连：监控 `/dev` 变化 + 定时探测，断线重连并补齐录像索引。

### 3.2 编码模块（NVIDIA 硬件编码）

- **MJPEG 输入**：nvjpeg GPU 解码 → nvvidconv 转 NV12 → nvv4l2h264enc 硬编（`h264_nvv4l2`）。
- **H.264 直出相机**：跳过板端编码，直接封装（省 CPU、降延迟）。
- 码率控制：CBR（恒定码率）便于容量规划，1080p30 建议 4~8 Mbps；GOP 建议 60（2 秒）。
- 每路独立配置（分辨率/帧率/码率/编码质量）。

### 3.3 录像与存储管理

- **分段**：每 1~5 分钟一个 MP4 段，`moov` 前置（faststart），断电后已写段落可播放。
- **文件命名**：`<相机ID>/<YYYYMMDD>/<HHMMSS>.mp4`，例如 `CAM01/20260806/140000.mp4`。
- **索引**：SQLite 记录相机、起止时间、时长、大小、路径、事件标签，供 API 查询。
- **循环覆盖**：按总配额（如 4TB）从最旧录像开始删除，保证磁盘不写满；空间 <10% 告警。
- **掉电恢复**：启动时扫描目录重建/修复索引，清理未封口的临时文件。
- **可选扩展**：事件录像（移动侦测/NPU 触发）带 pre/post 缓冲。

### 3.4 抓拍模块

- 从编码器当前帧取帧（不打断录像）→ JPEG 编码（质量 80~95 可配）。
- 保存到 `/data/photos/<相机ID>/<YYYYMMDD>/<HHMMSS>.jpg`，同时入库。
- 支持抓拍后通过 URL 直接访问/下载最新照片。

### 3.5 RESTful API 服务

- 框架：**FastAPI（Python）+ uvicorn**（推荐，开发快、自动生成 OpenAPI 文档），或 Go（Gin）单二进制。
- 认证：`Authorization: Bearer <api_key>`，可选双向 TLS（HTTPS）。
- 长耗时操作（如导出录像）异步处理，返回任务 ID 轮询。
- 自动生成 Swagger 文档：`http://<ip>:8000/docs`。

### 3.6 可选增值模块

| 模块 | 说明 |
|------|------|
| RTSP 实时预览 | mediamtx + nvv4l2decoder/nvvidconv，H.264 推流，支持浏览器/VLC 播放 |
| Web 管理界面 | 相机增删改、实时预览、录像回放、系统监控 |
| 移动侦测 | GPU/CPU 做 ROI 差分，触发抓拍/事件录像 |
| 云对接 | 录像/照片定时上传 OSS/S3，或对接平台 |

## 4. 技术栈选型

> **生产路线（已定）**：核心程序用 **C/C++ 高性能实现**（详见 [05-C++高性能实现与ONVIF设计.md](05-C++高性能实现与ONVIF设计.md)），并支持 **ONVIF（优先）+ RESTful** 双协议；本节的 Python/FastAPI 方案仅作为**接口验证原型**（camera-recorder/），生产代码以 `camera_cpp/` 为准。


| 层 | 方案 A（Python，推荐快速落地） | 方案 B（Go） |
|----|-------------------------------|--------------|
| 采集 | OpenCV / PyAV（V4L2） | v4l2 绑定库 |
| 编码 | FFmpeg + `h264_nvv4l2` / GStreamer nvv4l2 | FFmpeg（cgo） |
| API | FastAPI + uvicorn + pydantic | Gin / Echo |
| 索引 | SQLite（aiosqlite） | SQLite |
| 部署 | systemd + Docker（可选） | 单二进制 + systemd |

> 推荐 **方案 A**：媒体路径全部走 FFmpeg/GStreamer 的 nvv4l2 硬件编码保证性能，业务逻辑用 Python 快速迭代；若发现瓶颈（如 MJPEG 解码），再对底层做 C 扩展或换 Go。

> **平台可替换**：若 Demo 改用 Jetson Xavier NX（如手头已有设备），架构不变，仅把媒体插件换成 NVIDIA 的 `nvjpeg` / `nvv4l2h264enc`，详见 [04-Jetson-Xavier-NX-4路Demo方案.md](04-Jetson-Xavier-NX-4路Demo方案.md)。

### 4.1 开源复用 vs 自研边界

**结论：应用层从零开发（约 2000~5000 行核心代码），底层全部复用成熟开源组件，不建议在现有 NVR 项目上大改。**

| 层 | 复用开源组件 | 是否自研 |
|----|--------------|----------|
| 内核驱动 | Linux V4L2 / uvcvideo（内核自带，UVC 免驱） | 复用 |
| 硬件编解码库 | **NVIDIA V4L2/DeepStream**（nvv4l2h264enc / nvjpeg / nvvidconv，JetPack 自带） | 复用 |
| 媒体框架 | **FFmpeg**（`h264_nvv4l2` 编码器）/ **GStreamer**（nvv4l2h264enc 插件） | 复用 |
| 采集调度 | — | **自研**：多路采集服务、掉线重连、udev 设备命名 |
| 录像落盘 | — | **自研**：MP4 分段、SQLite 索引、配额循环覆盖、掉电恢复 |
| 拍照/API | FastAPI + uvicorn + pydantic + SQLite（均开源） | **自研业务**：接口、认证、任务管理 |
| RTSP 预览（可选） | mediamtx（MIT） | 复用 |
| 进程/服务管理 | systemd | 复用 |

**为什么应用层自研，而不是改现有 NVR 开源项目？**

| 开源 NVR | 主要问题 |
|----------|----------|
| ZoneMinder / Motion | 传统架构，面向 RTSP/IP 摄像头；USB 摄像头支持弱；不做 Jetson 硬件编码，8~12 路 CPU 扛不住 |
| Shinobi | 功能全但依赖重（Node.js），硬件加速适配与多 USB 摄像头调度不匹配本场景 |
| Frigate | 面向 RTSP + AI 检测；可借鉴其 FFmpeg 硬件加速做法，但其 USB 摄像头 + 纯录像 + REST 拍照 API 形态不符 |
| DeepStream 例程（deepstream-app） | 面向 IP/RTSP 相机 + AI 分析，非纯 USB 录像服务；多路 USB 采集 + 分段录像需自行封装 |

**自研可借用的参考实现**
- NVIDIA DeepStream 多路编码/解码例程（deepstream-app / nvv4l2 例程）；
- Frigate 的 FFmpeg 硬件加速配置思路（`hwaccel nvdec` / `h264_nvv4l2`）；
- 许可证提醒：NVIDIA JetPack/DeepStream 组件按 NVIDIA EULA 使用、FFmpeg 为 LGPL/GPL（编译选项决定）、GStreamer/mediamtx/FastAPI 为宽松许可，闭源商用需按各自条款合规（动态链接 LGPL 库通常可行）。

## 5. 数据模型（SQLite）

```sql
-- 相机配置表
CREATE TABLE cameras (
  id          INTEGER PRIMARY KEY,
  name        TEXT NOT NULL,          -- 例如 CAM01
  device      TEXT NOT NULL,          -- /dev/cam01 或 /dev/video0
  format      TEXT DEFAULT 'mjpeg',   -- h264 / mjpeg / yuy2
  width       INTEGER DEFAULT 1920,
  height      INTEGER DEFAULT 1080,
  fps         INTEGER DEFAULT 30,
  bitrate_kbps INTEGER DEFAULT 6000,
  enabled     INTEGER DEFAULT 1,
  created_at  TEXT
);

-- 录像索引表
CREATE TABLE recordings (
  id          INTEGER PRIMARY KEY,
  camera_id   INTEGER NOT NULL,
  start_time  TEXT NOT NULL,
  end_time    TEXT,
  duration_s  REAL,
  size_bytes  INTEGER,
  path        TEXT NOT NULL,
  event       TEXT DEFAULT '',        -- 事件标签，如 motion / manual
  FOREIGN KEY (camera_id) REFERENCES cameras(id)
);

-- 抓拍索引表
CREATE TABLE snapshots (
  id          INTEGER PRIMARY KEY,
  camera_id   INTEGER NOT NULL,
  time        TEXT NOT NULL,
  size_bytes  INTEGER,
  path        TEXT NOT NULL
);

-- 事件表
CREATE TABLE events (
  id          INTEGER PRIMARY KEY,
  camera_id   INTEGER,
  type        TEXT,                   -- photo / record / motion / alarm
  time        TEXT,
  payload     TEXT                    -- JSON 扩展字段
);
```

## 6. RESTful API 规范

- **Base URL**：`http://<device-ip>:8000/api/v1`
- **认证**：`Authorization: Bearer <api_key>`（未配置 key 时可关闭）
- **数据格式**：请求/响应均为 JSON；响应统一包装：

```json
{ "code": 0, "message": "ok", "data": { ... } }
```

### 6.1 接口总览

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/health` | 健康检查 |
| GET | `/cameras` | 相机列表与在线状态 |
| GET | `/cameras/{id}` | 单相机详情 |
| POST | `/cameras/{id}/photo` | **抓拍一张照片** |
| GET | `/cameras/{id}/photo/latest` | 获取最新照片（返回文件流） |
| POST | `/cameras/{id}/record/start` | 开始录像（可指定时长） |
| POST | `/cameras/{id}/record/stop` | 停止录像 |
| GET | `/cameras/{id}/recordings` | 查询录像记录（时间范围/分页） |
| GET | `/recordings/{rec_id}/download` | 下载/播放录像文件 |
| DELETE | `/recordings/{rec_id}` | 删除录像 |
| GET | `/system/storage` | 存储与留存状态 |
| GET | `/system/stats` | CPU/内存/温度/网络状态 |
| PUT | `/cameras/{id}/config` | 修改相机/编码配置 |

### 6.2 抓拍照片（核心接口 R4）

```
POST /api/v1/cameras/1/photo
Authorization: Bearer <api_key>
Content-Type: application/json
```

```json
// 请求体
{ "quality": 90 }
```

```json
// 响应 200
{
  "code": 0,
  "message": "ok",
  "data": {
    "camera_id": 1,
    "time": "2026-08-06T14:00:00.123+08:00",
    "size_bytes": 245678,
    "path": "/data/photos/CAM01/20260806/140000.jpg",
    "url": "/api/v1/cameras/1/photo/latest"
  }
}
```

### 6.3 开始 / 停止录像（R5）

```
POST /api/v1/cameras/1/record/start
```

```json
// 请求体（duration_s 为 0 表示一直录到 stop）
{ "bitrate_kbps": 6000, "duration_s": 0, "event": "manual" }

// 响应
{
  "code": 0,
  "data": {
    "session_id": "a3f9...",
    "camera_id": 1,
    "start_time": "2026-08-06T14:00:00+08:00"
  }
}
```

```
POST /api/v1/cameras/1/record/stop
// 请求体（可选）
{ "session_id": "a3f9..." }

// 响应
{
  "code": 0,
  "data": {
    "session_id": "a3f9...",
    "end_time": "2026-08-06T14:05:00+08:00",
    "duration_s": 300.0,
    "files": ["/data/recordings/CAM01/20260806/140000.mp4"]
  }
}
```

### 6.4 查询录像（R5）

```
GET /api/v1/cameras/1/recordings?start=2026-08-06T00:00:00%2B08:00&end=2026-08-07T00:00:00%2B08:00&page=1&page_size=20
```

```json
{
  "code": 0,
  "data": {
    "total": 120,
    "page": 1,
    "page_size": 20,
    "items": [
      {
        "id": 1024,
        "camera_id": 1,
        "start_time": "2026-08-06T14:00:00+08:00",
        "end_time": "2026-08-06T14:05:00+08:00",
        "duration_s": 300.0,
        "size_bytes": 225000000,
        "event": "manual",
        "url": "/api/v1/recordings/1024/download"
      }
    ]
  }
}
```

### 6.5 下载 / 播放录像

```
GET /api/v1/recordings/1024/download
```
- 成功：`200`，`Content-Type: video/mp4`，`Content-Disposition: attachment; filename="CAM01_20260806_140000.mp4"`，支持 `Range`（HTTP 分段）以便浏览器拖进度。
- 失败：`404`，录像不存在或已被循环覆盖。

### 6.6 存储状态

```
GET /api/v1/system/storage
```

```json
{
  "code": 0,
  "data": {
    "mount": "/data",
    "total_gb": 3725,
    "used_gb": 1800,
    "free_gb": 1925,
    "used_percent": 48.3,
    "retention_days_est": 5.5,
    "write_speed_mbps": 450,
    "overwrite": true
  }
}
```

### 6.7 错误码约定

| code | 含义 |
|------|------|
| 0 | 成功 |
| 40001 | 相机不存在或离线 |
| 40002 | 相机忙（正在录像/抓拍） |
| 40003 | 参数错误 |
| 40101 | 认证失败 |
| 40401 | 资源不存在（录像/照片被清理） |
| 50001 | 存储不可用或空间不足 |
| 50002 | 内部错误 |

## 7. 录像文件组织

```
/data
├── recordings/              # 录像
│   └── CAM01/
│       └── 20260806/
│           └── 140000.mp4
├── photos/                  # 抓拍
│   └── CAM01/
│       └── 20260806/
│           └── 140000.jpg
├── db/
│   └── records.db           # SQLite 索引
└── logs/
    └── camera-svc.log
```

## 8. 资源预算（8 路 1080p30）

| 项目 | 预算 | 说明 |
|------|------|------|
| CPU | <10% | 全硬件媒体链路 |
| 内存 | <1GB | 每路缓冲区 + 应用 |
| 磁盘写入 | 4~8 MB/s | 6 Mbps/路 × 8 路 |
| 12 路扩展（720p） | CPU <20%、写入 2~5 MB/s | 1.5~3 Mbps/路 × 12 路（OV9734，详见 03 文档） |
| 网络 | 低 | API 为主；RTSP 预览另计 |
| 启动恢复 | <2 分钟 | 索引重建 + 自动恢复录像 |

## 9. 实施里程碑

| 阶段 | 周期 | 交付 |
|------|------|------|
| M1 原型 | 2 周 | Jetson Xavier NX + 2 路 C920：采集 → 硬件编码 → MP4 分段落盘；`/photo`、`/recordings`、`/download` 接口可用 |
| M2 完善 | 2 周 | 8 路支持、存储配额循环覆盖、掉电恢复、RTSP 预览、基础 Web UI、系统监控 |
| M3 工程化 | 3~4 周 | 量产板卡适配、老化测试（7×24）、部署脚本/镜像、安全加固（HTTPS/API Key）、文档 |

## 10. 参考代码骨架

### 10.1 FastAPI 服务（示意）

```python
# app/main.py
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel
from typing import Optional

app = FastAPI(title="Camera Recording API", version="1.0.0")


class PhotoRequest(BaseModel):
    quality: int = 90  # JPEG 质量 1-100


class RecordStartRequest(BaseModel):
    bitrate_kbps: int = 6000
    duration_s: int = 0
    event: str = "manual"


@app.get("/api/v1/health")
def health():
    # 检查采集/编码/存储各服务状态
    return {"code": 0, "message": "ok", "data": {"status": "ok"}}


@app.get("/api/v1/cameras")
def list_cameras():
    # 从 cameras 表 + 运行时状态聚合
    return {"code": 0, "data": {"items": []}}


@app.post("/api/v1/cameras/{camera_id}/photo")
def take_photo(camera_id: int, req: PhotoRequest):
    # 1) 从该相机的编码器取当前帧
    # 2) 编码 JPEG 并写入 /data/photos/...
    # 3) 写入 snapshots 表
    # 4) 返回照片信息
    if camera_id not in CAMERA_MANAGER:
        raise HTTPException(status_code=404, detail="camera not found")
    info = CAMERA_MANAGER[camera_id].snapshot(quality=req.quality)
    return {"code": 0, "data": info}


@app.post("/api/v1/cameras/{camera_id}/record/start")
def record_start(camera_id: int, req: RecordStartRequest):
    session = CAMERA_MANAGER[camera_id].start_record(
        bitrate_kbps=req.bitrate_kbps,
        duration_s=req.duration_s,
        event=req.event,
    )
    return {"code": 0, "data": session}
```

### 10.2 媒体管道（FFmpeg + nvv4l2，示意）

```bash
# 方式 A：相机 H.264 直出（推荐，无解码开销）→ 直接硬件转封装分段
ffmpeg -f v4l2 -input_format h264 -video_size 1920x1080 -framerate 30 \
  -i /dev/cam01 -c:v copy -f segment -segment_time 300 -reset_timestamps 1 \
  -strftime 1 "/data/recordings/CAM01/%Y%m%d/%H%M%S.mp4"

# 方式 B：MJPEG 输入 → 软件解码（小规模）→ nvv4l2 硬件编码 H.264
ffmpeg -f v4l2 -input_format mjpeg -video_size 1920x1080 -framerate 30 \
  -i /dev/cam01 -c:v h264_nvv4l2 -b:v 6M -g 60 \
  -f segment -segment_time 300 -reset_timestamps 1 \
  -strftime 1 "/data/recordings/CAM01/%Y%m%d/%H%M%S.mp4"
```

> 生产环境若相机仅支持 MJPEG，建议用 GStreamer `nvjpeg`/`nvvidconv` 插件走 **GPU JPEG 解码 + 格式转换**，避免 8 路软件解码占用 CPU。

## 11. 风险与对策（软件侧）

| 风险 | 对策 |
|------|------|
| 摄像头掉线 | udev 绑定固定设备名 + 断线自动重连 + 告警 |
| 掉电丢数据 | 短分段 + moov 前置 + 启动修复 + UPS |
| 磁盘写满 | 配额循环覆盖 + 空间告警（API 暴露） |
| 时间不同步 | NTP/PTP + 统一 UTC 时间戳 |
| 多进程并发写 SQLite | 单写连接 / WAL 模式 / 队列 |
| API 暴露公网 | HTTPS + API Key + IP 白名单 + 限流 |

## 12. 12 路专项软件设计（OV9734 / Jetson）

| 要点 | 设计 |
|------|------|
| 进程模型 | 每路独立采集+编码进程（12 个 worker），单路崩溃/卡死不拖垮其他路 |
| 设备管理 | udev 规则按 USB 端口/序列号固定 `/dev/cam01~cam12`；热插拔自动重连 |
| 资源隔离 | systemd 对每个 worker 做 CPU 配额/内存上限/文件句柄限制 |
| 保活 | 看门狗（watchdog）监控 12 路心跳，异常自动拉起进程 |
| 时间同步 | NTP 校时 + 每帧打 UTC 时间戳；API 侧统一返回 +08:00 |
| 数据库 | SQLite 开 WAL、单写连接，避免 12 路并发写冲突 |
| 录像段 | 1~5 分钟分段、moov 前置，掉电只丢 ≤1 段 |
| 磁盘 | 配额循环覆盖（默认按天数/总量），<10% 空间告警 |
| 监控 | 帧率/掉帧数/温度/磁盘/在线状态，暴露到 `/system/stats` |
| 验收 | 12 路同时录像 7×24 掉帧率 <0.1%，单路热插拔不影响其余 11 路 |

---
*文档版本：v1.0（2026-08）*
