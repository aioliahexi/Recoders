// FFmpeg 媒体管道：V4L2/lavfi 采集 → 解码 → 缩放 → H.264 编码 → 帧回调
// 编码器：RK 上可用 h264_rkmpp（硬件），其他平台回退 libx264
// 说明：输入格式统一走"解码-再编码"，保证抓拍与编码流一致；
//       H.264 直出相机的"copy 直封"优化留待 M3 实现。
#include "pipeline.h"
#include "gst_encoder.h"

#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <thread>
#include <unistd.h>

#include "../util/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}

namespace {
struct AvDeviceRegister {
    AvDeviceRegister() { avdevice_register_all(); }
} g_avdevice_register;
}

namespace camera {

namespace {

std::string ts_name() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

std::string av_err(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

int interrupt_cb(void* opaque) {
    auto* self = static_cast<std::atomic<bool>*>(opaque);
    return self->load() ? 0 : 1;
}

}  // namespace

class FfmpegPipeline : public Pipeline {
public:
    ~FfmpegPipeline() override {
        stop();
        if (enc_ctx_) avcodec_free_context(&enc_ctx_);
        if (gst_enc_) gst_enc_->flush();
    }

    bool open(const PipelineParams& p) override {
        // 支持热重配：清理上一轮编码器状态
        if (gst_mode_ && gst_enc_) { gst_enc_->flush(); gst_enc_.reset(); }
        if (enc_ctx_) { avcodec_free_context(&enc_ctx_); enc_ctx_ = nullptr; }
        enc_ = nullptr;
        gst_mode_ = false;
        extradata_sent_ = false;
        enc_params_ = EncoderParams{};
        p_ = p;
        // 编码器选择：显式 nvgst 优先；auto 在 Jetson 上自动试 GStreamer 硬编
        bool want_gst = false;
        const char* enc_name = nullptr;
        if (p_.encoder == "nvgst") want_gst = true;
        else if (p_.encoder == "h264_rkmpp") enc_name = "h264_rkmpp";
        else if (p_.encoder == "h264_nvv4l2") enc_name = "h264_nvv4l2";
        else if (p_.encoder == "libx264") enc_name = "libx264";
        else {  // auto：RK=rkmpp > Jetson FFmpeg nvv4l2 > GStreamer nvv4l2 > libx264
            if (avcodec_find_encoder_by_name("h264_rkmpp")) enc_name = "h264_rkmpp";
            else if (avcodec_find_encoder_by_name("h264_nvv4l2")) enc_name = "h264_nvv4l2";
#ifdef HAVE_GSTREAMER
            else want_gst = true;
#else
            else enc_name = "libx264";
#endif
        }

        if (want_gst) {
#ifdef HAVE_GSTREAMER
            gst_enc_ = std::make_unique<GstH264Encoder>();
            if (gst_enc_->init(p_.width, p_.height, p_.fps, p_.bitrate_kbps, p_.gop)) {
                gst_mode_ = true;
                encoder_name_ = "nvv4l2(gst)";
                enc_params_ = gst_enc_->params();  // 宽高/码率立即就绪，SPS/PPS 首关键帧后补充
                return true;
            }
            gst_enc_.reset();
            fprintf(stderr, "[pipeline] GStreamer nvv4l2 不可用（回退 libx264）\n");
            if (p_.encoder == "nvgst") return false;
            enc_name = "libx264";
#else
            if (p_.encoder == "nvgst") {
                fprintf(stderr, "[pipeline] 未编译 GStreamer 支持 (HAVE_GSTREAMER)\n");
                return false;
            }
            enc_name = "libx264";
#endif
        }

        enc_ = avcodec_find_encoder_by_name(enc_name);
        if (!enc_) {
            fprintf(stderr, "[pipeline] 找不到编码器 %s\n", enc_name);
            return false;
        }
        encoder_name_ = enc_name;

        // 创建并打开编码器（录像服务需要其参数建流，必须在此就绪）
        enc_ctx_ = avcodec_alloc_context3(enc_);
        enc_ctx_->width = p_.width;
        enc_ctx_->height = p_.height;
        enc_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
        enc_ctx_->time_base = {1, p_.fps};
        enc_ctx_->framerate = {p_.fps, 1};
        enc_ctx_->bit_rate = p_.bitrate_kbps * 1000;
        enc_ctx_->gop_size = p_.gop;
        enc_ctx_->max_b_frames = 0;  // 简化 dts=pts
        enc_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;  // SPS/PPS 进 extradata，保证每段可独立解码
        if (std::string(encoder_name_) == "libx264") {
            av_opt_set(enc_ctx_->priv_data, "preset", "veryfast", 0);
            av_opt_set(enc_ctx_->priv_data, "tune", "zerolatency", 0);
        }
        if (avcodec_open2(enc_ctx_, enc_, nullptr) < 0) {
            fprintf(stderr, "[pipeline] 编码器 %s 打开失败\n", enc_name);
            avcodec_free_context(&enc_ctx_);
            return false;
        }
        // 导出编码器参数（含 SPS/PPS）
        AVCodecParameters* par = avcodec_parameters_alloc();
        avcodec_parameters_from_context(par, enc_ctx_);
        enc_params_.codec_id = par->codec_id;
        enc_params_.width = p_.width;
        enc_params_.height = p_.height;
        enc_params_.fps = p_.fps;
        enc_params_.bitrate_kbps = p_.bitrate_kbps;
        enc_params_.extradata.assign(par->extradata, par->extradata + par->extradata_size);
        avcodec_parameters_free(&par);
        return true;
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
        if (gst_mode_ && gst_enc_) gst_enc_->abort();  // 先解除 gst 阻塞
        // 有界等待线程退出（输入中断回调会快速解除 av 阻塞；正常 <1s 退出）
        for (int i = 0; i < 100 && thread_alive_.load(); i++) ::usleep(100 * 1000);
        bool alive = thread_alive_.load();
        if (thread_.joinable()) thread_.detach();
        thread_ = std::thread();  // 复位句柄：start() 可再次启动
        if (gst_mode_ && gst_enc_) {
            if (!alive) gst_enc_->flush();
            // 保留编码器上下文，支持 REST 再次 start()
        }
    }

