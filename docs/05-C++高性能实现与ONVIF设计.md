# C++ 高性能实现 + 多厂商摄像头兼容 + ONVIF 设计
> [!NOTE]
> **设计/调研文档（历史）**：本文档为早期设计与选型参考，**不代表当前实现**。
> 当前已实现并部署的系统见 [README.md](../README.md) 与 [07-当前实现与运维手册.md](07-当前实现与运维手册.md)。


> 本方案回答三条新要求：
> 1. 核心程序用 **C/C++** 高性能实现；
> 2. USB 摄像头识别**兼容多厂商**（UVC 通用协议，OV9734 只是其中之一）；
> 3. 与上位机通信**ONVIF 优先 + RESTful 并存**，ONVIF 在开源（onvif_srvd/gSOAP）基础上修改。

## 1. 总体架构（C++17 单主进程 + 多线程）

```mermaid
flowchart TB
    subgraph 核心进程 camera_server (C++17)
      DEV[设备发现服务<br/>libusb + sysfs + V4L2 能力探测]
      PIP[媒体管道<br/>V4L2 → NVIDIA 硬编 → H.264]
      REC[录像服务<br/>MP4 分段 + SQLite 索引]
      SNAP[抓拍服务<br/>nvjpeg GPU JPEG]
      RTSP[RTSP 流服务<br/>mediamtx 进程]
      ONVIF[ONVIF 服务<br/>gSOAP/onvif_srvd 改造]
      REST[RESTful 服务<br/>Drogon]
      MGR[存储管理<br/>配额循环覆盖]
    end
    USB1..N[USB 摄像头 1..N<br/>多厂商 UVC] --> DEV
    DEV --> PIP
    PIP --> REC --> MGR
    PIP --> SNAP
    PIP --> RTSP
    DEV --> ONVIF
    ONVIF --> RTSP
    ONVIF --> REST
    REC --> DB[(SQLite)]
    HOST[上位机/平台] -- ONVIF 优先 --> ONVIF
    HOST -- RESTful 补充 --> REST
    HOST -- RTSP 拉流 --> RTSP
```

**设计原则**
- 单一核心进程 `camera_server`（C++17），媒体/协议/存储模块化；
- 每路摄像头一个采集线程 + NVIDIA 多通道硬件编码，避免多进程开销；必要时 12 路拆两个进程；
- ONVIF 与 RESTful 共用一套相机注册表、用户表、存储索引。

## 2. 模块选型（开源组件）

| 模块 | 开源选型 | 说明 |
|------|----------|------|
| 语言/构建 | C++17 / GCC 12 + CMake | vcpkg 管理依赖 |
| 设备发现 | **libusb** + sysfs + V4L2 ioctl | 多厂商 UVC 通用识别（见 §3） |
| 视频采集 | V4L2（mmap，uvcvideo 免驱） | |
| 硬件编码 | **NVIDIA V4L2**（nvv4l2h264enc） | h264/h265 硬编，20×1080p30（Xavier NX 16G） |
| 格式转换 | nvvidconv / libyuv | NV12 转换/缩放 |
| 封装 | FFmpeg libavformat 或自研 MP4 muxer | 分段 MP4、moov 前置 |
| 抓拍 | nvjpeg（GPU JPEG 编码） | 不打断录像 |
| **ONVIF** | **onvif_srvd（gSOAP）fork 修改** | Profile S + WS-Discovery |
| RTSP | **mediamtx**（MIT，独立进程）或 live555 | |
| RESTful | **Drogon**（异步 C++ HTTP） | 补充接口 |
| 数据库 | SQLite3（C API，WAL） | |
| 配置/日志 | yaml-cpp / spdlog | |
| 任务调度 | std::thread + 线程池（可选 folly/boost） | |

## 3. 多厂商 USB 摄像头识别（关键设计）

### 3.1 目标
- **不绑定任何厂商/型号**：只要是标准 **UVC**（USB Video Class）设备即可接入；
- **稳定身份绑定**：拔插、换 USB 口后设备身份不漂移。

### 3.2 识别流程（四步）

```
① libusb 枚举 USB 总线，筛选接口类 14 (Video) 的设备
② 读取描述符：VID / PID / iManufacturer / iProduct / iSerialNumber
③ 关联 /dev/videoN：经 /sys/class/video4linux/videoN/device 的 USB 拓扑路径对齐
④ V4L2 能力探测：QUERYCAP → ENUM_FMT → ENUM_FRAMESIZES/INTERVALS → 能力表
```

### 3.3 身份绑定优先级
1. **序列号**（`iSerialNumber`，同型号多台也能区分）→ 首选；
2. **USB 物理端口**（总线拓扑如 `3-2.1`，序列号缺失时用）；
3. VID:PID + 枚举序号；
4. 最后兜底：枚举顺序。

配套 **udev 规则**：按序列号/物理端口生成稳定节点 `/dev/cam01...`（或直接使用 sysfs 路径），避免 `/dev/videoN` 漂移。

### 3.4 数据结构（示意）

```cpp
struct VideoFormat {
    uint32_t pixelformat;        // V4L2_PIX_FMT_H264/MJPEG/YUY2/NV12...
    uint32_t width, height, fps;
};

struct UvcCameraInfo {
    uint16_t vid, pid;
    std::string manufacturer, product, serial;
    std::string usb_path;                // 物理端口拓扑，如 "3-2.1"
    std::string v4l2_device;             // /dev/videoN 或 /dev/cam01
    std::vector<VideoFormat> formats;    // 能力表
};
```

