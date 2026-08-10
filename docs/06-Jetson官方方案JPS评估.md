# Jetson 官方方案（Jetson Platform Services / AI-NVR）需求满足度评估
> [!NOTE]
> **设计/调研文档（历史）**：本文档为早期设计与选型参考，**不代表当前实现**。
> 当前已实现并部署的系统见 [README.md](../README.md) 与 [07-当前实现与运维手册.md](07-当前实现与运维手册.md)。


> 评估对象：NVIDIA Jetson Platform Services（JPS，前身 Metropolis Microservices for Jetson）及其参考应用 AI-NVR（VST + DeepStream + SDR + eMDAT）
> 参考文档：[Quick Start](https://docs.nvidia.com/jetson/jps/setup/quick-start.html) / [JPS Overview](https://docs.nvidia.com/moj/moj-overview.html) / [Release Notes 2.0](https://docs.nvidia.com/moj/moj-Releasenotes.html) / [VST Overview](https://docs.nvidia.com/moj/vst/VST_Overview.html) / [VST API](https://docs.nvidia.com/moj/vst/VST_Application_development_guide.html)
> 评估日期：2026-08-06

## 1. 一句话结论

**直接采用 Jetson 官方 JPS/AI-NVR 不能满足本项目的核心需求**，存在三条硬性不匹配：

1. **硬件**：JPS 2.0 仅支持 Jetson Orin 系列（JetPack 6.1/6.2），本项目的 Demo 平台 Xavier NX（最高 JetPack 5.1.x）无法运行；
2. **输入源**：VST 只接入 IP 摄像头（ONVIF-S / 自定义 RTSP URL）和双路 CSI，**不支持 USB UVC 摄像头**，本项目的 OV9734 USB 方案需要自行开发 USB→RTSP 桥接；
3. **协议**：VST 是 ONVIF **客户端**（发现/管理 IP 相机），不提供 ONVIF **服务端**，本项目"设备对外呈现为虚拟多路 ONVIF 相机/NVR"的需求（WS-Discovery / GetProfiles / GetStreamUri）无官方覆盖。

但 VST 作为"NVR 录像/存储/回放/流媒体"后端是成熟可复用的：**若平台切到 Orin 且愿意接受 IP 相机（或自建 USB→RTSP 桥）**，可用 VST 替换自研方案中录像分段、存储配额、回放、WebRTC 等约 2~3 周工作量，并顺带获得 DeepStream/AI 分析能力。

## 2. JPS/AI-NVR 是什么

| 层 | 组成 | 说明 |
|----|------|------|
| Foundation Services | VST、Redis（消息总线）、Ingress（API 网关）、Storage、Networking、Monitoring（Prometheus/Grafana）、Firewall | systemd 管理的容器化基础服务，随 JetPack 安装到 `/opt/nvidia/jetson/services` |
| AI Services | DeepStream Perception（目标检测/跟踪）、Analytics（越线/ROI/人数/热力图）、SDR（流自动路由）、VLM/零样本检测（AGX） | 通过 REST API 调用 |
| 参考工作流 | AI-NVR、VIA（视频摘要） | docker compose 部署的应用包 |

## 3. 需求逐项对照

| 需求 | 本方案设计 | JPS/AI-NVR 是否满足 | 说明 |
|------|-----------|--------------------|------|
| R1 多路同时录像 | 12×USB UVC（OV9734）并行采集→硬编→落盘 | ⚠️ 部分满足 | VST 支持多路录像（计划/事件/常录，H.264/H.265），但输入必须是 **IP 相机 RTSP**；USB 相机需自建桥 |
| R2 本地存储/配额/循环覆盖 | NVMe + 配额管理 + 循环覆盖 + 掉电恢复 | ✅ 满足 | VST Storage API：aging policy（自动删旧）、存储监控、媒体下载/上传 |
| R3 RESTful API | /health、/cameras、/photo、/record、/recordings、/storage | ⚠️ 部分满足 | VST 有 `sensor/list`、`record/start|stop`、`replay/*`、`storage/*` 等 REST API 并经 Ingress 网关暴露；但 API 形状与本方案设计不同，需自研适配层 |
| R4 拍照 API | 编码器取帧→JPEG 落盘→URL 访问 | ⚠️ 弱满足 | VST 支持对**已录制流**抓快照（replay snapshot）、NVStreamer 支持文件快照；对**实时相机"即拍"**无一等公民 API，需自研 |
| R5 录像控制/查询/下载 | 起停、按相机/时间查询、下载 | ✅ 满足 | record start/stop、timeline 查询、storage download 齐全 |
| R6 可运维 | 状态查询、健康检查、异常自恢复 | ✅ 满足 | systemd 服务、Monitoring/告警、防火墙、Ingress 安全（基础就绪） |
| ONVIF（优先协议） | 设备对外呈现虚拟多路 ONVIF 相机：WS-Discovery、GetProfiles、GetStreamUri | ❌ 不满足 | VST 仅做 ONVIF **客户端**（发现 IP 相机）；无 ONVIF 服务端能力。RTSP 代理 URL 可对外供 RTSP 客户端拉流，但不是 ONVIF 协议 |
| USB UVC 摄像头接入 | V4L2 + uvcvideo，热插拔/udev 固定设备名 | ❌ 不满足 | VST 不支持 USB/V4L2 输入（仅 IP RTSP + 双路 CSI）。12 路 USB 全部需要自研采集 + USB→RTSP 桥 |
| 平台：Xavier NX（Demo） | JetPack 5.1.3 | ❌ 不满足 | JPS 2.0 仅支持 Orin（JetPack 6.1/6.2），Xavier NX 最高 JetPack 5.1.x，官方不支持 JPS/VST |
| 平台：量产（Orin 系列） | Jetson Orin NX/Nano | ✅ 满足 | 若量产选 Orin，JPS 可直接运行（JetPack 6.1/6.2） |
| 7×24 可靠性 | 看门狗、单路隔离、掉电恢复 | ✅ 满足（平台侧） | 容器化 + systemd 托管 + 监控告警，成熟度高于自研 |
| AI 能力（未来） | 可选 NPU 移动侦测等 | ✅✅ 超出预期 | DeepStream 检测/跟踪、越线/ROI/人数分析、零样本检测、VLM 摘要——若后续要 AI，JPS 是最大加分项 |

## 4. 三个关键硬伤（详细）

### 4.1 硬件平台不兼容
- JPS 2.0 官方要求：Jetson Orin AGX / Orin NX 16/8GB / Orin Nano 8GB，JetPack 6.1 GA（BSP R36.4.0）或 JP 6.2（R36.4.3）。
- Xavier NX 属 legacy 产品线，最高 JetPack 5.1.x（Ubuntu 20.04），**无法安装 JPS/VST**。
- 本项目 04 号文档的 4 路 Demo 平台是 Xavier NX → 官方方案无法在其上验证。

### 4.2 输入源不支持 USB UVC 相机
- VST 的传感器接入方式：ONVIF-S 自动发现（IP 相机）、按 IP 地址/RTSP URL 手动添加、双路 CSI 相机。
- **没有 USB/V4L2 采集适配器**（VST 的 Adaptor 架构提供 Device Discovery / Device Control 两个扩展接口，但需自研实现，且仍要走 RTSP 中转）。
- 现实路径：每路 OV9734 USB 相机 → 自研 GStreamer/mediamtx 推 RTSP → VST 按 RTSP URL 添加。**采集、编码、热插拔、设备绑定这一大块还是要自研**，JPS 省不掉。

### 4.3 不提供 ONVIF 服务端
- 本项目 05 号文档的核心是"设备对外 = 虚拟多路 ONVIF 相机/NVR"（WS-Discovery、GetProfiles、GetStreamUri、快照）。
- JPS 栈中 VST 只做 ONVIF 客户端；对外呈现能力是 RTSP 代理 + WebRTC，**无 ONVIF Server**。
- 若要保留 ONVIF 对外协议，仍需自研 onvif_srvd/gSOAP 方案（即 05 号文档已规划的路线）。

## 5. 建议（三条路径）

### 路径 A：维持现状自研（推荐，除非明确要上 AI）
- 理由：M1~M4 已完成（camera_cpp），M5 仅剩 Xavier NX 真机部署；核心需求（USB 多路 + REST/ONVIF + 本地录像）JPS 均无直接覆盖，引入 JPS 反而引入一套重型容器栈和平台迁移成本。
- 成本：继续 1 周完成 M5。

### 路径 B：Orin + JPS 后端 + 自研 USB 桥（若平台切 Orin 且要 AI）
- 架构：12×USB 相机 → 自研 V4L2 采集 + mediamtx 推 RTSP → VST 录像/存储/回放/WebRTC → 自研适配层（对外 REST/ONVIF Server）。
- 收益：VST 替换录像分段、配额循环覆盖、回放、监控等后端（省 2~3 周）；获得 DeepStream/AI 分析能力。
- 代价：平台成本上升（Orin NX/Nano 模组价高）、仍需自研采集桥 + ONVIF Server + 拍照 API 适配、12 路 USB 在 Orin 上的带宽/供电问题依旧存在。

### 路径 C：混合渐进（低风险）
- 第一阶段维持自研录像（Xavier NX 16G 即可跑）；
- 仅当 AI 需求落地时，在 Orin 上单独引入 DeepStream/JPS AI 服务，与自研录像解耦集成（通过 RTSP 拉流喂给 DeepStream）。
- JPS 的 VST 可留作后续 NVR 后端替换项，不阻塞当前进度。

## 6. 参考资料

- [Jetson Platform Services Quick Start](https://docs.nvidia.com/jetson/jps/setup/quick-start.html)
- [Jetson Platform Services Overview](https://docs.nvidia.com/moj/moj-overview.html)
- [Release Notes (v2.0)](https://docs.nvidia.com/moj/moj-Releasenotes.html)
- [VST Overview](https://docs.nvidia.com/moj/vst/VST_Overview.html)
- [VST Components & Customization (API)](https://docs.nvidia.com/moj/vst/VST_Application_development_guide.html)
- [VST Record Stream Management API](https://docs.nvidia.com/vss/3.2.0/vst-record-stream-management-api.html)
- [VST Storage Management API](https://docs.nvidia.com/vss/3.2.0/vst-storage-management-api.html)
- [VST Replay Stream Management API](https://docs.nvidia.com/vss/3.2.0/vst-replay-stream-management-api.html)

---
*文档版本：v1.0（2026-08-06）*

---

## 7. 补充评估：不要 AI 能力，仅用 Xavier NX 16G 是否满足

> 追问背景：既然 JPS 是 Orin 专属，那么**放弃 AI（也就不用 JPS）**，仅用现有/手头的 Xavier NX 16G 单板跑完整方案（12×OV9734 720p30 录像 + REST/ONVIF + 7×24），是否够用？

### 7.1 结论

**够用，且余量充足**。核心计算/存储/接口资源全部满足；真正的瓶颈不是算力而是**两件事**：① 12 路 MJPEG 解码必须走 GPU（nvjpeg），不能纯软件；② 12 个廉价 UVC 模组挂在 USB Hub 上的供电与枚举稳定性（工程问题，非算力问题）。

### 7.2 资源逐项核算（12×720p30，无 AI）

| 资源 | 需求 | Xavier NX 16G 能力 | 余量 |
|------|------|--------------------|------|
| H.264 硬件编码 | 12×720p30 ≈ 5.3×1080p30 当量 | **20×1080p30**（H.264） | ≈27%，余量 ~3.7× |
| MJPEG 解码 | 12×720p30（OV9734 仅 MJPEG/YUY2，无 H.264 直出） | GPU nvjpeg（CUDA）可全卸；纯软件需 3~4/6 核 | ⚠️ **必须走 GPU 路径** |
| CPU（非媒体） | 分段 mux/文件写/API/ONVIF/RTSP | 6 核 Carmel（余 ~4-5 核） | ✅ |
| 内存 | <1GB（12×720p 缓冲+应用） | 16GB LPDDR4x | ✅ 富余 |
| USB 带宽 | 12×~2-4MB/s ≈ 24-48MB/s（192-384Mbps） | USB3.1 Gen1（5Gbps，聚合上限 ~4Gbps） | ✅ 带宽充足 |
| 存储写入 | 12×2-3Mbps ≈ 3-4.5MB/s | NVMe M.2（≥500MB/s） | ✅ |
| 存储容量 | 12×720p@3Mbps ≈ 260-390GB/天 | 2TB≈5-7天 / 4TB≈9-14天 | 与 12 路方案设计同量级 |
| 功耗/散热 | 满载持续 | 20W 模式，需主动散热 | ⚠️ 必须装风扇 |

> 编码规格来源：Xavier NX 官方规格 H.264 编码 **2×4K60 | 4×4K30 | 10×1080p60 | 20×1080p30**（比 04 文档里"与 H.265 相近"的说法更精确，H.264 是 20×1080p30，H.265 是 22×）。

### 7.3 三个必须注意的工程点

1. **MJPEG 解码是唯一算力瓶颈**
   - OV9734 只出 MJPEG/YUY2（无 H.264 直出），必须解码后再硬编 H.264。
   - 12×720p30 纯软件解码（libjpeg-turbo）约吃掉 3~4 个核，会与录像/API 争 CPU，7×24 下掉帧风险高。
   - **方案：走 GPU nvjpeg 解码**（04 文档 M2 已规划），或降为 15fps 录像作为兜底。CPU 解码只适合 ≤4 路验证。

2. **12 路 UVC 的 USB 工程稳定性（多路 UVC 通用风险，非 Xavier 特有）**
   - 必须主动供电 Hub（12×约 110-250mA，合计 1.3-3A，加 Hub 自身功耗），普通 Hub 必掉线/掉帧；
   - 建议按 USB 控制器分 2 个 Hub（若载板有多个 USB3 口），降低单 Hub 枚举压力；
   - 热插拔/掉线自恢复逻辑（camera_cpp M1 已有）必须保留并真机压测。

3. **生命周期与平台定位**
   - Xavier NX 属 legacy 产品线，**模块供应到约 2028-01，最高 JetPack 5.1.3**（Ubuntu 20.04），后续无官方大版本更新 → 适合 Demo/小批量，不适合面向多年量产的新产品；
   - 若未来又想加 AI：Xavier NX **16G 版 = 21 TOPS**，可跑轻量目标检测（如 PeopleNet 低分辨率），同硬件即可升级，无需换板——这是选 16G 而非 8G 的最大理由；
   - 平台定位：Xavier NX 16G 作为"演示 + AI 预留"平台；量产升级 Orin NX/Nano（同为 NVIDIA 生态，迁移成本低）。

### 7.4 结论表

| 判断维度 | 结论 |
|----------|------|
| 12×720p30 录像+本地存储+API+ONVIF | ✅ 满足（余量充足） |
| 唯一算力瓶颈 | MJPEG 解码 → 必须 GPU nvjpeg |
| 主要工程风险 | 12 路 UVC 供电/枚举（非算力） |
| 生命周期 | ⚠️ EOL 临近，适合 Demo/小批量 |
| 未来加 AI | ✅ 16G=21 TOPS，同硬件可升级 |
| 量产路径 | Xavier NX 仅适合 Demo/小批量；量产升级 Orin 系列 |

**最终建议**：不要 AI 时，Xavier NX 16G **单独一台即可满足 12 路需求**，无需 JPS、无需 Orin；只需按 7.3 的三点做好工程化（GPU 解码、供电 Hub、散热）。把它定位为"**演示 + AI 预留**"平台，量产届时升级 Orin 系列。
