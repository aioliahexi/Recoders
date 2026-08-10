// 媒体管道接口：采集 → 编码 → H.264 帧回调 / 抓拍
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace camera {

// 实时帧率计：统计最近 ~0.5s 窗口的编码帧率（区分"配置帧率"与"实际帧率"）
class FpsMeter {
public:
    FpsMeter() { last_ = std::chrono::steady_clock::now(); }
    void tick() {
        count_++;
        auto now = std::chrono::steady_clock::now();
        // 每 500ms 采样一次，保留最近 8 个样本 = 4s 滑动窗口（读数平滑，抗 USB 突发抖动）
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_).count() >= 500) {
            samples_.emplace_back(now, count_);
            if (samples_.size() > 8) samples_.pop_front();
            if (samples_.size() >= 2) {
                const auto& a = samples_.front();
                const auto& b = samples_.back();
                const double dt = std::chrono::duration<double>(b.first - a.first).count();
                if (dt > 0.5)
                    fps_.store(static_cast<double>(b.second - a.second) / dt, std::memory_order_relaxed);
            }
            last_ = now;
        }
    }
    double fps() const {
        // 超过 1.5s 无新帧（如拔线离线）视为 0
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last_).count() > 1.5)
            return 0.0;
        return fps_.load(std::memory_order_relaxed);
    }

private:
    uint64_t count_ = 0;
    std::chrono::steady_clock::time_point last_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, uint64_t>> samples_;
    std::atomic<double> fps_{0.0};
};

struct EncodeFrame {
    uint64_t pts_ms = 0;          // 统一毫秒时间戳
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool key = false;             // 关键帧
};

// 编码器参数（供录像 muxer 建立输出流用）
struct EncoderParams {
    int codec_id = 0;             // AV_CODEC_ID_H264
    int width = 0, height = 0;
    int fps = 30;
    int bitrate_kbps = 2000;
    std::vector<uint8_t> extradata;   // SPS/PPS
    uint32_t tb_num = 1000, tb_den = 1;  // 输出时间基 1/1000
};

struct PipelineParams {
    int camera_id = 0;
    std::string name;
    std::string device;           // /dev/video0；mock 时忽略
    bool mock = false;            // 用 lavfi testsrc2 模拟（无硬件调试）
    std::string input_format;     // mjpeg / h264 / yuy2
    int width = 1280, height = 720, fps = 30;
    int bitrate_kbps = 2000;
    int gop = 60;
    std::string encoder;          // auto / h264_rkmpp / h264_nvv4l2 / nvgst / libx264
    std::string capture = "auto"; // auto / ffmpeg / gst（GStreamer 全链路采集解码，Jetson 1080p 提速）
    bool osd = true;              // 叠加时间戳 + 相机名称（gst 路径）
};

class Pipeline {
public:
    virtual ~Pipeline() = default;
    virtual bool open(const PipelineParams& p) = 0;
    virtual bool start() = 0;     // 启动采集线程
    virtual void stop() = 0;

    // 注册/移除编码帧监听（录像、RTSP 等可同时订阅）
    virtual size_t add_frame_listener(std::function<void(const EncodeFrame&)> cb) = 0;
    virtual void remove_frame_listener(size_t token) = 0;

    // SPS/PPS 就绪回调（GStreamer nvv4l2 编码器需首关键帧后才可提取）
    virtual void set_extradata_callback(std::function<void(const std::vector<uint8_t>&)> cb) = 0;

    // 抓拍一帧 JPEG 到 photo_dir/<ts>.jpg，并更新 latest.jpg；
    // out_name 返回生成的文件名（供 REST 写照片事件标签索引）
    virtual bool snapshot(const std::string& photo_dir, int quality,
                          std::string* out_name = nullptr) = 0;

    virtual const EncoderParams& encoder_params() const = 0;
    virtual bool running() const = 0;
    virtual std::string status() const = 0;

    // 实测帧率（REST/Web 显示用；0=未运行或尚无数据）
    virtual double measured_fps() const { return 0.0; }

    // 最近一帧的墙钟毫秒（在线判定：距当前 <3s 视为在线）
    virtual uint64_t last_frame_ms() const { return 0; }

    // 最新预览帧 JPEG（实时流用；max_w>0 时降采样到该宽度）
    // 返回 true 且 out 非空表示有可用帧；无新帧可返回 false（流端应跳过本次刷新）
    virtual bool preview_jpeg(std::vector<uint8_t>& out, int max_w = 640) { return false; }
};

std::unique_ptr<Pipeline> create_pipeline();

}  // namespace camera
