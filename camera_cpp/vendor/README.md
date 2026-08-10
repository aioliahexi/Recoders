# 第三方组件集成说明

| 组件 | 用途 | 集成方式 |
|------|------|----------|
| [onvif_srvd](https://github.com/KoynovStas/onvif_srvd)（gSOAP） | ONVIF 服务端骨架 | `git submodule add` 到 `vendor/onvif_srvd`，fork 修改 Device/Media/Discovery 服务实现；gSOAP 版本建议升级到最新稳定版 |
| [mediamtx](https://github.com/bluenviron/mediamtx)（MIT） | RTSP 流服务 | 独立进程/子模块；每路摄像头一条 RTSP 推流，ONVIF `GetStreamUri` 返回其 URL |
| Rockchip MPP | H.264/JPEG 硬件编解码 | RK 板卡 BSP 提供（`librockchip_mpp` / `mpp.h`） |
| Drogon | RESTful HTTP | vcpkg 安装；M4 阶段接入 |
| libusb | USB 描述符读取（增强） | 可选；当前设备发现用 sysfs 零依赖实现，需要更多控制能力时接入 |

## onvif_srvd 改造点（fork 后）

1. `DeviceService`：`GetDeviceInformation` 返回本机型号/固件；`GetUsers`/`SetUser` 对接 SQLite 用户表；
2. `MediaService`：`GetProfiles` 遍历相机注册表；`GetStreamUri` 返回 `rtsp://<ip>:8554/<camera_name>`；`GetSnapshotUri` 返回 HTTP 快照 URL；
3. `DiscoveryService`：启动时发 WS-Discovery `Hello`，响应 `Probe`/`Resolve`；
4. 认证：WS-Security UsernameToken Digest，与 REST 共用用户表。
