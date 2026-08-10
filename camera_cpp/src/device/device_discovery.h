// 多厂商 UVC 摄像头发现（libusb/sysfs + V4L2 能力探测）
// 设计文档：docs/05-C++高性能实现与ONVIF设计.md §3
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace camera {

struct VideoFormat {
    uint32_t fourcc = 0;   // 如 V4L2_PIX_FMT_MJPEG / H264 / YUY2
    int width = 0, height = 0, fps = 0;
};

struct UvcCameraInfo {
    uint16_t vid = 0, pid = 0;
    std::string manufacturer;   // iManufacturer
    std::string product;        // iProduct
    std::string serial;         // iSerialNumber（同型号多台区分关键）
    std::string usb_path;       // USB 物理端口拓扑，如 "3-2.1"
    std::string v4l2_device;    // /dev/videoN（或 udev 固定 /dev/cam01）
    bool is_uvc = false;        // 接口类是否 14 (Video)
    std::vector<VideoFormat> formats;  // V4L2 能力表

    // 稳定设备 UUID：由 USB 物理端口 + VID:PID + 序列号派生，跨重启/换 /dev/videoN 不变
    std::string uuid;
};

// 扫描本机所有 UVC 摄像头（Linux sysfs；其他平台返回空）
// probe=true 时额外做 V4L2 格式探测（会打开 /dev/videoN）；
// 启动绑定仅需身份（UUID）时可 probe=false，避免与后续采集打开冲突
std::vector<UvcCameraInfo> discover_uvc_cameras(bool probe = true);

// 打印信息（调试用）
std::string describe(const UvcCameraInfo& c);

}  // namespace camera
