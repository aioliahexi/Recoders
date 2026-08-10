// 相机管理：启动/注册/删除 共用逻辑（main 启动、热插拔线程、REST 添加删除）
#pragma once

#include <string>

#include "config.h"
#include "services.h"

namespace camera {

struct CameraSpec {
    int id = 0;              // 0 = 自动分配
    std::string name;
    std::string device;
    bool mock = false;
    std::string input_format = "mjpeg";
    int width = 1920, height = 1080, fps = 30;
    int bitrate_kbps = 3000, gop = 60;
    std::string capture = "gst";
    bool osd = true;
};

// 启动/注册相机：绑定 UUID、应用自定义名、建管道/目录、入统一 config.json；
// autostart=true 立即开始录像
bool launch_camera(Context& ctx, Recorder* rec, const CameraSpec& spec,
                   bool autostart, std::string* err);

// 删除相机：停止管道、从统一 config.json 移除（数据保留），清理运行时表
bool remove_camera(Context& ctx, Recorder* rec, int id, std::string* err);

// 分配新相机 id
int next_camera_id(Context& ctx);

// 解析设备 UUID（/dev/videoN -> UUID；未发现返回空）
std::string device_uuid(const std::string& dev);

// 解析设备 vid/pid（/dev/videoN -> {vid,pid}；未发现返回 {0,0}）
std::pair<uint16_t, uint16_t> device_vidpid(const std::string& dev);

}  // namespace camera