    size_t add_frame_listener(std::function<void(const EncodeFrame&)> cb) override {
        std::lock_guard<std::mutex> lk(listeners_mu_);
        listeners_.push_back(std::move(cb));
        return listeners_.size();
    }

    void remove_frame_listener(size_t token) override {
        std::lock_guard<std::mutex> lk(listeners_mu_);
        if (token > 0 && token <= listeners_.size()) {
            listeners_[token - 1] = nullptr;  // 标记移除，回调时跳过
        }
    }

    bool snapshot(const std::string& photo_dir, int quality,
                  std::string* out_name) override {
        std::lock_guard<std::mutex> lk(snap_mu_);
        if (!snap_frame_ || !snap_frame_->data[0]) return false;
        // 用 mjpeg 编码器压缩最新一帧
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        ctx->width = p_.width;
        ctx->height = p_.height;
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->time_base = {1, p_.fps};
        ctx->qmin = ctx->qmax = 2 + (31 - 2) * (100 - quality) / 98;  // quality 1..100 -> q 2..31
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        AVFrame* yuvj = av_frame_alloc();
        yuvj->format = AV_PIX_FMT_YUVJ420P;
        yuvj->width = p_.width;
        yuvj->height = p_.height;
        if (av_frame_get_buffer(yuvj, 32) < 0) {
            av_frame_free(&yuvj);
            avcodec_free_context(&ctx);
            return false;
        }
        // yuv420p -> yuvj420p（全范围）
        SwsContext* sws = sws_getContext(p_.width, p_.height, AV_PIX_FMT_YUV420P,
                                         p_.width, p_.height, AV_PIX_FMT_YUVJ420P,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws) {
            sws_scale(sws, snap_frame_->data, snap_frame_->linesize, 0, p_.height,
                      yuvj->data, yuvj->linesize);
            sws_freeContext(sws);
        }
        int rc = avcodec_send_frame(ctx, yuvj);
        AVPacket* pkt = av_packet_alloc();
        bool ok = false;
        if (rc >= 0 && avcodec_receive_packet(ctx, pkt) >= 0) {
            std::error_code ec;
            std::filesystem::create_directories(photo_dir, ec);
            const std::string fname = ts_name() + ".jpg";
            const std::string path = photo_dir + "/" + fname;
            const std::string latest = photo_dir + "/latest.jpg";
            FILE* f = fopen(path.c_str(), "wb");
            if (f) {
                fwrite(pkt->data, 1, pkt->size, f);
                fclose(f);
                ok = true;
                if (out_name) *out_name = fname;
            }
            // 同步 latest.jpg（不打断录像）
            if (ok) {
                FILE* lf = fopen(latest.c_str(), "wb");
                if (lf) { fwrite(pkt->data, 1, pkt->size, lf); fclose(lf); }
            }
        }
        av_packet_free(&pkt);
        av_frame_free(&yuvj);
        avcodec_free_context(&ctx);
        return ok;
    }

