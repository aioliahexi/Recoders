// GStreamer nvv4l2 硬件编码后端（Jetson）
// 链路: appsrc(I420) -> nvvidconv -> NVMM -> nvv4l2h264enc -> h264parse -> appsink
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "pipeline.h"

namespace camera {

#ifdef HAVE_GSTREAMER

class GstH264Encoder {
public:
    GstH264Encoder();
    ~GstH264Encoder();

    // 初始化编码器（返回 false 表示不可用）
    bool init(int width, int height, int fps, int bitrate_kbps, int gop);
    bool ok() const { return pipeline_ != nullptr; }

    // 推入一帧 yuv420p；随后 pull 收集 H.264 包
    bool push(const uint8_t* yuv, size_t size, uint64_t pts_ms);
    bool pull(std::vector<uint8_t>* out, bool* key, uint64_t* pts_ms);

    // 解除阻塞（置 NULL），供 stop() 先于线程 join 调用，避免 push 卡死
    void abort();
    // 停止：发送 EOS 并排空（线程已退出后调用）
    void flush();

    const EncoderParams& params() const { return params_; }

private:
    void* pipeline_ = nullptr;   // GstElement*
    void* appsrc_ = nullptr;     // GstElement*
    void* appsink_ = nullptr;    // GstElement*
    void* bus_ = nullptr;        // GstBus*
    EncoderParams params_;
    bool key_seen_ = false;
    uint64_t first_key_ms_ = 0;
};

#endif  // HAVE_GSTREAMER

#ifndef HAVE_GSTREAMER
// 非 GStreamer 平台：stub，保证接口一致且可编译
class GstH264Encoder {
public:
    bool init(int, int, int, int, int) { return false; }
    bool ok() const { return false; }
    bool push(const uint8_t*, size_t, uint64_t) { return false; }
    bool pull(std::vector<uint8_t>*, bool*, uint64_t*) { return false; }
    void abort() {}
    void flush() {}
    const EncoderParams& params() const { return params_; }
private:
    EncoderParams params_;
};
#endif

}  // namespace camera
