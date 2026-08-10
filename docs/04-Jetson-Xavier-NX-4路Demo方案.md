# Jetson Xavier NX 4 路 Demo 方案
> [!NOTE]
> **设计/调研文档（历史）**：本文档为早期设计与选型参考，**不代表当前实现**。
> 当前已实现并部署的系统见 [README.md](../README.md) 与 [07-当前实现与运维手册.md](07-当前实现与运维手册.md)。


> 本方案描述：用 **NVIDIA Jetson Xavier NX** 做 **4 路 USB 摄像头录像 + 拍照 API** 的 Demo 版本（对应完整方案的功能子集）。

## 1. 适用场景与选型理由

| 场景 | 说明 |
|------|------|
| 已有 Xavier NX 硬件 | 手头有设备，快速验证「多路 USB 录像 + REST API」可行性 |
| 需要 AI 能力 | 后续要加目标检测/人脸识别等，Jetson 的 CUDA/DeepStream 生态成熟 |
| 平台验证 | 用 Demo 验证方案后再决定量产平台（Orin 系列） |

> ⚠️ **重要提醒**：Xavier NX 已属 NVIDIA **legacy 产品线**（模块供应到 2028-01），最高支持 **JetPack 5.1.3（Ubuntu 20.04）**，**JetPack 6 仅支持 Orin 系列**。做 Demo 没问题；若做新产品量产，建议直接评估 **Jetson Orin NX / Nano**。

## 2. Xavier NX 规格速览

| 项目 | 规格 |
|------|------|
| CPU | 6 核 Carmel（ARMv8.2） |
| GPU | 384 CUDA 核 + 48 Tensor 核（Volta），支持 CUDA |
| 内存 | 8GB / 16GB LPDDR4x |
| H.265 硬件编码 | 2×4K60 / 4×4K30 / 10×1080p60 / **22×1080p30** |
| H.264 硬件编码 | 能力与 H.265 相近（4 路 1080p30 仅占不到 1/5） |
| 硬件解码 | 最高 2×8K30 / 多路 4K60（远超 4 路需求） |
| 图像加速 | nvjpeg（GPU JPEG 编解码）、nvvidconv |
| USB | USB 3.1（载板通常 4×USB-A + USB-C） |
| 存储 | M.2 NVMe（开发套件支持） |
| 功耗模式 | 10W / 15W / 20W |
| SDK | JetPack 5.1.3（Ubuntu 20.04）为最高版本 |

**结论：Xavier NX 做 4 路 720p/1080p30 录像，编码能力余量 >5 倍，完全够用。**

## 3. 4 路 Demo 硬件配置（BOM）

| 项目 | 型号 | 数量 | 参考价 |
|------|------|------|--------|
| 主控 | Jetson Xavier NX 模组（8/16GB）+ 载板（或开发套件） | 1 | ¥1500~3500 |
| 摄像头 | Logitech C920（H.264 直出，验证最简单管道）或 OV9734 720p 模组 | 4 | ¥300/个（C920）或 ¥100/个（OV9734） |
| USB Hub | USB3.0 主动供电 4 口 | 1 | ¥100 |
| 录像盘 | NVMe SSD 500GB~1TB | 1 | ¥250~400 |
| 电源 | 19V/65W（开发套件规格） | 1 | 随套件 |
| 散热 | 主动风扇套件 | 1 | ¥50~100 |
| **合计** | | | **约 ¥3500~6000** |

- 4 路 1080p30 MJPEG 带宽约 120~320 Mbps，单 USB3.0 控制器 + 1 个主动供电 Hub 足够；
- 系统跑在 NVMe 或 eMMC（载板），录像数据独立放 NVMe。

## 4. 软件栈

| 层 | 组件 | 说明 |
|----|------|------|
| OS | JetPack 5.1.3（Ubuntu 20.04 arm64） | Xavier NX 最高版本；勿升 JetPack 6 |
| 采集 | V4L2（uvcvideo），GStreamer `v4l2src` | USB UVC 免驱 |
| JPEG 解码 | `jpegdec`（软件，4 路可行）/ nvjpeg（GPU，自定义代码） | 4 路 1080p 软件解码 CPU 约占 60~70%，720p 更省 |
| 格式转换 | `nvvidconv`（GPU/NVMM） | |
| H.264 编码 | `nvv4l2h264enc`（硬件） | NVIDIA 硬件编码器 |
| 录像封装 | `mp4mux` / ffmpeg 封装 | 分段 MP4 |
| API | FastAPI + uvicorn + SQLite | 与完整方案完全一致（复用 02 文档） |
| RTSP（可选） | mediamtx / DeepStream | |