### 3.5 多厂商兼容策略
- **通用 UVC 协议处理，不做 VID/PID 白名单**；OV9734、Logitech、海康 USB、任意 UVC 模组都走同一条链路；
- 能力差异自动降级：`H.264 直出 → MJPEG（nvjpeg GPU 解码）→ YUY2`；
- 厂商怪癖兜底：枚举慢/需复位 → libusb 超时重试 + udev 触发；
- 配置按 `serial / usb_path` 匹配相机身份（而不是 `/dev/videoN` 顺序），保证换口不串号。

## 4. ONVIF 服务设计（优先协议）

### 4.1 角色定位
- 本机对上位机呈现为"**虚拟多路摄像机/NVR**"：
  - **每路 USB 摄像头 = 一个 ONVIF MediaProfile**；
  - 上位机流程：WS-Discovery 发现 → `GetProfiles` → `GetStreamUri` 拿 RTSP → 拉流/取快照。
- **ONVIF 负责**：设备发现、设备信息、实时流、快照、用户认证；
- **RESTful 负责**：ONVIF Profile S 不覆盖的本地能力——录像起停/查询/下载、存储状态、系统监控、批量配置。

### 4.2 开源基础与修改点（onvif_srvd / gSOAP）

- **基础**：fork [onvif_srvd](https://github.com/KoynovStas/onvif_srvd)（gSOAP 生成 SOAP/WS-Security 框架，C++ Linux 守护进程），按需升级 gSOAP 版本；可参考 thunderbird-onvif（V4L2 直连 ONVIF）做对接思路。
- **需要实现的 ONVIF 服务**：

| ONVIF 服务 | 关键接口 | 实现要点 |
|-----------|----------|----------|
| Device | `GetDeviceInformation` / `GetSystemDateAndTime` / `GetUsers` / `SetUser` | 对接本机信息与用户表（与 REST 共用） |
| Media | `GetProfiles` / `GetStreamUri` / `GetSnapshotUri` / `GetVideoSources` | 每路摄像头一个 Profile，RTSP URL 指向 mediamtx 对应流 |
| Discovery | WS-Discovery `Probe` / `Hello` / `Resolve` | 组播 `239.255.255.250:3702` |
| 可选 | Events（移动侦测告警）、Imaging（亮度/对比度） | 后续扩展；无云台可跳过 PTZ |

- **认证**：WS-Security `UsernameToken` Digest；与 REST 共用用户表（API Key / 用户名密码）。

### 4.3 ONVIF 与 RESTful 能力映射

| 能力 | ONVIF（优先） | RESTful（补充） |
|------|---------------|-----------------|
| 设备发现 | WS-Discovery | `GET /api/v1/cameras` |
| 实时流 | `Media.GetStreamUri` → RTSP | — |
| 快照 | `Media.GetSnapshotUri` → JPEG | `GET /api/v1/cameras/{id}/photo/latest` |
| 设备信息 | `Device.GetDeviceInformation` | `GET /health`、`/system/stats` |
| 录像控制/查询/下载 | Profile S 不含（可走扩展） | `/record/start|stop`、`/recordings`、`/download` |
| 存储状态 | 无 | `GET /api/v1/system/storage` |

## 5. 进程/线程模型与性能

| 项 | 设计 |
|----|------|
| 进程 | 1 个 `camera_server` 主进程；RTSP 用独立 mediamtx 进程 |
| 线程 | 每路采集线程 + 编码（NVIDIA 多通道回调）+ ONVIF/REST/索引各线程 |
| 内存 | 每路 2~3 帧环形缓冲（NV12），12 路 <1GB |
| CPU | 全硬件媒体链路，12 路 720p30 <20% |
| 故障隔离 | 采集线程崩溃不影响其他路；设备掉线自动重连（udev 事件） |

## 6. 构建与部署

```
camera_cpp/
├── CMakeLists.txt            # + vcpkg: libusb yaml-cpp spdlog sqlite3 drogon
├── src/
│   ├── device_discovery.{h,cpp}   # 多厂商 UVC 识别（libusb+sysfs+V4L2）
│   ├── pipeline.{h,cpp}           # V4L2→nvjpeg/nvvidconv→nvv4l2h264enc
│   ├── recorder.{h,cpp}           # 分段 MP4 + SQLite
│   ├── onvif/                     # onvif_srvd fork 集成点
│   ├── rest/                      # Drogon 路由
│   └── main.cpp
├── vendor/onvif_srvd/        # git submodule
└── deploy/camera_server.service
```

- Jetson：JetPack 自带 nvv4l2h264enc/nvjpeg；`ffmpeg -encoders | grep h264_nvv4l2` 验证；
- 部署：systemd 单服务 + mediamtx 服务；
- 上位机联调：ONVIF Device Manager / VLC（RTSP）验证 Profile S 互通。

## 7. 实施里程碑

| 阶段 | 内容 | 工期 | 状态 |
|------|------|------|------|
| M1 | C++ 工程骨架 + 多厂商设备发现（含能力探测/udev 绑定） | 2 周 | ✅ camera_cpp 已实现 |
| M2 | 媒体管道（V4L2→nvjpeg/nvv4l2h264enc）+ 分段录像 + SQLite + 配额 | 2~3 周 | ✅ 本机实测通过 |
| M3 | ONVIF（Device/Media/WS-Discovery）+ RTSP + 快照 | 2~3 周 | ✅ 本机实测通过 |
| M4 | RESTful（自研零依赖 HTTP/JSON）+ 鉴权 | 1~2 周 | ✅ 本机实测通过 |
| M5 | Jetson Xavier NX 部署 + 真机集成测试 | — | ⏳ 待 SSH 连通后执行（deploy/jetson/deploy.sh） |

---
*文档版本：v1.0（2026-08）*
