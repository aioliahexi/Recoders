// 相机管理实现
#include "camera_manager.h"

#include <mutex>

#include "device/device_discovery.h"
#include "media/gst_capture.h"
#include "media/pipeline.h"
#include "media/recorder.h"
#include "util/log.h"

namespace camera {

namespace {

std::map<std::string, std::string> dev_uuid_map() {
    std::map<std::string, std::string> m;
    for (auto& d : discover_uvc_cameras(false))  // 仅身份，不打开设备
        if (d.is_uvc && !d.v4l2_device.empty() && !d.uuid.empty())
            m[d.v4l2_device] = d.uuid;
    return m;
}

}  // namespace

std::string device_uuid(const std::string& dev) {
    auto m = dev_uuid_map();
    auto it = m.find(dev);
    return it == m.end() ? "" : it->second;
}

std::pair<uint16_t, uint16_t> device_vidpid(const std::string& dev) {
    for (auto& d : discover_uvc_cameras(false))
        if (d.is_uvc && d.v4l2_device == dev)
            return {d.vid, d.pid};
    return {0, 0};
}

int next_camera_id(Context& ctx) {
    std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
    int id = 1;
    while (ctx.pipelines.count(id)) id++;
    return id;
}

bool launch_camera(Context& ctx, Recorder* rec, const CameraSpec& spec,
                   bool autostart, std::string* err) {
    if (err) err->clear();
    int id = spec.id > 0 ? spec.id : next_camera_id(ctx);
    if (ctx.pipelines.count(id)) {
        if (err) *err = "相机 id 已存在";
        return false;
    }
    PipelineParams p;
    p.camera_id = id;
    p.name = spec.name;
    p.device = spec.device;
    p.mock = spec.mock;
    p.input_format = spec.input_format;
    p.width = spec.width;
    p.height = spec.height;
    p.fps = spec.fps;
    p.bitrate_kbps = spec.bitrate_kbps;
    p.gop = spec.gop;
    p.encoder = ctx.cfg.encoder;
    p.capture = spec.capture;
    p.osd = spec.osd;

    // 应用统一 config.json 中的自定义名（UUID 绑定优先，其次 id）
    auto rj = load_recorder_json(ctx.cfg.config_path);
    const std::string uuid = device_uuid(p.device);
    std::string name = spec.name;
    {
        std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
        ctx.params[id] = p;
        ctx.names[id] = name;
        CameraCfg* found = nullptr;
        if (!uuid.empty()) {
            for (auto& r : rj.cameras)
                if (r.uuid == uuid) { found = &r; break; }
        }
        if (!found) {
            for (auto& r : rj.cameras)
                if (r.id == id) { found = &r; break; }
        }
        if (found) {
            if (!found->name.empty()) name = found->name;
            found->id = id;
            found->device = p.device;
            if (!uuid.empty() && found->uuid.empty()) found->uuid = uuid;
        } else {
            CameraCfg r;
            r.id = id; r.uuid = uuid; r.name = spec.name; r.device = p.device;
            r.width = p.width; r.height = p.height; r.fps = p.fps;
            r.bitrate_kbps = p.bitrate_kbps; r.gop = p.gop;
            r.input_format = p.input_format; r.capture = p.capture; r.osd = p.osd;
            rj.cameras.push_back(r);
        }
        ctx.uuids[id] = uuid;
        ctx.names[id] = name;
        ctx.params[id].name = name;
        p.name = name;   // 关键：同步本地参数，否则 gst 管道 OSD 水印用 config 默认名(CAM01/CAM02)
    }
    save_recorder_json(ctx.cfg.config_path, rj);

    auto pipe = (p.capture == "gst") ? create_gst_capture() : create_pipeline();
    if (!pipe) pipe = create_pipeline();
    if (!pipe->open(p)) {
        if (err) *err = "管道初始化失败";
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
        ctx.pipelines[id] = std::move(pipe);
    }
    rec->start_camera(id, ctx.names[id], ctx.pipelines[id]->encoder_params());
    rec->set_camera_uuid(id, uuid);
    {
        std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
        ctx.vp[id] = device_vidpid(p.device);
    }
    ctx.pipelines[id]->add_frame_listener([rec, id](const EncodeFrame& f) {
        rec->on_frame(id, f);
    });
    ctx.pipelines[id]->set_extradata_callback([rec, id](const std::vector<uint8_t>& ex) {
        rec->set_extradata(id, ex);
    });
    if (autostart) ctx.pipelines[id]->start();
    return true;
}

bool remove_camera(Context& ctx, Recorder* rec, int id, std::string* err) {
    if (err) err->clear();
    std::unique_ptr<Pipeline> pipe;
    std::string name;
    std::string uuid;
    {
        std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
        auto it = ctx.pipelines.find(id);
        if (it == ctx.pipelines.end()) {
            if (err) *err = "相机不存在";
            return false;
        }
        pipe = std::move(it->second);
        ctx.pipelines.erase(it);
        name = ctx.names.count(id) ? ctx.names[id] : "";
        uuid = ctx.uuids.count(id) ? ctx.uuids[id] : "";
        ctx.params.erase(id);
        ctx.names.erase(id);
        ctx.uuids.erase(id);
    }
    if (pipe) pipe->stop();
    rec->stop_camera(id);
    // 从统一 config.json 移除（保留数据目录）
    auto rj = load_recorder_json(ctx.cfg.config_path);
    rj.cameras.erase(
        std::remove_if(rj.cameras.begin(), rj.cameras.end(),
                       [&](const CameraCfg& r) {
                           return r.id == id || (!uuid.empty() && r.uuid == uuid);
                       }),
        rj.cameras.end());
    save_recorder_json(ctx.cfg.config_path, rj);
    LOG_INFO("[camera] 删除相机 id=%d name=%s uuid=%s", id, name.c_str(), uuid.c_str());
    return true;
}

}  // namespace camera
