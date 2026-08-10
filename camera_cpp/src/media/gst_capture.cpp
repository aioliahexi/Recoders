// GStreamer 全链路采集 Pipeline 实现（v4l2src -> jpegdec -> nvvidconv -> NVENC -> appsink）
#ifdef HAVE_GSTREAMER

#include "gst_capture.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include "../util/log.h"

namespace camera {

namespace {

struct Nal { const uint8_t* p; size_t n; uint8_t type; };

std::vector<Nal> scan_nals(const uint8_t* data, size_t size) {
    std::vector<Nal> out;
    auto find_code = [&](size_t from, size_t* cl, size_t* nal) -> bool {
        size_t p = from;
        while (p + 3 <= size) {
            if (data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) { *cl = 3; *nal = p + 3; return true; }
            if (p + 4 <= size && data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 0 && data[p + 3] == 1) { *cl = 4; *nal = p + 4; return true; }
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

}  // namespace

class GstCapturePipeline : public Pipeline {
public:
    ~GstCapturePipeline() override { stop(); }

    bool open(const PipelineParams& p) override {
        p_ = p;
        encoder_name_ = "gst-cap(jpegdec+NVENC)";  // 显示用（GStreamer 多线程软解 + NVENC 硬编）
        params_.codec_id = 27;
        params_.width = p.width;
        params_.height = p.height;
        params_.fps = p.fps;
        params_.bitrate_kbps = p.bitrate_kbps;
        return true;  // 实际管道在 start() 线程中构建（支持重连）
    }

    bool start() override {
        if (thread_.joinable()) return false;
        running_.store(true);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() override {
        running_.store(false);
        generation_.fetch_add(1);  // 使旧线程退出（即使已分离也能感知）
        // 有界等待线程退出（run() 内 pull 超时 50ms、teardown 限时 2s，正常 <3s 退出）
        for (int i = 0; i < 100 && thread_alive_.load(); i++) ::usleep(100 * 1000);
        bool alive = thread_alive_.load();
        if (thread_.joinable()) thread_.detach();
        thread_ = std::thread();  // 复位句柄：start() 可再次启动
        if (!alive) teardown();   // 线程已退出：安全清理（幂等）；挂起则不触碰 gst
    }

    size_t add_frame_listener(std::function<void(const EncodeFrame&)> cb) override {
        std::lock_guard<std::mutex> lk(lm_);
        listeners_.push_back(std::move(cb));
        return listeners_.size();
    }

    void remove_frame_listener(size_t token) override {
        std::lock_guard<std::mutex> lk(lm_);
        if (token > 0 && token <= listeners_.size()) listeners_[token - 1] = nullptr;
    }

    void set_extradata_callback(std::function<void(const std::vector<uint8_t>&)> cb) override {
        extradata_cb_ = std::move(cb);
    }

    bool snapshot(const std::string& photo_dir, int quality,
                  std::string* out_name) override {
        // 一次性 FFmpeg 抓帧解码（独立于采集线程）
        return ffmpeg_one_shot_snapshot(photo_dir, quality, out_name);
    }

    const EncoderParams& encoder_params() const override { return params_; }
    bool running() const override { return running_.load(); }
    std::string status() const override { return encoder_name_; }
    double measured_fps() const override { return fps_meter_.fps(); }
    uint64_t last_frame_ms() const override { return last_frame_ms_.load(); }

    bool preview_jpeg(std::vector<uint8_t>& out, int max_w) override {
        (void)max_w;  // 直通相机原生 MJPG，避免额外解码/转码开销
        std::lock_guard<std::mutex> lk(snap_mu_);
        if (snap_jpeg_.empty()) return false;
        out = snap_jpeg_;
        return true;
    }

private:
    void emit(const EncodeFrame& f) {
        fps_meter_.tick();
        last_frame_ms_.store(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        std::vector<std::function<void(const EncodeFrame&)>> cbs;
        { std::lock_guard<std::mutex> lk(lm_); for (auto& c : listeners_) if (c) cbs.push_back(c); }
        for (auto& c : cbs) c(f);
    }

    bool build_pipeline() {
        if (gst_pipe_) return true;
        static bool inited = false;
        if (!inited) { gst_init(nullptr, nullptr); inited = true; }
        // 主链：v4l2src -> tee；分支1 = 编码录像（jpegdec->nvvidconv->NVENC）；分支2 = 相机 MJPG 直通预览（jsink，leaky 不阻塞主链）
        // 相机名（textoverlay 转义）
        std::string cam_name = p_.name.empty() ? ("CAM" + std::to_string(p_.camera_id)) : p_.name;
        std::string esc_name;
        for (char ch : cam_name) {
            if (ch == '\\' || ch == '"') esc_name += '\\';
            esc_name += ch;
        }
        std::string caps_jpeg = "image/jpeg,width=" + std::to_string(p_.width) +
                                ",height=" + std::to_string(p_.height) +
                                ",framerate=" + std::to_string(p_.fps) + "/1";
        std::string caps_nvmm = "video/x-raw(memory:NVMM),width=" + std::to_string(p_.width) +
                                ",height=" + std::to_string(p_.height) +
                                ",framerate=" + std::to_string(p_.fps) + "/1";
        std::string nvenc = "nvv4l2h264enc bitrate=" + std::to_string(p_.bitrate_kbps) +
                            " idrinterval=" + std::to_string(p_.gop);
        std::string desc;
        if (p_.osd) {
            // 主链：jpegdec -> 时间戳 + 相机名叠加 -> NVENC；预览分支：叠加后降采样 jpegenc
            desc = "v4l2src device=" + p_.device +
                   " ! queue max-size-buffers=4 ! " + caps_jpeg + " ! "
                   "jpegdec ! "
                   "clockoverlay time-format=\"%Y-%m-%d %H:%M:%S\" font-desc=\"Sans 20\" halignment=left valignment=top ! "
                   "textoverlay text=\"" + esc_name + "\" font-desc=\"Sans 20\" halignment=left valignment=bottom ! "
                   "tee name=t1 "
                   "t1. ! queue max-size-buffers=4 ! "
                   "nvvidconv ! " + caps_nvmm + " ! "
                   + nvenc + " ! "
                   "appsink name=sink max-buffers=32 drop=false "
                   "t1. ! queue max-size-buffers=2 leaky=downstream ! "
                   "nvvidconv ! video/x-raw,format=I420,width=640,height=360 ! jpegenc quality=75 ! "
                   "appsink name=jsink max-buffers=1 drop=true";
        } else {
            // 无 OSD：相机 MJPG 直通预览（零转码）
            desc = "v4l2src device=" + p_.device +
                   " ! queue max-size-buffers=4 ! " + caps_jpeg + " ! "
                   "tee name=t "
                   "t. ! queue max-size-buffers=4 ! jpegdec ! queue max-size-buffers=4 ! "
                   "nvvidconv ! " + caps_nvmm + " ! "
                   "queue max-size-buffers=4 ! "
                   + nvenc + " ! "
                   "appsink name=sink max-buffers=32 drop=false "
                   "t. ! queue max-size-buffers=2 leaky=downstream ! "
                   "appsink name=jsink max-buffers=1 drop=true";
        }
        GError* err = nullptr;
        GstElement* pipe = gst_parse_launch(desc.c_str(), &err);
        if (!pipe) {
            LOG_ERROR("[gstcap:%d] 管道创建失败: %s", p_.camera_id, err ? err->message : "?");
            if (err) g_error_free(err);
            return false;
        }
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
        if (!sink) { gst_object_unref(pipe); return false; }
        GstElement* js = gst_bin_get_by_name(GST_BIN(pipe), "jsink");
        if (!js) { gst_object_unref(sink); gst_object_unref(pipe); return false; }
        gst_sink_ = sink;
        gst_jsink_ = js;
        gst_bus_ = gst_element_get_bus(pipe);
        gst_pipe_ = pipe;
        GstStateChangeReturn sr = gst_element_set_state(pipe, GST_STATE_PLAYING);
        if (sr == GST_STATE_CHANGE_FAILURE) { teardown(); return false; }
        LOG_INFO("[gstcap:%d] %s 管道已启动", p_.camera_id, p_.device.c_str());
        return true;
    }

    void teardown() {
        // 必须先同步置 NULL 再释放，否则 GStreamer 报 "dispose while PLAYING" 并可能 SEGV
        if (gst_pipe_) {
            GstElement* p = static_cast<GstElement*>(gst_pipe_);
            gst_element_set_state(p, GST_STATE_NULL);
            gst_element_get_state(p, nullptr, nullptr, 2 * GST_SECOND);  // 限时等待（编码器卡死时 2 秒后放弃）
            gst_object_unref(p);
        }
        if (gst_sink_) gst_object_unref(gst_sink_);
        if (gst_jsink_) gst_object_unref(gst_jsink_);
        if (gst_bus_) gst_object_unref(gst_bus_);
        gst_sink_ = nullptr; gst_jsink_ = nullptr; gst_bus_ = nullptr; gst_pipe_ = nullptr;
        extradata_sent_ = false;
        params_.extradata.clear();
    }

    void run() {
        thread_alive_.store(true);
        const int gen = generation_.load();
        while (running_.load() && gen == generation_.load()) {
            if (!build_pipeline()) {
                if (running_.load() && gen == generation_.load())
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            bool broken = false;
            while (running_.load() && gen == generation_.load() && !broken) {
                // 检查 bus 错误
                if (gst_bus_) {
                    GstMessage* msg = gst_bus_pop_filtered(static_cast<GstBus*>(gst_bus_),
                        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
                    if (msg) {
                        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                            GError* e2 = nullptr; gchar* dbg = nullptr;
                            gst_message_parse_error(msg, &e2, &dbg);
                            LOG_ERROR("[gstcap:%d] 管道错误: %s (%s)", p_.camera_id,
                                      e2 ? e2->message : "?", dbg ? dbg : "");
                            if (e2) g_error_free(e2); if (dbg) g_free(dbg);
                            broken = true;
                        }
                        gst_message_unref(msg);
                        if (broken) break;
                    }
                }
                GstSample* sample = gst_app_sink_try_pull_sample(
                    GST_APP_SINK(gst_sink_), 50 * GST_MSECOND);
                if (!sample) {
                    if (!running_.load()) break;
                    continue;  // 超时无样本，继续轮询
                }
                GstBuffer* buf = gst_sample_get_buffer(sample);
                GstMapInfo map;
                if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
                    bool key = false;
                    for (auto& n : scan_nals(map.data, map.size))
                        if (n.type == 5 || n.type == 7 || n.type == 8) { key = true; break; }
                    uint64_t pts = GST_BUFFER_PTS_IS_VALID(buf)
                        ? static_cast<uint64_t>(GST_BUFFER_PTS(buf) / GST_MSECOND) : 0;
                    // 首关键帧提取 SPS/PPS
                    if (key && !extradata_sent_) {
                        std::vector<uint8_t> sps, pps;
                        for (auto& n : scan_nals(map.data, map.size)) {
                            if (n.type == 7 && sps.empty()) sps = annexb(n.p, n.n);
                            else if (n.type == 8 && pps.empty()) pps = annexb(n.p, n.n);
                        }
                        if (!sps.empty() && !pps.empty()) {
                            params_.extradata = sps;
                            params_.extradata.insert(params_.extradata.end(), pps.begin(), pps.end());
                            extradata_sent_ = true;
                            if (extradata_cb_) extradata_cb_(params_.extradata);
                        }
                    }
                    if (key) {
                        std::lock_guard<std::mutex> lk(snap_mu_);
                        snap_h264_.assign(map.data, map.data + map.size);
                    }
                    EncodeFrame ef;
                    ef.pts_ms = pts;
                    ef.data = map.data;
                    ef.size = map.size;
                    ef.key = key;
                    emit(ef);
                    gst_buffer_unmap(buf, &map);
                }
                gst_sample_unref(sample);
                // 预览分支：非阻塞取最新相机 MJPG 帧缓存（jsink 满则丢，不阻塞主链）
                if (gst_jsink_) {
                    GstSample* js = gst_app_sink_try_pull_sample(GST_APP_SINK(gst_jsink_), 0);
                    if (js) {
                        GstBuffer* jb = gst_sample_get_buffer(js);
                        GstMapInfo jm;
                        if (gst_buffer_map(jb, &jm, GST_MAP_READ)) {
                            {
                                std::lock_guard<std::mutex> lk(snap_mu_);
                                snap_jpeg_.assign(jm.data, jm.data + jm.size);
                            }
                            gst_buffer_unmap(jb, &jm);
                        }
                        gst_sample_unref(js);
                    }
                }
            }
            teardown();
            if (running_.load() && gen == generation_.load()) {
                LOG_INFO("[gstcap:%d] 采集中断，3 秒后重连", p_.camera_id);
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
        thread_alive_.store(false);
    }

    bool ffmpeg_one_shot_snapshot(const std::string& photo_dir, int quality,
                                   std::string* out_name) {
        std::error_code ec;
        std::filesystem::create_directories(photo_dir, ec);
        std::string ts = std::to_string(static_cast<long long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        std::string out = photo_dir + "/" + ts + ".jpg";
        std::string latest = photo_dir + "/latest.jpg";
        std::vector<uint8_t> h264;
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            h264 = snap_h264_;
        }
        if (!h264.empty()) {
            // 方式1：解码已缓存的关键帧（不占设备）；需前置 SPS/PPS 才能独立解码
            const std::string tmp = "/tmp/gcap_snap.h264";
            {
                std::ofstream of(tmp, std::ios::binary);
                if (!params_.extradata.empty())
                    of.write(reinterpret_cast<const char*>(params_.extradata.data()),
                             static_cast<std::streamsize>(params_.extradata.size()));
                of.write(reinterpret_cast<const char*>(h264.data()), static_cast<std::streamsize>(h264.size()));
            }
            std::string cmd = "ffmpeg -y -v error -f h264 -i " + tmp +
                              " -frames:v 1 -q:v 2 " + out + " 2>/dev/null";
            if (std::system(cmd.c_str()) == 0 && std::filesystem::exists(out)) {
                std::error_code ec2;
                std::filesystem::copy_file(out, latest, std::filesystem::copy_options::overwrite_existing, ec2);
                if (out_name) *out_name = ts + ".jpg";
                return !ec2;
            }
        }
        // 方式2：兜底直接抓设备（可能在别的进程占用时失败）
        std::string cmd = "ffmpeg -y -v error -f v4l2 -input_format mjpeg -video_size " +
                          std::to_string(p_.width) + "x" + std::to_string(p_.height) +
                          " -framerate " + std::to_string(p_.fps) + " -i " + p_.device +
                          " -frames:v 1 -q:v 2 " + out + " 2>/dev/null";
        if (std::system(cmd.c_str()) != 0 || !std::filesystem::exists(out)) return false;
        std::error_code ec2;
        std::filesystem::copy_file(out, latest, std::filesystem::copy_options::overwrite_existing, ec2);
        if (out_name) *out_name = ts + ".jpg";
        return !ec2;
    }

    PipelineParams p_;
    EncoderParams params_;
    std::string encoder_name_;
    std::atomic<bool> running_{false};
    std::atomic<bool> thread_alive_{false};
    std::atomic<int> generation_{0};
    std::thread thread_;
    FpsMeter fps_meter_;
    std::atomic<uint64_t> last_frame_ms_{0};
    void* gst_pipe_ = nullptr;   // GstElement*
    void* gst_sink_ = nullptr;   // GstElement*（H.264 输出）
    void* gst_jsink_ = nullptr;  // GstElement*（MJPG 预览直通）
    void* gst_bus_ = nullptr;    // GstBus*
    bool extradata_sent_ = false;
    std::mutex lm_;
    std::vector<std::function<void(const EncodeFrame&)>> listeners_;
    std::function<void(const std::vector<uint8_t>&)> extradata_cb_;
    std::mutex snap_mu_;
    std::vector<uint8_t> snap_h264_;   // 最近关键帧 H.264（抓拍用，避免再开设备）
    std::vector<uint8_t> snap_jpeg_;   // 最近相机 MJPG 帧（实时预览流用，直通零转码）
};

std::unique_ptr<Pipeline> create_gst_capture() {
    return std::make_unique<GstCapturePipeline>();
}

}  // namespace camera

#endif  // HAVE_GSTREAMER