> 架构与 [02-软件架构与API设计.md](02-软件架构与API设计.md) 相同，媒体链路走 NVIDIA nvv4l2h264enc / nvjpeg，API 层、数据库、存储管理全部复用。

## 5. GStreamer 媒体管道示例

### 5.1 管道 A：相机 H.264 直出（C920，最简，零转码）

```bash
# 直接封装，不重新编码，CPU 占用几乎为 0
gst-launch-1.0 v4l2src device=/dev/video0 \
  ! video/x-h264,width=1920,height=1080,framerate=30/1 \
  ! h264parse ! mp4mux ! filesink location=/data/recordings/CAM01/140000.mp4
```

### 5.2 管道 B：OV9734 MJPEG 输入 → 硬件编码 H.264（推荐 Demo）

```bash
# v4l2src → 软件 JPEG 解码 → nvvidconv 转 NVMM → 硬件 H.264 编码 → MP4
gst-launch-1.0 v4l2src device=/dev/video0 \
  ! image/jpeg,width=1280,height=720,framerate=30/1 \
  ! jpegdec ! nvvidconv ! video/x-raw(memory:NVMM) \
  ! nvv4l2h264enc bitrate=2000000 \
  ! h264parse ! mp4mux ! filesink location=/data/recordings/CAM01/140000.mp4
```

- 每路一个 GStreamer 进程（4 路 = 4 进程，与 02 文档的进程模型一致）；
- 多路抓拍：从编码前帧用 `nvjpeg` 编码 JPEG（GPU 加速），或 `jpegenc`（软件，4 路可接受）。

## 6. Xavier NX vs Orin（4 路视角）

| 维度 | Jetson Xavier NX 16G | Jetson Orin NX 16G |
|------|----------------------|--------------------|
| 硬件编码能力（H.264） | 20×1080p30 | 1×8K30 / 2×4K60 等（更强） |
| 4 路余量 | >5 倍 | 更大 |
| 生态 | CUDA / DeepStream / TensorRT（AI 强） | 同左，另支持 JetPack 6 / JPS |
| SDK 现状 | **legacy**，最高 JetPack 5.1.3 | 主流支持（JetPack 6.1/6.2） |
| 价格 | 模组 ¥1500+ | 模组 ¥3000+ |
| 功耗 | 10~20W | 10~25W |
| 量产风险 | EOL 临近（模块供应到约 2028-01），不推荐长期量产 | 无 EOL 风险 |
| 适用 | Demo / 已有设备 / AI 预留 | **量产升级路径** |

**建议**：Demo 用 Xavier NX 完全可行；若 Demo 验证后要量产，升级 Orin 系列——媒体管道同为 NVIDIA 插件（nvv4l2/nvjpeg/nvvidconv），API/数据库/存储层可原样复用，成本主要在模组与载板。

## 7. 4 路 Demo 实施步骤

| 步骤 | 内容 | 工期 |
|------|------|------|
| 1 | 刷 JetPack 5.1.3，验证 4 路 USB 摄像头枚举 | 0.5 天 |
| 2 | 跑通管道 A/B，确认 4 路同时编码不掉帧 | 1 天 |
| 3 | 部署 02 文档的 API 服务（复用），对接 4 路进程 | 2~3 天 |
| 4 | 存储管理（配额循环覆盖）+ 掉电恢复 | 1~2 天 |
| 5 | 7×24 稳定性验证 + 拍照 API 联调 | 2 天 |
| **合计** | | **约 1 周** |

## 8. 注意事项

- **不要升 JetPack 6**：Xavier NX 不支持，只能装 JetPack 5.1.x（Ubuntu 20.04）；
- **JPEG 解码 CPU**：4 路 1080p MJPEG 软件解码约占 4 个核；若 CPU 紧张，降到 720p 或用 nvjpeg；
- **供电**：开发套件 19V/65W，4 摄像头 + NVMe 满载请用原装电源；
- **散热**：20W 模式需主动散热，否则降频掉帧；
- **编码器参数**：`nvv4l2h264enc` 的 GOP/码率按 02 文档建议配置（bitrate 2~6 Mbps、关键帧间隔 60）。

---
*文档版本：v1.0（2026-08）*
