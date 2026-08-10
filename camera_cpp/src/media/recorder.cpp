// 录像服务实现：MP4 分段写入（libavformat）+ SQLite 索引 + 配额循环覆盖
#include "recorder.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "../db.h"
#include "../util/log.h"

namespace camera {

namespace fs = std::filesystem;

namespace {

uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string av_err(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    return buf;
}

std::string ts_name(uint64_t ms) {
    std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);  // 统一 UTC，与 parse_segment_ms(timegm) 一致
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    return buf;
}

uint64_t parse_segment_ms(const std::string& name) {
    std::regex re(R"(^(\d{8})_(\d{6})\.mp4$)");
    std::smatch m;
    if (!std::regex_match(name, m, re)) return 0;
    std::tm tm{};
    std::string s = m[1].str() + m[2].str();
    strptime(s.c_str(), "%Y%m%d%H%M%S", &tm);
    std::time_t t = timegm(&tm);
    return static_cast<uint64_t>(t) * 1000;
}

}  // namespace

class SegmentRecorder : public Recorder {
public:
    ~SegmentRecorder() override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [id, st] : cur_) finalize_locked(id);
    }

    bool open(const std::string& data_dir, uint64_t quota_bytes,
              int segment_time_s, Db* db) override {
        std::lock_guard<std::mutex> lk(mu_);
        data_dir_ = data_dir;
        quota_ = quota_bytes;
        seg_time_ms_ = static_cast<uint64_t>(segment_time_s) * 1000;
        db_ = db;  // 共享索引连接
        fs::create_directories(data_dir_ + "/recordings");
        fs::create_directories(data_dir_ + "/db");
        return true;
    }

    bool start_camera(int id, const std::string& name, const EncoderParams& enc) override {
        std::lock_guard<std::mutex> lk(mu_);
        cam_name_[id] = name;
        enc_map_[id] = enc;
        fs::create_directories(rec_dir_locked(id));
        return true;
    }

    bool stop_camera(int id) override {
        std::lock_guard<std::mutex> lk(mu_);
        finalize_locked(id);
        return true;
    }

    void on_frame(int id, const EncodeFrame& frame) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto& st = cur_[id];
        if (!st.fmt) open_segment_locked(id, frame.pts_ms);
        if (!st.fmt) return;
        // 轮转：关键帧到达且超时 → 先封当前段，再开新段，本关键帧作为新段首帧
        // （保证每个 MP4 段都以 IDR 开头，可独立解码）
        if (frame.key && frame.pts_ms >= st.start_pts_ms + seg_time_ms_) {
            finalize_locked(id);
            open_segment_locked(id, frame.pts_ms);
            if (!st.fmt) return;
        }
        if (!st.pkt) st.pkt = av_packet_alloc();
        st.pkt->data = const_cast<uint8_t*>(frame.data);
        st.pkt->size = static_cast<int>(frame.size);
        // 毫秒 PTS -> muxer 实际 time_base（write_header 后由 mov muxer 决定）
        {
            AVRational ms_tb{1, 1000};
            AVRational out_tb{st.tb_num, st.tb_den};
            st.pkt->pts = av_rescale_q(static_cast<int64_t>(frame.pts_ms), ms_tb, out_tb);
            st.pkt->dts = st.pkt->pts;
            st.pkt->duration = av_rescale_q(1000 / std::max(enc_map_.at(id).fps, 1), ms_tb, out_tb);
        }
        st.pkt->stream_index = 0;
        st.pkt->flags = (frame.key ? AV_PKT_FLAG_KEY : 0);
        int wrc = av_interleaved_write_frame(st.fmt, st.pkt);
        if (wrc < 0) {
            LOG_ERROR("[recorder] cam %d 写帧失败 pts=%llu key=%d size=%d: %s",
                      id, (unsigned long long)frame.pts_ms, frame.key ? 1 : 0,
                      (int)frame.size, av_err(wrc).c_str());
        }
        av_packet_unref(st.pkt);
        st.bytes += frame.size;
    }

    void stats(int* count, uint64_t* bytes) override {
        std::lock_guard<std::mutex> lk(mu_);
        if (count) *count = db_->count();
        if (bytes) *bytes = db_->total_size();
    }

    void set_extradata(int id, const std::vector<uint8_t>& ex) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = enc_map_.find(id);
        if (it != enc_map_.end()) it->second.extradata = ex;
    }

    void set_camera_uuid(int id, const std::string& uuid) override {
        std::lock_guard<std::mutex> lk(mu_);
        cam_uuid_[id] = uuid;
    }

    int rescan() override {
        std::lock_guard<std::mutex> lk(mu_);
        int n = 0;
        for (auto& [id, name] : cam_name_) {
            std::string dir = rec_dir_locked(id);
            if (!fs::is_directory(dir)) continue;
            for (const auto& e : fs::directory_iterator(dir)) {
                if (e.path().extension() != ".mp4") continue;
                const std::string path = e.path().string();
                const std::string fname = e.path().filename().string();
                uint64_t start = parse_segment_ms(fname);
                if (!start) continue;
                uint64_t size = fs::file_size(e.path());
                auto exist = db_->get_by_path(path);
                if (exist && exist->size_bytes == size) continue;
                // 估算时长：size*8/码率（用该相机自身码率）
                int bps = 0;
                auto eit = enc_map_.find(id);
                if (eit != enc_map_.end()) bps = eit->second.bitrate_kbps;
                uint64_t dur_ms = (bps > 0) ? size * 8000ULL / (bps * 1000ULL) : 0;
                DbSegment seg{0, id, name,
                              cam_uuid_.count(id) ? cam_uuid_[id] : "",
                              start, start + dur_ms, size, path};
                db_->upsert(seg);
                n++;
            }
        }
        return n;
    }

    int enforce_quota() override {
        std::lock_guard<std::mutex> lk(mu_);
        int deleted = 0;
        while (db_->total_size() > quota_) {
            auto rows = db_->oldest(20);
            if (rows.empty()) break;
            for (auto& r : rows) {
                if (fs::exists(r.path)) {
                    std::error_code ec;
                    fs::remove(r.path, ec);
                }
                db_->remove_by_id(r.id);
                deleted++;
                if (db_->total_size() <= quota_) break;
            }
        }
        return deleted;
    }

    int clear_recordings() override {
        std::lock_guard<std::mutex> lk(mu_);
        int total = 0;
        int n = 0;
        auto rows = db_->list(0, 0, 0, 1, 1000000, &total);
        for (auto& r : rows) {
            std::error_code ec;
            if (fs::exists(r.path)) fs::remove(r.path, ec);
            db_->remove_by_id(r.id);
            n++;
        }
        return n;
    }

    std::vector<SegmentInfo> list_segments(int camera_id, uint64_t start_ms,
                                           uint64_t end_ms, int page, int page_size,
                                           int* total) override {
        auto rows = db_->list(camera_id, start_ms, end_ms, page, page_size, total);
        std::vector<SegmentInfo> out;
        out.reserve(rows.size());
        for (auto& r : rows) {
            out.push_back({static_cast<int>(r.id), r.camera_id, r.camera_name,
                           r.start_ms, r.end_ms, r.size_bytes, r.path});
        }
        return out;
    }

