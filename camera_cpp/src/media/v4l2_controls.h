// V4L2 摄像头控件（亮度/对比度/饱和度/清晰度等），Linux 专用
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace camera {

struct V4l2Control {
    uint32_t id = 0;
    std::string name;
    int64_t min = 0, max = 0, step = 1, def = 0, value = 0;
    bool menu = false;
};

// 列出设备的可用控件及当前值（非 Linux 或设备不存在时返回空）
std::vector<V4l2Control> v4l2_list_controls(const std::string& device);

// 设置控件值（返回是否成功）
bool v4l2_set_control(const std::string& device, uint32_t id, int64_t value);

}  // namespace camera
