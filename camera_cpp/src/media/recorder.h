// 录像服务：分段 MP4 + SQLite 索引 + 配额循环覆盖
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../db.h"
#include "pipeline.h"

namespace camera {

struct SegmentInfo {
    int id = 0;
    int camera_id = 0;
    std::string camera_name;
    uint64_t start_ms = 0, end_ms = 0;
    uint64_t size_bytes = 0;
    std::string path;
};

class Recorder {
public:
    virtual ~Recorder() = default;

    // db 由外部传入（与 REST/ONVIF 共用同一索引连接）
    virtual bool open(const std::string& data_dir, uint64_t quota_bytes,
                      int segment_time_s, Db* db) = 0;
    // 每路相机各自的编码参数（分辨率/码率/SPS 不同，必须分开建流）
    virtual bool start_camera(int camera_id, const std::string& name,
                              const EncoderParams& enc) = 0;

    // GStreamer 编码器首关键帧后补充 SPS/PPS（建流时读取）
    virtual void set_extradata(int camera_id, const std::vector<uint8_t>& ex) = 0;

    // 物理相机设备 UUID（用于录像索引绑定设备）
    virtual void set_camera_uuid(int camera_id, const std::string& uuid) = 0;
    virtual bool stop_camera(int camera_id) = 0;

    // 编码帧回调（由 Pipeline 线程调用）
    virtual void on_frame(int camera_id, const EncodeFrame& frame) = 0;

    virtual int rescan() = 0;                       // 启动时重建索引
    virtual int enforce_quota() = 0;                // 超配额删最旧

    virtual std::vector<SegmentInfo> list_segments(int camera_id,
                                                   uint64_t start_ms,
                                                   uint64_t end_ms,
                                                   int page, int page_size,
                                                   int* total) = 0;

    // 统计：段总数 + 总占用字节
    virtual void stats(int* count, uint64_t* bytes) = 0;

    // 清空全部录像：删除文件与索引，返回删除段数（调用前应 stop_camera 封段）
    virtual int clear_recordings() = 0;
};

std::unique_ptr<Recorder> create_recorder();

}  // namespace camera