private:
    std::string rec_dir_locked(int id) const {
        auto it = cam_name_.find(id);
        return data_dir_ + "/recordings/" + (it == cam_name_.end() ? std::to_string(id) : it->second);
    }

    void open_segment_locked(int id, uint64_t first_pts_ms) {
        auto& st = cur_[id];
        if (st.fmt) return;
        auto enc_it = enc_map_.find(id);
        if (enc_it == enc_map_.end()) return;  // 未注册的相机不录像
        const EncoderParams& enc = enc_it->second;
        const std::string path = rec_dir_locked(id) + "/" + ts_name(now_ms()) + ".mp4";
        if (avformat_alloc_output_context2(&st.fmt, nullptr, "mp4", path.c_str()) < 0) {
            st.fmt = nullptr;
            return;
        }
        st.stream = avformat_new_stream(st.fmt, nullptr);
        st.stream->time_base = AVRational{static_cast<int>(enc.tb_num), static_cast<int>(enc.tb_den)};
        st.stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        st.stream->codecpar->codec_id = static_cast<AVCodecID>(enc.codec_id);
        st.stream->codecpar->width = enc.width;
        st.stream->codecpar->height = enc.height;
        st.stream->codecpar->format = AV_PIX_FMT_YUV420P;
        st.stream->codecpar->bit_rate = enc.bitrate_kbps * 1000;
        if (!enc.extradata.empty()) {
            st.stream->codecpar->extradata = static_cast<uint8_t*>(
                av_mallocz(enc.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            std::memcpy(st.stream->codecpar->extradata, enc.extradata.data(), enc.extradata.size());
            st.stream->codecpar->extradata_size = static_cast<int>(enc.extradata.size());
        }
        int rc_io = avio_open(&st.fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
        // 分段 MP4（fMP4）：moov 前置 + 按关键帧/2s 分片，录制中的片段也可实时播放
        av_opt_set(st.fmt->priv_data, "movflags",
                   "frag_keyframe+empty_moov+default_base_moof", 0);
        av_opt_set(st.fmt->priv_data, "frag_duration", "2000000", 0);  // 2s 分片（微秒）
        int rc_hdr = rc_io < 0 ? 0 : avformat_write_header(st.fmt, nullptr);
        if (rc_io < 0 || rc_hdr < 0) {
            LOG_ERROR("[recorder] 打开 %s 失败: io=%d hdr=%d%s%s",
                      path.c_str(), rc_io, rc_hdr,
                      rc_io < 0 ? av_err(rc_io).c_str() : "",
                      rc_hdr < 0 ? av_err(rc_hdr).c_str() : "");
            if (st.fmt->pb) avio_closep(&st.fmt->pb);
            avformat_free_context(st.fmt);
            st = SegmentState{};
            return;
        }
        st.start_pts_ms = first_pts_ms;  // 旋转判定用（流时间）
        st.start_ms = now_ms();           // 入库用（墙钟 UTC）
        st.path = path;
        // write_header 后记录 muxer 实际 time_base
        st.tb_num = st.stream->time_base.num;
        st.tb_den = st.stream->time_base.den;
    }

    void finalize_locked(int id) {
        auto& st = cur_[id];
        if (!st.fmt) return;
        av_write_trailer(st.fmt);
        if (st.fmt->pb) avio_closep(&st.fmt->pb);
        avformat_free_context(st.fmt);
        st.fmt = nullptr;
        uint64_t end = now_ms();
        uint64_t size = 0;
        if (!st.path.empty()) {
            std::error_code ec;
            uintmax_t sz = fs::file_size(st.path, ec);
            if (!ec) size = sz;
        }
        DbSegment seg{0, id, cam_name_.count(id) ? cam_name_[id] : "",
                      cam_uuid_.count(id) ? cam_uuid_[id] : "",
                      st.start_ms, end, size, st.path};
        db_->upsert(seg);
        st = SegmentState{};
    }

    struct SegmentState {
        AVFormatContext* fmt = nullptr;
        AVStream* stream = nullptr;
        AVPacket* pkt = nullptr;
        uint64_t start_pts_ms = 0;  // 首帧流时间（旋转判定）
        uint64_t start_ms = 0;      // 墙钟 UTC（入库）
        uint64_t bytes = 0;
        std::string path;
        int tb_num = 1, tb_den = 1000;  // muxer 实际时间基
    };

    std::string data_dir_;
    uint64_t quota_ = 0;
    uint64_t seg_time_ms_ = 300000;
    std::map<int, EncoderParams> enc_map_;
    Db* db_ = nullptr;  // 非拥有
    std::map<int, std::string> cam_name_;
    std::map<int, std::string> cam_uuid_;   // camera_id -> 设备 UUID
    std::map<int, SegmentState> cur_;
    std::mutex mu_;
};

std::unique_ptr<Recorder> create_recorder() {
    return std::make_unique<SegmentRecorder>();
}

}  // namespace camera
