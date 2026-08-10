// camera_server 主程序：录像 + RESTful + ONVIF + RTSP 全服务装配
// 用法:
//   camera_server --config config.mock.ini            # 常驻运行（Ctrl+C 停止）
//   camera_server --config config.ini --list          # 列出 UVC 摄像头
//   camera_server --config x.ini --duration 20 --snapshot  # 定时退出 + 抓拍
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <unistd.h>

extern "C" {
#include <libavutil/log.h>
}

#include "camera_manager.h"
#include "config.h"
#include "db.h"
#include "device/device_discovery.h"
#include "media/gst_capture.h"
#include "media/pipeline.h"
#include "media/recorder.h"
#include "net/http_server.h"
#include "onvif/onvif_service.h"
#include "rest/rest_service.h"
#include "rtsp/rtsp_server.h"
#include "web/web_ui.h"
#include "services.h"
#include "util/log.h"

using namespace camera;

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

int main(int argc, char** argv) {
    AppCfg cfg = load_config(argc, argv);
    av_log_set_level(AV_LOG_ERROR);

    if (cfg.list_devices) {
        auto cams = discover_uvc_cameras();
        LOG_INFO("发现 %zu 路 UVC 摄像头:", cams.size());
        for (const auto& c : cams) std::cout << "  - " << describe(c) << "\n";
        return 0;
    }

    if (cfg.host_ip.empty()) cfg.host_ip = detect_host_ip();
    LOG_INFO("camera_server 启动 | data=%s quota=%lluMB seg=%ds http=%d rtsp=%d ip=%s encoder=%s",
             cfg.data_dir.c_str(), (unsigned long long)cfg.quota_mb, cfg.segment_time_s,
             cfg.http_port, cfg.rtsp_port, cfg.host_ip.c_str(), cfg.encoder.c_str());

    // 运行时上下文
    Context ctx;
    ctx.cfg = cfg;
    ctx.started_ms = now_ms();
    Db db(cfg.data_dir + "/db/records.db");
    ctx.db = &db;
    auto rec = create_recorder();
    rec->open(cfg.data_dir, cfg.quota_mb * 1024 * 1024, cfg.segment_time_s, &db);
    ctx.recorder = rec.get();

    // 设备发现：/dev/videoN -> 设备 UUID（物理端口+VID/PID+序列号 派生）
    std::map<std::string, std::string> dev_uuid;
    for (auto& d : discover_uvc_cameras(false)) {  // 仅身份，不打开设备（避免与采集冲突）
        if (d.is_uvc && !d.v4l2_device.empty() && !d.uuid.empty())
            dev_uuid[d.v4l2_device] = d.uuid;
    }
    // 统一 config.json：系统静态相机 + 运行时注册相机已合并，直接启动（含离线相机）
    for (const auto& c : cfg.cameras) {
        CameraSpec sp;
        sp.id = c.id;
        sp.name = c.name;
        sp.device = c.device;
        sp.mock = c.mock;
        sp.input_format = c.input_format;
        sp.width = c.width; sp.height = c.height; sp.fps = c.fps;
        sp.bitrate_kbps = c.bitrate_kbps; sp.gop = c.gop;
        sp.capture = c.capture; sp.osd = c.osd;
        launch_camera(ctx, rec.get(), sp, false, nullptr);
    }
    ctx.rest_public = cfg.rest_public;   // 免鉴权 RESTful 开关（启动恢复）

    // HTTP：RESTful + ONVIF SOAP
    net::HttpServer http;
    if (!http.start(cfg.http_port)) {
        LOG_ERROR("HTTP 端口 %d 绑定失败", cfg.http_port);
        return 1;
    }
    if (!cfg.api_key.empty()) {
        http.auth_check = [&ctx, key = cfg.api_key](const net::Request& r) {
            if (ctx.rest_public.load()) return true;  // 免鉴权 RESTful（调试开关，Web 系统页可切换）
            if (r.path.rfind("/onvif/", 0) == 0) return true;  // ONVIF 走 WS-Security
            if (r.path == "/" || r.path == "/index.html") return true;  // Web 管理页
            if (r.path == "/health") return true;  // 健康检查免鉴权
            return r.header("authorization") == "Bearer " + key ||
                   r.header("x-api-key") == key;
        };
    }
    RestService rest(&ctx);
    rest.setup(http);
    setup_web_ui(http, &ctx);

    OnvifService onvif(&ctx);
    ctx.onvif = &onvif;
    http.route("POST", "/onvif/device_service", [&](const net::Request& r) { return onvif.handle_soap(r); });
    http.route("GET", "/onvif/device_service", [&](const net::Request& r) { return onvif.handle_soap(r); });
    onvif.start(cfg.http_port, cfg.rtsp_port, cfg.host_ip);
    onvif.set_discovery(cfg.onvif_discovery);

    // RTSP
    RtspServer rtsp(&ctx);
    rtsp.start(cfg.rtsp_port, cfg.host_ip);

    // 启动录像
    if (cfg.autostart) {
        std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
        for (auto& [id, p] : ctx.pipelines) p->start();
        LOG_INFO("共 %zu 路开始录像", ctx.pipelines.size());
    }

    // ---------- 热插拔监控线程 ----------
    // 每 2 秒扫描 USB：新 UUID 自动注册相机；拔线保留配置/历史并显示离线；
    // 插回同 UUID 恢复；设备路径变化自动重配
    std::thread hotplug([&] {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::map<std::string, UvcCameraInfo> by_uuid, by_dev;
            for (auto& d : discover_uvc_cameras(false)) {
                if (!d.is_uvc || d.uuid.empty()) continue;
                // 同一 UUID 有多个视频节点（如第二节点无采集格式）时，
                // 优先保留"已被某相机使用的节点"；否则保留第一个
                auto& slot = by_uuid[d.uuid];
                bool is_managed_dev = false;
                for (auto& [id, prm] : ctx.params)
                    if (prm.device == d.v4l2_device) { is_managed_dev = true; break; }
                if (slot.v4l2_device.empty() || is_managed_dev) slot = d;
                by_dev[d.v4l2_device] = d;
            }
            std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
            // 1) 在线状态 / 设备路径变化 / 启动时未绑定 uuid 的相机
            for (auto& [id, p] : ctx.pipelines) {
                std::string& uuid = ctx.uuids[id];
                if (uuid.empty()) {
                    auto it = by_dev.find(ctx.params[id].device);
                    if (it != by_dev.end()) {
                        uuid = it->second.uuid;
                        ctx.uuids[id] = uuid;
                        rec->set_camera_uuid(id, uuid);
                        auto rj = load_recorder_json(cfg.config_path);
                        for (auto& r : rj.cameras)
                            if (r.id == id) { r.uuid = uuid; break; }
                        save_recorder_json(cfg.config_path, rj);
                    }
                    continue;
                }
                auto du = by_uuid.find(uuid);
                if (du != by_uuid.end()) {
                    if (ctx.params[id].device != du->second.v4l2_device) {
                        LOG_INFO("[hotplug] 相机 %d (%s) 设备 %s -> %s", id,
                                 ctx.names[id].c_str(), ctx.params[id].device.c_str(),
                                 du->second.v4l2_device.c_str());
                        ctx.params[id].device = du->second.v4l2_device;
                        bool was = p->running();
                        p->stop();
                        p->open(ctx.params[id]);
                        ctx.recorder->start_camera(id, ctx.names[id], p->encoder_params());
                        if (was) p->start();
                    }
                    // 设备在且路径不变：pipeline 重连循环自行恢复，无需干预
                }
                // uuid 未出现 → 离线：配置与历史保留，pipeline 重连循环继续重试
            }
            // 2) 注册新 UUID 相机
            bool found_new = false;
            for (auto& [uuid, d] : by_uuid) {
                bool known = false;
                for (auto& [id, u] : ctx.uuids) if (u == uuid) { known = true; break; }
                if (known) continue;
                // 新相机：探测格式并校验（仅注册有可用采集格式的节点，排除第二节点）
                UvcCameraInfo info = d;
                for (auto& x : discover_uvc_cameras(true))
                    if (x.uuid == uuid && !x.formats.empty()) { info = x; break; }
                if (info.formats.empty()) {
                    LOG_INFO("[hotplug] 跳过无采集格式的节点 %s (uuid=%s)", d.v4l2_device.c_str(), uuid.c_str());
                    continue;
                }
                // 无序列号相机换 USB 口：新 uuid 但同 vid/pid，若恰好一台同型号离线相机 → 重绑定（保持配置/名称/历史）
                if (d.serial.empty()) {
                    uint64_t nowm = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count());
                    int match = -1, cnt = 0;
                    for (auto& [cid, cp] : ctx.pipelines) {
                        uint64_t lf = cp->last_frame_ms();
                        bool online = lf > 0 && (nowm - lf) < 3000;
                        if (!online && ctx.vp.count(cid) &&
                            ctx.vp[cid].first == d.vid && ctx.vp[cid].second == d.pid) {
                            match = cid; cnt++;
                        }
                    }
                    if (cnt == 1) {
                        int cid = match;
                        LOG_INFO("[hotplug] 相机 %d (%s) 换口重绑定 uuid=%s 设备=%s",
                                 cid, ctx.names[cid].c_str(), uuid.c_str(), d.v4l2_device.c_str());
                        ctx.uuids[cid] = uuid;
                        ctx.params[cid].device = d.v4l2_device;
                        rec->set_camera_uuid(cid, uuid);
                        auto rj2 = load_recorder_json(cfg.config_path);
                        for (auto& r : rj2.cameras)
                            if (r.id == cid) { r.uuid = uuid; r.device = d.v4l2_device; break; }
                        save_recorder_json(cfg.config_path, rj2);
                        // 重配管道到新设备
                        auto& cp = ctx.pipelines[cid];
                        bool was = cp->running();
                        cp->stop();
                        cp->open(ctx.params[cid]);
                        ctx.recorder->start_camera(cid, ctx.names[cid], cp->encoder_params());
                        if (was) cp->start();
                        log_operation(&ctx, cid, "camera_rebind",
                                      "相机换口重绑定 " + d.v4l2_device, 1);
                        continue;
                    }
                }
                int id = next_camera_id(ctx);
                CameraSpec sp;
                sp.id = id;
                sp.name = info.product.empty() ? ("CAM" + std::to_string(id)) : info.product;
                sp.device = info.v4l2_device;
                int bw = 0, bh = 0, bf = 0;
                std::string fmt = "mjpeg";
                for (auto& f : info.formats) {
                    if (f.width < 640 || f.fps < 15) continue;
                    if (f.width * f.height > bw * bh) {
                        bw = f.width; bh = f.height; bf = f.fps;
                        fmt = (f.fourcc == 0x34363248u /*H264*/) ? "h264"
                            : (f.fourcc == 0x47504a4du /*MJPG*/) ? "mjpeg" : "mjpeg";
                    }
                }
                if (bw == 0) { bw = 1920; bh = 1080; bf = 30; }
                sp.width = bw; sp.height = bh; sp.fps = bf;
                sp.bitrate_kbps = 3000; sp.gop = 60;
                sp.input_format = fmt; sp.capture = "gst"; sp.osd = true;
                LOG_INFO("[hotplug] 注册新相机 id=%d name=%s dev=%s uuid=%s (%dx%d@%d %s)",
                         id, sp.name.c_str(), sp.device.c_str(), uuid.c_str(), bw, bh, bf, fmt.c_str());
                std::string err;
                if (launch_camera(ctx, rec.get(), sp, true, &err)) {
                    log_operation(&ctx, id, "camera_register",
                                  "新相机注册 " + sp.name + " " + d.v4l2_device, 1);
                    found_new = true;
                } else {
                    LOG_ERROR("[hotplug] 新相机注册失败: %s", err.c_str());
                }
            }
            (void)found_new;
        }
    });
    hotplug.detach();

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);  // 流式发送断开不杀进程（配合 MSG_NOSIGNAL）

    // 主循环：状态打印
    auto t0 = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (cfg.run_seconds > 0 &&
            std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(cfg.run_seconds)) {
            break;
        }
        int cnt = 0;
        uint64_t bytes = 0;
        rec->stats(&cnt, &bytes);
        rec->rescan();
        {
            std::lock_guard<std::recursive_mutex> lk(ctx.cam_mu);
            LOG_INFO("[运行中] 相机=%zu 录像段=%d 占用=%lluMB http=:%d rtsp=:%d",
                     ctx.pipelines.size(), cnt, (unsigned long long)(bytes / 1024 / 1024),
                     cfg.http_port, cfg.rtsp_port);
        }
    }

    // 看门狗：优雅关闭若在 12 秒内未完成，强制退出进程（进程退出会回收所有线程/资源）
    std::thread watchdog([] {
        std::this_thread::sleep_for(std::chrono::seconds(12));
        _exit(0);
    });
    watchdog.detach();

    // 可选：停止前抓拍
    if (cfg.snapshot) {
        for (auto& [id, p] : ctx.pipelines) {
            std::string dir = cfg.data_dir + "/photos/" + ctx.names[id];
            if (p->snapshot(dir, 90)) LOG_INFO("抓拍完成: %s/latest.jpg", dir.c_str());
            else LOG_ERROR("抓拍失败: %s", ctx.names[id].c_str());
        }
    }

    // 收尾
    for (auto& [id, p] : ctx.pipelines) p->stop();
    for (auto& [id, name] : ctx.names) rec->stop_camera(id);
    rec->rescan();
    int del = rec->enforce_quota();
    onvif.stop();
    rtsp.stop();
    http.stop();
    int total = 0;
    rec->list_segments(0, 0, 0, 1, 1, &total);
    LOG_INFO("停止 | 录像段=%d 配额删除=%d", total, del);
    return 0;
}