    const EncoderParams& encoder_params() const override { return enc_params_; }
    bool running() const override { return running_.load(); }
    std::string status() const override { return encoder_name_; }
    double measured_fps() const override { return fps_meter_.fps(); }
    uint64_t last_frame_ms() const override { return last_frame_ms_.load(); }

    bool preview_jpeg(std::vector<uint8_t>& out, int max_w) override {
        std::lock_guard<std::mutex> lk(snap_mu_);
        if (!snap_frame_ || !snap_frame_->data[0]) return false;
        // 预览降采样到 max_w 宽度（640 默认），编码 JPEG
        int w = p_.width, h = p_.height;
        if (max_w > 0 && w > max_w) {
            h = h * max_w / w;
            w = max_w;
            if (h % 2) h++;
        }
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) return false;
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (!ctx) return false;
        ctx->width = w;
        ctx->height = h;
        ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx->time_base = {1, p_.fps};
        ctx->qmin = ctx->qmax = 6;  // 预览画质
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            avcodec_free_context(&ctx);
            return false;
        }
        AVFrame* yuvj = av_frame_alloc();
        yuvj->format = AV_PIX_FMT_YUVJ420P;
        yuvj->width = w;
        yuvj->height = h;
        if (av_frame_get_buffer(yuvj, 32) < 0) {
            av_frame_free(&yuvj);
            avcodec_free_context(&ctx);
            return false;
        }
        SwsContext* sws = sws_getContext(p_.width, p_.height, AV_PIX_FMT_YUV420P,
                                         w, h, AV_PIX_FMT_YUVJ420P,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws) {
            av_frame_free(&yuvj);
            avcodec_free_context(&ctx);
            return false;
        }
        sws_scale(sws, snap_frame_->data, snap_frame_->linesize, 0, p_.height,
                  yuvj->data, yuvj->linesize);
        sws_freeContext(sws);
        AVPacket* pkt = av_packet_alloc();
        bool ok = false;
        if (avcodec_send_frame(ctx, yuvj) >= 0 && avcodec_receive_packet(ctx, pkt) >= 0) {
            out.assign(pkt->data, pkt->data + pkt->size);
            ok = true;
        }
        av_packet_free(&pkt);
        av_frame_free(&yuvj);
        avcodec_free_context(&ctx);
        return ok;
    }

