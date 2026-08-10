// GStreamer nvv4l2 H.264 硬件编码后端实现（Jetson，需 gstreamer dev + nvv4l2h264enc）
#ifdef HAVE_GSTREAMER

#include "gst_encoder.h"

#include <cstring>
#include <string>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "../util/log.h"

namespace camera {

namespace {

struct NalRange { const uint8_t* p; size_t n; uint8_t type; };

// 从 annex-b 流解析 NAL（找起始码）
std::vector<NalRange> scan_nals(const uint8_t* data, size_t size) {
    std::vector<NalRange> out;
    auto find_code = [&](size_t from, size_t* code_len, size_t* nal) -> bool {
        size_t p = from;
        while (p + 3 <= size) {
            if (data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) { *code_len = 3; *nal = p + 3; return true; }
            if (p + 4 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1) { *code_len = 4; *nal = p + 4; return true; }
            p++;
        }
        return false;
    };
    size_t cl = 0, nal = 0;
    if (!find_code(0, &cl, &nal)) return out;
    while (nal <= size) {
        size_t ncl = 0, nnal = size;
        bool found = find_code(nal, &ncl, &nnal);
        size_t end = found ? nnal - ncl : size;
        if (end > nal) out.push_back({data + nal, end - nal, static_cast<uint8_t>(data[nal] & 0x1f)});
        if (!found) break;
        nal = nnal;
    }
    return out;
}

std::vector<uint8_t> annexb(const uint8_t* nal, size_t n) {
    std::vector<uint8_t> v = {0, 0, 0, 1};
    v.insert(v.end(), nal, nal + n);
    return v;
}

// 去掉 SPS(7)/PPS(8) NAL（avcC extradata 已携带，避免与 mov muxer 冲突）
std::vector<uint8_t> strip_sps_pps(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    for (auto& n : scan_nals(data, size)) {
        if (n.type == 7 || n.type == 8) continue;
        auto v = annexb(n.p, n.n);
        out.insert(out.end(), v.begin(), v.end());
    }
    return out;
}

}  // namespace

GstH264Encoder::GstH264Encoder() = default;
GstH264Encoder::~GstH264Encoder() { flush(); }

bool GstH264Encoder::init(int width, int height, int fps, int bitrate_kbps, int gop) {
    if (pipeline_) return true;
    static bool gst_inited = false;
    if (!gst_inited) { gst_init(nullptr, nullptr); gst_inited = true; }

    std::string desc = "appsrc name=src format=time ! "
                       "queue max-size-buffers=4 ! "  // NVIDIA 插件链路要求显式 queue
                       "video/x-raw,format=I420,width=" + std::to_string(width) +
                       ",height=" + std::to_string(height) +
                       ",framerate=" + std::to_string(fps) + "/1 ! "
                       "nvvidconv ! video/x-raw(memory:NVMM),width=" +
                       std::to_string(width) + ",height=" + std::to_string(height) +
                       ",framerate=" + std::to_string(fps) + "/1 ! "
                       "queue max-size-buffers=4 ! "
                       "nvv4l2h264enc bitrate=" + std::to_string(bitrate_kbps) +
                       " idr-interval=" + std::to_string(gop) + " ! "
                       "appsink name=sink max-buffers=32 drop=false";

    GError* err = nullptr;
    GstElement* pipe = gst_parse_launch(desc.c_str(), &err);
    if (!pipe) {
        if (err) { LOG_ERROR("[gst] 管道创建失败: %s", err->message); g_error_free(err); }
        return false;
    }
    GstElement* src = gst_bin_get_by_name(GST_BIN(pipe), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
    bus_ = gst_element_get_bus(pipe);
    if (!src || !sink) {
        LOG_ERROR("[gst] appsrc/appsink 获取失败");
        if (src) gst_object_unref(src);
        if (sink) gst_object_unref(sink);
        gst_object_unref(pipe);
        return false;
    }
    // appsrc caps
    GstCaps* caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420",
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height,
                                        "framerate", GST_TYPE_FRACTION, fps, 1, nullptr);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);
    g_object_set(G_OBJECT(src), "format", GST_FORMAT_TIME, nullptr);

    // 启动管道（否则 appsrc push 会在 preroll 阶段阻塞）
    GstStateChangeReturn sr = gst_element_set_state(pipe, GST_STATE_PLAYING);
    if (sr == GST_STATE_CHANGE_FAILURE) {
        LOG_ERROR("[gst] 管道进入 PLAYING 失败");
        gst_object_unref(sink);
        gst_object_unref(src);
        gst_object_unref(pipe);
        return false;
    }
    pipeline_ = pipe;
    appsrc_ = src;
    appsink_ = sink;
    params_.codec_id = 27;  // AV_CODEC_ID_H264
    params_.width = width;
    params_.height = height;
    params_.fps = fps;
    params_.bitrate_kbps = bitrate_kbps;
    LOG_INFO("[gst] nvv4l2h264enc 就绪 %dx%d@%d %dkbps", width, height, fps, bitrate_kbps);
    return true;
}

