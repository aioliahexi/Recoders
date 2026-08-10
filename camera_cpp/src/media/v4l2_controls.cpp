#include "v4l2_controls.h"

#ifdef __linux__
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace camera {

std::vector<V4l2Control> v4l2_list_controls(const std::string& device) {
    std::vector<V4l2Control> out;
#ifdef __linux__
    int fd = ::open(device.c_str(), O_RDWR);
    if (fd < 0) return out;
    struct v4l2_queryctrl qc {};
    qc.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (::ioctl(fd, VIDIOC_QUERYCTRL, &qc) == 0) {
        if (qc.flags & V4L2_CTRL_FLAG_DISABLED) { qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL; continue; }
        V4l2Control c;
        c.id = qc.id;
        c.name = reinterpret_cast<const char*>(qc.name);
        c.min = qc.minimum;
        c.max = qc.maximum;
        c.step = qc.step;
        c.def = qc.default_value;
        c.menu = (qc.type == V4L2_CTRL_TYPE_MENU);
        struct v4l2_control ctl {};
        ctl.id = qc.id;
        if (::ioctl(fd, VIDIOC_G_CTRL, &ctl) == 0) c.value = ctl.value;
        out.push_back(c);
        qc.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    ::close(fd);
#endif
    return out;
}

bool v4l2_set_control(const std::string& device, uint32_t id, int64_t value) {
#ifdef __linux__
    int fd = ::open(device.c_str(), O_RDWR);
    if (fd < 0) return false;
    struct v4l2_control ctl {};
    ctl.id = id;
    ctl.value = static_cast<int32_t>(value);
    bool ok = ::ioctl(fd, VIDIOC_S_CTRL, &ctl) == 0;
    ::close(fd);
    return ok;
#else
    (void)device; (void)id; (void)value;
    return false;
#endif
}

}  // namespace camera