    void set_extradata_callback(std::function<void(const std::vector<uint8_t>&)> cb) override {
        extradata_cb_ = std::move(cb);
    }

private:
    void run() {
        thread_alive_.store(true);
        const int gen = generation_.load();
        // 快照帧缓冲（跨重连复用）
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            snap_frame_ = av_frame_alloc();
            snap_frame_->format = AV_PIX_FMT_YUV420P;
            snap_frame_->width = p_.width;
            snap_frame_->height = p_.height;
            av_frame_get_buffer(snap_frame_, 32);
        }
        AVPacket* ipkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* scaled = av_frame_alloc();
        scaled->format = AV_PIX_FMT_YUV420P;
        scaled->width = p_.width;
        scaled->height = p_.height;
        av_frame_get_buffer(scaled, 32);
        const int64_t t0 = av_gettime();

        // 外层重连循环：输入断开后自动重试（摄像头热插拔调试必需）
        while (running_.load() && gen == generation_.load()) {
            // ===== 打开输入 =====
            AVFormatContext* ifmt = avformat_alloc_context();
            ifmt->interrupt_callback.callback = interrupt_cb;
            ifmt->interrupt_callback.opaque = &running_;
            AVDictionary* opts = nullptr;
            std::string url;
            if (p_.mock) {
                ifmt->iformat = const_cast<AVInputFormat*>(av_find_input_format("lavfi"));
                url = "testsrc2=size=" + std::to_string(p_.width) + "x" +
                      std::to_string(p_.height) + ":rate=" + std::to_string(p_.fps);
            } else {
                url = p_.device;
#ifdef __linux__
                av_dict_set(&opts, "input_format", p_.input_format.c_str(), 0);
                av_dict_set(&opts, "video_size",
                            (std::to_string(p_.width) + "x" + std::to_string(p_.height)).c_str(), 0);
                av_dict_set(&opts, "framerate", std::to_string(p_.fps).c_str(), 0);
#else
                fprintf(stderr, "[pipeline] 非 Linux 平台仅支持 mock 模式\n");
                avformat_free_context(ifmt);
                running_.store(false);
                break;
#endif
            }
            int ret = avformat_open_input(&ifmt, url.c_str(), ifmt->iformat, &opts);
            av_dict_free(&opts);
            if (ret < 0) {
                fprintf(stderr, "[pipeline:%d] %s 打开失败: %s（3 秒后重试）\n",
                        p_.camera_id, url.c_str(), av_err(ret).c_str());
                avformat_free_context(ifmt);
                if (running_.load()) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
                break;
            }
            avformat_find_stream_info(ifmt, nullptr);
            int vs = av_find_best_stream(ifmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (vs < 0) {
                avformat_close_input(&ifmt);
                if (running_.load()) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
                break;
            }

            // 解码器
            const AVCodec* dec = avcodec_find_decoder(ifmt->streams[vs]->codecpar->codec_id);
            AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
            avcodec_parameters_to_context(dec_ctx, ifmt->streams[vs]->codecpar);
            // 多线程解码（MJPEG 切片级多线程可显著提速 1080p）
            dec_ctx->thread_count = 0;  // 0=自动（按核数）
            dec_ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
            if (avcodec_open2(dec_ctx, dec, nullptr) < 0) {
                avcodec_free_context(&dec_ctx);
                avformat_close_input(&ifmt);
                if (running_.load()) { std::this_thread::sleep_for(std::chrono::seconds(3)); continue; }
                break;
            }

            // 若解码器直接输出 I420 且行对齐，可跳过 swscale 零拷贝直推编码器（大幅降 CPU）
            bool direct420 = (dec_ctx->pix_fmt == AV_PIX_FMT_YUV420P ||
                              dec_ctx->pix_fmt == AV_PIX_FMT_YUVJ420P);
            // 缩放上下文（编码器已在 open() 中创建）
            SwsContext* sws = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                                             p_.width, p_.height, AV_PIX_FMT_YUV420P,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
            const double in_tb_ms = av_q2d(ifmt->streams[vs]->time_base) * 1000.0;
            const int64_t t0_wall = av_gettime_relative();
            uint64_t encoded_frames = 0;
            LOG_INFO("[pipeline:%d] %s 已连接", p_.camera_id, url.c_str());

            // ===== 采集/编码循环 =====
            while (running_.load() && gen == generation_.load()) {
                ret = av_read_frame(ifmt, ipkt);
                if (ret == AVERROR_EXIT || ret < 0) break;
                if (ipkt->stream_index != vs) {
                    av_packet_unref(ipkt);
                    continue;
                }
                if (avcodec_send_packet(dec_ctx, ipkt) >= 0) {
                    while (avcodec_receive_frame(dec_ctx, frame) >= 0) {
                        const bool direct = direct420 && frame->linesize[0] == frame->width;
                        // 快照缓存：direct 用 memcpy，否则 swscale
                        if (direct) {
                            std::lock_guard<std::mutex> lk(snap_mu_);
                            for (int pl = 0; pl < 3; pl++) {
                                int ph = (pl == 0) ? dec_ctx->height : dec_ctx->height / 2;
                                std::memcpy(snap_frame_->data[pl], frame->data[pl],
                                            static_cast<size_t>(frame->linesize[pl]) * ph);
                            }
                        } else {
                            std::lock_guard<std::mutex> lk(snap_mu_);
                            sws_scale(sws, frame->data, frame->linesize, 0, dec_ctx->height,
                                      snap_frame_->data, snap_frame_->linesize);
                        }
                        if (!direct) {
                            sws_scale(sws, frame->data, frame->linesize, 0, dec_ctx->height,
                                      scaled->data, scaled->linesize);
                            scaled->pts = frame->pts;
                        }
                        // mock 源按"已编码帧数"节流到实时帧率（真实相机自然实时，无需处理）
                        if (p_.mock) {
                            int64_t target_us = t0_wall + static_cast<int64_t>(encoded_frames * 1000000LL / p_.fps);
                            int64_t now_us = av_gettime_relative();
                            if (target_us > now_us) av_usleep(target_us - now_us);
                        }
                        if (gst_mode_ && gst_enc_) {
                            // GStreamer nvv4l2：推入原始帧并收集 H.264 包
                            const uint8_t* enc_src = direct ? frame->data[0] : scaled->data[0];
                            const int* enc_ls = direct ? frame->linesize : scaled->linesize;
                            size_t ysz = static_cast<size_t>(enc_ls[0]) * p_.height +
                                        static_cast<size_t>(enc_ls[1]) * (p_.height / 2) +
                                        static_cast<size_t>(enc_ls[2]) * (p_.height / 2);
                            // gst 推入 PTS 必须从 0 开始（NVIDIA 编码器对巨大初始 PTS 会卡死）
                            uint64_t frame_pts = encoded_frames * 1000ULL / p_.fps;
                            pushes_++;
                            if ((pushes_ % 60) == 0)
                                LOG_INFO("[pipeline:%d] gst 推入=%llu 拉出=%llu", p_.camera_id,
                                         (unsigned long long)pushes_, (unsigned long long)pulls_);
                            if (gst_enc_->push(enc_src, ysz, frame_pts)) {
                                std::vector<uint8_t> h264;
                                bool key = false;
                                uint64_t pts = 0;
                                while (gst_enc_->pull(&h264, &key, &pts)) {
                                    pulls_++;
                                    // SPS/PPS 先于首帧 emit 同步给录像服务（否则首段以空 avcC 打开）
                                    if (!extradata_sent_ && !gst_enc_->params().extradata.empty()) {
                                        extradata_sent_ = true;
                                        enc_params_.extradata = gst_enc_->params().extradata;
                                        if (extradata_cb_) extradata_cb_(gst_enc_->params().extradata);
                                    }
                                    EncodeFrame ef;
                                    ef.pts_ms = pts;
                                    ef.data = h264.data();
                                    ef.size = h264.size();
                                    ef.key = key;
                                    encoded_frames++;
                                    emit_frame(ef);
                                }
                            }
                        } else if (avcodec_send_frame(enc_ctx_, scaled) >= 0) {
                            encoded_frames++;
                            AVPacket* epkt = av_packet_alloc();
                            while (avcodec_receive_packet(enc_ctx_, epkt) >= 0) {
                                uint64_t pts_ms = 0;
                                if (frame->pts != AV_NOPTS_VALUE) {
                                    pts_ms = static_cast<uint64_t>(
                                        frame->pts * av_q2d(ifmt->streams[vs]->time_base) * 1000);
                                } else {
                                    pts_ms = static_cast<uint64_t>((av_gettime() - t0) / 1000);
                                }
                                EncodeFrame ef;
                                ef.pts_ms = pts_ms;
                                ef.data = epkt->data;
                                ef.size = epkt->size;
                                ef.key = (epkt->flags & AV_PKT_FLAG_KEY) != 0;
                                emit_frame(ef);
                                av_packet_unref(epkt);
                            }
                            av_packet_free(&epkt);
                        }
                        av_frame_unref(frame);
                    }
                }
                av_packet_unref(ipkt);
            }

            // ===== 关闭输入侧（编码器保留，供重连复用）=====
            sws_freeContext(sws);
            avcodec_free_context(&dec_ctx);
            avformat_close_input(&ifmt);
            if (running_.load() && gen == generation_.load()) {
                LOG_INFO("[pipeline:%d] 输入中断，3 秒后重连", p_.camera_id);
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
        }
        thread_alive_.store(false);

        // 注意：此处不再 flush 编码器（send_frame(NULL) 会使编码器进入 draining 状态，
        // 重启后 send 帧会返回 AVERROR_EOF 导致无法录像）。
        // libx264 配置为 zerolatency + max_b_frames=0，无缓冲帧，退出无需补帧；
        // 段文件收尾由 Recorder::stop_camera 的 av_write_trailer 完成。

        av_frame_free(&frame);
        av_frame_free(&scaled);
        {
            std::lock_guard<std::mutex> lk(snap_mu_);
            av_frame_free(&snap_frame_);
            snap_frame_ = nullptr;
        }
    }

    void emit_frame(const EncodeFrame& f) {
        fps_meter_.tick();
        last_frame_ms_.store(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        std::vector<std::function<void(const EncodeFrame&)>> cbs;
        {
            std::lock_guard<std::mutex> lk(listeners_mu_);
            for (auto& cb : listeners_) {
                if (cb) cbs.push_back(cb);
            }
        }
        for (auto& cb : cbs) cb(f);
    }

    void close_all() {
        // 线程退出后资源已在 run() 释放
    }

private:
    PipelineParams p_;
    const AVCodec* enc_ = nullptr;
    AVCodecContext* enc_ctx_ = nullptr;
    std::string encoder_name_;
    EncoderParams enc_params_;
    std::mutex listeners_mu_;
    std::vector<std::function<void(const EncodeFrame&)>> listeners_;
    std::atomic<bool> running_{false};
    std::atomic<bool> thread_alive_{false};
    std::atomic<int> generation_{0};
    std::thread thread_;
    FpsMeter fps_meter_;
    std::atomic<uint64_t> last_frame_ms_{0};

    bool gst_mode_ = false;
    uint64_t pushes_ = 0, pulls_ = 0;   // 诊断计数
    std::unique_ptr<GstH264Encoder> gst_enc_;
    std::function<void(const std::vector<uint8_t>&)> extradata_cb_;
    bool extradata_sent_ = false;

    std::mutex snap_mu_;
    AVFrame* snap_frame_ = nullptr;  // 缓存最新解码帧（yuv420p）
};

std::unique_ptr<Pipeline> create_pipeline() {
    return std::make_unique<FfmpegPipeline>();
}

}  // namespace camera