bool GstH264Encoder::push(const uint8_t* yuv, size_t size, uint64_t pts_ms) {
    if (!pipeline_) return false;
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, size, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buf);
        return false;
    }
    std::memcpy(map.data, yuv, size);
    gst_buffer_unmap(buf, &map);
    GST_BUFFER_PTS(buf) = pts_ms * GST_MSECOND;
    GST_BUFFER_DTS(buf) = pts_ms * GST_MSECOND;
    GST_BUFFER_DURATION(buf) = GST_SECOND / params_.fps;
    GstFlowReturn fr = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buf);
    if (fr != GST_FLOW_OK) {
        LOG_ERROR("[gst] appsrc push 失败 flow=%d", (int)fr);
    }
    return fr == GST_FLOW_OK;
}

bool GstH264Encoder::pull(std::vector<uint8_t>* out, bool* key, uint64_t* pts_ms) {
    if (!pipeline_) return false;
    // 轮询 bus：捕获管道错误/警告（静默吞掉会导致"无输出"假象）
    if (bus_) {
        GstMessage* msg = gst_bus_pop_filtered(static_cast<GstBus*>(bus_),
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
        if (msg) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* err = nullptr; gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                LOG_ERROR("[gst] 管道错误: %s (%s)", err ? err->message : "?", dbg ? dbg : "");
                if (err) g_error_free(err); if (dbg) g_free(dbg);
            } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                GError* err = nullptr; gchar* dbg = nullptr;
                gst_message_parse_warning(msg, &err, &dbg);
                LOG_ERROR("[gst] 管道警告: %s", err ? err->message : "?");
                if (err) g_error_free(err); if (dbg) g_free(dbg);
            }
            gst_message_unref(msg);
        }
    }
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);
    if (!sample) return false;
    GstBuffer* buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return false;
    }
    out->assign(map.data, map.data + map.size);
    if (pts_ms) *pts_ms = GST_BUFFER_PTS_IS_VALID(buf) ? static_cast<uint64_t>(GST_BUFFER_PTS(buf) / GST_MSECOND) : 0;
    // 关键帧判定：含 IDR(5) 或 SPS(7)/PPS(8)
    bool is_key = false;
    for (auto& n : scan_nals(map.data, map.size)) {
        if (n.type == 5 || n.type == 7 || n.type == 8) { is_key = true; break; }
    }
    if (key) *key = is_key;
    // 首次关键帧提取 SPS/PPS 作为 extradata（供 mp4/RTSP）
    if (is_key && !key_seen_) {
        key_seen_ = true;
        first_key_ms_ = *pts_ms;
        std::vector<uint8_t> sps, pps;
        for (auto& n : scan_nals(map.data, map.size)) {
            if (n.type == 7 && sps.empty()) sps = annexb(n.p, n.n);
            else if (n.type == 8 && pps.empty()) pps = annexb(n.p, n.n);
        }
        if (!sps.empty() && !pps.empty()) {
            params_.extradata = sps;
            params_.extradata.insert(params_.extradata.end(), pps.begin(), pps.end());
            LOG_INFO("[gst] 已提取 SPS/PPS extradata=%zu", params_.extradata.size());
        }
        // 关键帧包去掉 SPS/PPS（avcC 已有，避免 mov muxer 报 Invalid argument）
        auto stripped = strip_sps_pps(map.data, map.size);
        if (!stripped.empty()) {
            out->assign(stripped.begin(), stripped.end());
            if (pts_ms) *pts_ms = GST_BUFFER_PTS_IS_VALID(buf)
                ? static_cast<uint64_t>(GST_BUFFER_PTS(buf) / GST_MSECOND) : 0;
            gst_buffer_unmap(buf, &map);
            gst_sample_unref(sample);
            return true;
        }
    }
    gst_buffer_unmap(buf, &map);
    gst_sample_unref(sample);
    return true;
}

void GstH264Encoder::abort() {
    // 置 NULL 会让阻塞中的 appsrc push 以 GST_FLOW_FLUSHING 返回，解除卡死
    if (pipeline_)
        gst_element_set_state(static_cast<GstElement*>(pipeline_), GST_STATE_NULL);
}

void GstH264Encoder::flush() {
    if (!pipeline_) return;
    // 排空 appsink 剩余数据
    while (true) {
        GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink_), 0);
        if (!s) break;
        gst_sample_unref(s);
    }
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
    GstElement* pp = static_cast<GstElement*>(pipeline_);
    gst_element_set_state(pp, GST_STATE_NULL);
    gst_element_get_state(pp, nullptr, nullptr, 5 * GST_SECOND);  // 限时等待
    gst_object_unref(appsrc_);
    gst_object_unref(appsink_);
    if (bus_) gst_object_unref(bus_);
    gst_object_unref(pipeline_);
    pipeline_ = nullptr;
    appsrc_ = nullptr;
    appsink_ = nullptr;
    bus_ = nullptr;
}

}  // namespace camera

#endif  // HAVE_GSTREAMER
