// GStreamer 全链路采集 Pipeline（Jetson 1080p 提速用）
// 链路: v4l2src -> jpegdec(多线程软解~28fps@1080p) -> nvvidconv -> NVMM -> nvv4l2h264enc -> appsink
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pipeline.h"

namespace camera {

#ifdef HAVE_GSTREAMER

std::unique_ptr<Pipeline> create_gst_capture();

#else
inline std::unique_ptr<Pipeline> create_gst_capture() { return nullptr; }
#endif

}  // namespace camera
