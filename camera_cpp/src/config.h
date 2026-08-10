// 统一 JSON 配置（camera_server 单一配置文件，无第三方依赖）
//
// 一个 config.json 同时管理：
//   - 系统配置：data_dir / quota_mb / segment_time_s / encoder / http_port /
//               rtsp_port / api_key / host_ip / autostart / onvif_*
//   - 用户配置：nvr_name / rest_public / cameras[]（含 UUID 绑定、相机参数）
//
// 兼容迁移（首次运行自动完成）：
//   - 旧 config.ini  -> 同目录 config.json
//   - 旧 data_dir/recorder.json（及更早的 camera_names/registry/nvr_settings.json）
//     -> 合并进 config.json 并删除
#pragma once

#include <map>
#include <string>
#include <vector>

namespace camera {

// 相机配置（静态默认 + 运行时注册统一结构）
struct CameraCfg {
    int id = 0;                // 持久化 id（0 = 由系统自动分配）
    std::string uuid;          // 物理设备 UUID（绑定设备；空=未识别）
    std::string name = "CAM01";
    std::string device = "/dev/video0";
    bool mock = false;
    std::string input_format = "mjpeg";  // mjpeg / h264 / yuy2
    int width = 1920, height = 1080, fps = 30;
    int bitrate_kbps = 3000;
    int gop = 60;
    bool enabled = true;       // 是否启用该路（Web 配置页可开关）
    bool osd = true;           // 画面叠加时间戳 + 相机名称（GStreamer 路径）
    std::string capture = "gst";  // auto / ffmpeg / gst
};

struct AppCfg {
    std::string config_path;   // 统一 JSON 配置文件路径（运行时读写，空=未持久化）
    std::string data_dir = "/tmp/cam-demo";
    uint64_t quota_mb = 200;
    int segment_time_s = 60;   // 录像分段秒数（默认 1 分钟一段，按时间命名）
    std::string encoder = "auto";  // auto / h264_rkmpp / libx264
    int http_port = 8000;          // RESTful + ONVIF SOAP
    int rtsp_port = 8554;          // RTSP 实时流
    std::string api_key;           // REST 鉴权（空=关闭）
    std::string host_ip;           // 自动探测，可手动指定
    bool autostart = true;         // 启动即开始录像
    bool onvif_enabled = true;     // ONVIF 服务开关
    bool onvif_discovery = true;   // WS-Discovery 开关
    int run_seconds = 0;           // 0 = 运行到 Ctrl+C
    bool snapshot = false;         // 停止前对每路抓拍一张 JPEG
    bool list_devices = false;
    // ---- 用户配置（同一 config.json） ----
    std::string nvr_name = "camera_server NVR";
    bool rest_public = false;      // 免鉴权 RESTful（调试用）
    std::vector<CameraCfg> cameras;
};

// 用法: camera_server [--config file(.json|.ini)] [--mock] [--duration N] [--list]
// 返回的 cfg.config_path 为统一 JSON 文件（.ini 传入时自动迁移到同名 .json）
AppCfg load_config(int argc, char** argv);

// 完整写回统一 JSON（系统 + 用户），原子写（先写临时文件再 rename）
void save_json_config(const std::string& config_path, const AppCfg& cfg);

// 用户配置（nvr_name / rest_public / cameras）读写自统一 JSON 文件；
// save 为读-改-写，保留 system 字段
struct RecorderJson {
    std::string nvr_name = "camera_server NVR";
    bool rest_public = false;  // 免鉴权 RESTful（调试用）
    std::vector<CameraCfg> cameras;
    bool needs_save = false;   // 从旧 JSON 迁移而来，需要写回
};
RecorderJson load_recorder_json(const std::string& config_path);
void save_recorder_json(const std::string& config_path, const RecorderJson& cfg);

// 迁移完成后删除旧的 data_dir 配置文件（recorder.json / camera_names.json /
// camera_registry.json / nvr_settings.json）
void remove_legacy_json(const std::string& data_dir);

}  // namespace camera
