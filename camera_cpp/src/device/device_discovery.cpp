// 多厂商 UVC 摄像头发现实现
// 原理：/sys/class/video4linux/videoN/device 符号链接 -> USB 接口目录，
//       向上回溯到含 idVendor 的 USB 设备目录，读取描述符文件，
//       再对 /dev/videoN 做 V4L2 能力探测（仅 Linux）。
#include "device_discovery.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace camera {

namespace {

constexpr const char* kVideo4Linux = "/sys/class/video4linux";
constexpr const char* kVideoDevPrefix = "/dev/";

std::string read_sysfs(const std::string& path) {
    std::ifstream in(path);
    std::string v;
    std::getline(in, v);
    return v;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 从 videoN/device 符号链接起点，向上回溯到 USB 设备目录（含 idVendor）
// 返回 USB 设备目录绝对路径；同时记录最近一层接口目录的 bInterfaceClass。
bool find_usb_device_dir(const std::string& start_abs, std::string* usb_dev_dir,
                         std::string* iface_class) {
    fs::path cur(start_abs);
    iface_class->clear();
    for (int depth = 0; depth < 12 && !cur.empty(); ++depth) {
        const fs::path idv = cur / "idVendor";
        if (fs::exists(idv)) {
            *usb_dev_dir = cur.string();
            return true;
        }
        const fs::path cls = cur / "bInterfaceClass";
        if (fs::exists(cls) && iface_class->empty()) {
            *iface_class = trim(read_sysfs(cls.string()));
        }
        cur = cur.parent_path();
    }
    return false;
}

// 解析 sysfs 中的 bInterfaceClass：0e = Video (UVC)
// FNV-1a 64 位哈希 -> 8 位十六进制段（用于 UUID 派生）
uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

// 稳定 UUID：
//  - 有序列号：以 序列号+vid:pid 为身份（换 USB 口不变，跨端口稳定）
//  - 无序列号：含 usb 物理端口（同型号多台只能靠端口区分；换口由热插拔重绑定处理）
// 格式 xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
std::string make_uuid(const UvcCameraInfo& c) {
    char buf[64];
    if (!c.serial.empty()) {
        std::snprintf(buf, sizeof(buf), "vid=%04x;pid=%04x;sn=%s",
                      c.vid, c.pid, c.serial.c_str());
    } else {
        std::snprintf(buf, sizeof(buf), "usb=%s;vid=%04x;pid=%04x;sn=",
                      c.usb_path.c_str(), c.vid, c.pid);
    }
    std::string id = buf;
    uint64_t a = fnv1a64(id);
    uint64_t b = fnv1a64(id + "#b");
    char out[40];
    std::snprintf(out, sizeof(out), "%08llx-%04llx-%04llx-%04llx-%012llx",
                  (unsigned long long)(a >> 32), (unsigned long long)((a >> 16) & 0xffff),
                  (unsigned long long)(a & 0xffff),
                  (unsigned long long)((b >> 48) & 0xffff),
                  (unsigned long long)(b & 0xffffffffffffULL));
    return out;
}

bool is_video_class(const std::string& cls_hex) {
    return cls_hex == "0e" || cls_hex == "0E";
}

#ifdef __linux__
void probe_v4l2_formats(const std::string& dev, std::vector<VideoFormat>* out) {
    int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return;
    struct v4l2_capability cap {};
    if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
        ::close(fd);
        return;
    }
    // 仅枚举 Video Capture 设备
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        ::close(fd);
        return;
    }
    struct v4l2_fmtdesc fmt {};
    fmt.index = 0;
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    while (::ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
        struct v4l2_frmsizeenum frmsize {};
        frmsize.index = 0;
        frmsize.pixel_format = fmt.pixelformat;
        while (::ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
            if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                struct v4l2_frmivalenum fiv {};
                fiv.index = 0;
                fiv.pixel_format = fmt.pixelformat;
                fiv.width = frmsize.discrete.width;
                fiv.height = frmsize.discrete.height;
                while (::ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &fiv) == 0) {
                    if (fiv.type == V4L2_FRMIVAL_TYPE_DISCRETE && fiv.discrete.denominator) {
                        out->push_back({fmt.pixelformat,
                                        static_cast<int>(frmsize.discrete.width),
                                        static_cast<int>(frmsize.discrete.height),
                                        static_cast<int>(fiv.discrete.denominator / fiv.discrete.numerator)});
                    }
                    fiv.index++;
                }
            }
            frmsize.index++;
        }
        fmt.index++;
    }
    ::close(fd);
}
#endif  // __linux__

}  // namespace

std::vector<UvcCameraInfo> discover_uvc_cameras(bool probe) {
    std::vector<UvcCameraInfo> result;
    if (!fs::is_directory(kVideo4Linux)) return result;

    for (const auto& entry : fs::directory_iterator(kVideo4Linux)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("video", 0) != 0) continue;  // video0, video1, ...

        // device 符号链接 -> 接口目录（用 canonical 正确解析相对链接）
        fs::path link = entry.path() / "device";
        std::error_code ec;
        fs::path start_abs;
        fs::path target = fs::read_symlink(link, ec);
        if (ec) continue;
        fs::path cand = fs::canonical(link, ec);
        if (!ec) {
            start_abs = cand;
        } else {
            // 回退：以符号链接所在目录为基准
            start_abs = (entry.path() / target).lexically_normal();
        }

        std::string usb_dev_dir, iface_class;
        if (!find_usb_device_dir(start_abs, &usb_dev_dir, &iface_class)) continue;

        UvcCameraInfo info;
        info.usb_path = fs::path(usb_dev_dir).filename().string();  // 如 "3-2.1"
        info.is_uvc = is_video_class(iface_class);
        if (!info.is_uvc) continue;  // 只要 UVC 视频设备

        auto read16 = [&](const char* f) -> uint16_t {
            std::string v = trim(read_sysfs(usb_dev_dir + "/" + f));
            return v.empty() ? 0 : static_cast<uint16_t>(std::stoul(v, nullptr, 16));
        };
        info.vid = read16("idVendor");
        info.pid = read16("idProduct");
        info.manufacturer = read_sysfs(usb_dev_dir + "/manufacturer");
        info.product = read_sysfs(usb_dev_dir + "/product");
        info.serial = read_sysfs(usb_dev_dir + "/serial");
        info.uuid = make_uuid(info);
        info.v4l2_device = kVideoDevPrefix + name;

#ifdef __linux__
        if (probe) probe_v4l2_formats(info.v4l2_device, &info.formats);
#endif
        result.push_back(std::move(info));
    }
    return result;
}

std::string describe(const UvcCameraInfo& c) {
    std::ostringstream os;
    os << "cam " << c.v4l2_device
       << " | VID:PID=" << std::hex << c.vid << ":" << c.pid << std::dec
       << " | " << c.manufacturer << " " << c.product
       << " | serial=" << (c.serial.empty() ? "(无)" : c.serial)
       << " | usb=" << c.usb_path
       << " | uuid=" << c.uuid
       << " | formats=" << c.formats.size();
    return os.str();
}

}  // namespace camera
