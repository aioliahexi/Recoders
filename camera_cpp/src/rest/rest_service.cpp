#include "rest_service.h"

#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>

#include "../camera_manager.h"
#include "../device/device_discovery.h"
#include "../media/v4l2_controls.h"
#include "../onvif/onvif_service.h"
#include "../util/json.h"
#include "../util/log.h"

namespace fs = std::filesystem;
using camera::json::Value;

namespace camera {

namespace {

std::string ok_body(const Value& data) {
    Value r = Value::object();
    r["code"] = int64_t(0);
    r["message"] = "ok";
    r["data"] = data;
    return json::dump(r);
}

// URL 解码（查询参数：前端 encodeURIComponent 后为 %-编码）
std::string url_decode(const std::string& s) {
    std::string out;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { out += static_cast<char>((h << 4) | l); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

// 照片文件时间戳解析：epoch-ms 文件名（13 位数字）或 %Y%m%d_%H%M%S
uint64_t photo_ts_ms(const std::string& name) {
    if (name.size() >= 13 &&
        name.substr(0, 13).find_first_not_of("0123456789") == std::string::npos) {
        return std::strtoull(name.substr(0, 13).c_str(), nullptr, 10);
    }
    if (name.size() >= 15 && name[8] == '_') {
        std::tm tm{};
        std::istringstream ss(name.substr(0, 15));
        ss >> std::get_time(&tm, "%Y%m%d_%H%M%S");
        time_t t = timegm(&tm);
        if (t > 0) return static_cast<uint64_t>(t) * 1000ULL;
    }
    return 0;
}

std::string load_nvr_name(const Context* ctx) {
    return load_recorder_json(ctx->cfg.config_path).nvr_name;
}

// 相机名称清洗：去掉路径分隔符与非法字符，避免目录越权
std::string sanitize_name(std::string n) {
    std::string out;
    out.reserve(n.size());
    for (char ch : n) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|' || ch < 0x20)
            out += '_';
        else
            out += ch;
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    return out;
}

// 相机改名：迁移录像/照片目录并更新 DB 路径，然后持久化
void rename_camera(Context* ctx, int id, const std::string& old_name, const std::string& new_name) {
    if (new_name.empty() || new_name == old_name) return;
    std::error_code ec;
    const std::string rec_old = ctx->cfg.data_dir + "/recordings/" + old_name;
    const std::string rec_new = ctx->cfg.data_dir + "/recordings/" + new_name;
    if (fs::exists(rec_old) && !fs::exists(rec_new)) {
        fs::rename(rec_old, rec_new, ec);
        if (!ec && ctx->db) ctx->db->update_path_prefix(rec_old, rec_new);
    }
    const std::string ph_old = ctx->cfg.data_dir + "/photos/" + old_name;
    const std::string ph_new = ctx->cfg.data_dir + "/photos/" + new_name;
    if (fs::exists(ph_old) && !fs::exists(ph_new)) fs::rename(ph_old, ph_new, ec);
    // 持久化到统一 config.json（更新该相机记录的名称/设备）
    auto rj = load_recorder_json(ctx->cfg.config_path);
    bool found = false;
    for (auto& r : rj.cameras) {
        if ((!ctx->uuids[id].empty() && r.uuid == ctx->uuids[id]) || r.id == id) {
            r.name = new_name;
            r.id = id;
            if (!ctx->uuids[id].empty()) r.uuid = ctx->uuids[id];
            found = true;
            break;
        }
    }
    if (!found) {
        CameraCfg r;
        r.id = id;
        r.uuid = ctx->uuids.count(id) ? ctx->uuids[id] : "";
        r.name = new_name;
        r.device = ctx->params[id].device;
        rj.cameras.push_back(r);
    }
    rj.nvr_name = load_nvr_name(ctx);
    save_recorder_json(ctx->cfg.config_path, rj);
}

// 单调时钟毫秒（流循环节流用）
int64_t now_ms_steady() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 操作日志（系统页动作审计）
void log_op(Context* ctx, int camera_id, const std::string& action,
            const std::string& detail, int result) {
    log_operation(ctx, camera_id, action, detail, result);
}

// 流式发送（MSG_NOSIGNAL：客户端断开时不触发 SIGPIPE）
bool send_all(int fd, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return false;
        off += static_cast<size_t>(w);
    }
    return true;
}

bool send_all(int fd, const std::string& s) {
    return send_all(fd, s.data(), s.size());
}

std::string err_body(int code, const std::string& msg) {
    Value r = Value::object();
    r["code"] = int64_t(code);
    r["message"] = msg;
    return json::dump(r);
}

std::string now_iso() {
    auto t = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
    std::time_t sec = static_cast<std::time_t>(ms / 1000);
    std::tm tm{};
    gmtime_r(&sec, &tm);
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    std::string s = buf;
    s += "." + std::to_string(ms % 1000) + "Z";
    return s;
}

Pipeline* find_pipeline(Context* ctx, int id) {
    std::lock_guard<std::recursive_mutex> lk(ctx->cam_mu);
    auto it = ctx->pipelines.find(id);
    return it == ctx->pipelines.end() ? nullptr : it->second.get();
}

std::string camera_photo_dir(Context* ctx, int id) {
    return ctx->cfg.data_dir + "/photos/" + ctx->names[id];
}

Value camera_status_json(Context* ctx, int id) {
    std::lock_guard<std::recursive_mutex> lk(ctx->cam_mu);
    Pipeline* p = find_pipeline(ctx, id);
    Value v = Value::object();
    v["id"] = int64_t(id);
    v["name"] = ctx->names[id];
    v["device"] = ctx->params[id].device;
    // 在线：3 秒内有编码帧输出（拔线后自动转为未连接）
    uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    bool online = p && p->last_frame_ms() > 0 && (now - p->last_frame_ms()) < 3000;
    v["online"] = online;
    v["recording"] = online && p->running();
    v["encoder"] = p ? p->status() : "";
    v["resolution"] = std::to_string(ctx->params[id].width) + "x" +
                      std::to_string(ctx->params[id].height);
    v["fps"] = int64_t(ctx->params[id].fps);
    v["fps_actual"] = p ? p->measured_fps() : 0.0;
    v["bitrate_kbps"] = int64_t(ctx->params[id].bitrate_kbps);
    v["uuid"] = ctx->uuids.count(id) ? ctx->uuids[id] : "";
    return v;
}

}  // namespace

void RestService::setup(net::HttpServer& http) {
    http.route("GET", "/health", [](const net::Request&) {
        Value d = Value::object();
        d["status"] = "ok";
        d["time"] = now_iso();
        return net::Response{200, "application/json", ok_body(d)};
    });
    // MJPEG 实时预览流（长连接，浏览器 <img> 直接播放）
    http.route_stream("GET", R"(/api/v1/cameras/([0-9]+)/mjpeg)",
                      [this](int fd, const net::Request& r) { stream_mjpeg(fd, r); });
    http.route("GET", "/api/v1/cameras", [this](const net::Request& r) { return handle_cameras(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)", [this](const net::Request& r) { return handle_camera_detail(r); });
    http.route("POST", "/api/v1/cameras/([0-9]+)/photo", [this](const net::Request& r) { return handle_photo(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/photo/latest", [this](const net::Request& r) { return handle_photo_latest(r); });
    http.route("POST", "/api/v1/cameras/([0-9]+)/record/start", [this](const net::Request& r) { return handle_record_start(r); });
    http.route("POST", "/api/v1/cameras/([0-9]+)/record/stop", [this](const net::Request& r) { return handle_record_stop(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/recordings", [this](const net::Request& r) { return handle_recordings(r); });
    http.route("GET", "/api/v1/recordings/([0-9]+)/download", [this](const net::Request& r) { return handle_download(r); });
    http.route("DELETE", "/api/v1/recordings/([0-9]+)", [this](const net::Request& r) { return handle_delete(r); });
    http.route("GET", "/api/v1/system/storage", [this](const net::Request& r) { return handle_storage(r); });
    http.route("GET", "/api/v1/system/stats", [this](const net::Request& r) { return handle_stats(r); });
    http.route("GET", "/api/v1/system/onvif", [this](const net::Request& r) { return handle_onvif_get(r); });
    http.route("PUT", "/api/v1/system/onvif", [this](const net::Request& r) { return handle_onvif_put(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/photos", [this](const net::Request& r) { return handle_photos(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/photos/tags", [this](const net::Request& r) { return handle_photo_tags(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/photos/([^/]+\\.jpg)", [this](const net::Request& r) { return handle_photo_file(r); });
    http.route("DELETE", "/api/v1/cameras/([0-9]+)/photos/([^/]+\\.jpg)", [this](const net::Request& r) { return handle_photo_delete(r); });
    http.route("GET", "/api/v1/system/settings", [this](const net::Request& r) { return handle_settings_get(r); });
    http.route("PUT", "/api/v1/system/settings", [this](const net::Request& r) { return handle_settings_put(r); });
    http.route("GET", "/api/v1/system/logs", [this](const net::Request& r) { return handle_logs(r); });
    http.route("GET", "/api/v1/system/discover", [this](const net::Request& r) { return handle_discover(r); });
    http.route("POST", "/api/v1/cameras", [this](const net::Request& r) { return handle_camera_add(r); });
    http.route("DELETE", "/api/v1/cameras/([0-9]+)", [this](const net::Request& r) { return handle_camera_delete(r); });
    http.route("POST", "/api/v1/system/clear", [this](const net::Request& r) { return handle_clear(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/config", [this](const net::Request& r) { return handle_config_get(r); });
    http.route("PUT", "/api/v1/cameras/([0-9]+)/config", [this](const net::Request& r) { return handle_config_put(r); });
    http.route("GET", "/api/v1/cameras/([0-9]+)/controls", [this](const net::Request& r) { return handle_controls_get(r); });
    http.route("PUT", "/api/v1/cameras/([0-9]+)/controls", [this](const net::Request& r) { return handle_controls_put(r); });
}

net::Response RestService::handle_cameras(const net::Request&) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    Value arr = Value::array();
    for (const auto& [id, _] : ctx_->pipelines) {
        arr.push_back(camera_status_json(ctx_, id));
    }
    Value d = Value::object();
    d["items"] = arr;
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_camera_detail(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+))");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) {
        return {404, "application/json", err_body(40001, "相机不存在")};
    }
    return {200, "application/json", ok_body(camera_status_json(ctx_, id))};
}

net::Response RestService::handle_photo(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/photo)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    Pipeline* p = find_pipeline(ctx_, id);
    if (!p) return {404, "application/json", err_body(40001, "相机不存在")};
    int quality = 90;
    std::string tag, source;
    if (!req.body.empty()) {
        try {
            Value body = json::parse(req.body);
            quality = static_cast<int>(body.get("quality").as_int(90));
            tag = sanitize_name(body.get("tag").as_string(""));    // 事件标签（存 SQLite）
            source = sanitize_name(body.get("source").as_string(""));  // manual/event
        } catch (...) { /* 忽略坏 body，用默认 */ }
    }
    std::string dir = camera_photo_dir(ctx_, id);
    std::string out_name;
    if (!p->snapshot(dir, quality, &out_name)) {
        return {500, "application/json", err_body(50002, "抓拍失败")};
    }
    if (source.empty()) source = "manual";
    if (tag.empty()) tag = "手动抓拍";
    if (!out_name.empty() && ctx_->db)
        ctx_->db->add_photo(id, out_name, photo_ts_ms(out_name), tag, source);
    log_op(ctx_, id, "photo_take", "抓拍照片 标签=" + tag + " 来源=" + source, 1);
    Value d = Value::object();
    d["camera_id"] = int64_t(id);
    d["name"] = out_name;
    d["path"] = dir + "/latest.jpg";
    d["url"] = "/api/v1/cameras/" + std::to_string(id) + "/photo/latest";
    d["tag"] = tag;
    d["source"] = source;
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_photo_latest(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/photo/latest)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    std::string path = camera_photo_dir(ctx_, id) + "/latest.jpg";
    if (!fs::exists(path)) {
        return {404, "application/json", err_body(40401, "还没有照片")};
    }
    net::Response resp;
    resp.file_path = path;
    resp.content_type = "image/jpeg";
    return resp;
}

net::Response RestService::handle_record_start(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/record/start)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    Pipeline* p = find_pipeline(ctx_, id);
    if (!p) return {404, "application/json", err_body(40001, "相机不存在")};
    if (p->running()) return {409, "application/json", err_body(40002, "相机正在录像")};
    if (!p->start()) return {500, "application/json", err_body(50002, "启动录像失败")};
    log_op(ctx_, id, "record_start", "开始录像", 1);
    Value d = Value::object();
    d["camera_id"] = int64_t(id);
    d["start_time"] = now_iso();
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_record_stop(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/record/stop)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    Pipeline* p = find_pipeline(ctx_, id);
    if (!p) return {404, "application/json", err_body(40001, "相机不存在")};
    if (!p->running()) return {409, "application/json", err_body(40002, "相机未在录像")};
    p->stop();
    ctx_->recorder->stop_camera(id);
    ctx_->recorder->rescan();
    log_op(ctx_, id, "record_stop", "停止录像", 1);
    Value d = Value::object();
    d["camera_id"] = int64_t(id);
    d["stopped"] = true;
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_recordings(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/recordings)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    uint64_t start = 0, end = 0;
    int page = 1, page_size = 20;
    auto q = [&](const std::string& key) -> std::string {
        size_t p = req.query.find(key + "=");
        if (p == std::string::npos) return "";
        size_t v = p + key.size() + 1;
        size_t e = req.query.find('&', v);
        return req.query.substr(v, e == std::string::npos ? std::string::npos : e - v);
    };
    if (!q("start").empty()) start = std::strtoull(q("start").c_str(), nullptr, 10);
    if (!q("end").empty()) end = std::strtoull(q("end").c_str(), nullptr, 10);
    if (!q("page").empty()) page = std::atoi(q("page").c_str());
    if (!q("page_size").empty()) page_size = std::atoi(q("page_size").c_str());
    int total = 0;
    auto segs = ctx_->recorder->list_segments(id, start, end, page, page_size, &total);
    Value items = Value::array();
    for (auto& s : segs) {
        Value it = Value::object();
        it["id"] = int64_t(s.id);
        it["camera_id"] = int64_t(s.camera_id);
        it["start_ms"] = static_cast<int64_t>(s.start_ms);
        it["end_ms"] = static_cast<int64_t>(s.end_ms);
        it["duration_s"] = static_cast<double>((s.end_ms - s.start_ms) / 1000.0);
        it["size_bytes"] = static_cast<int64_t>(s.size_bytes);
        it["url"] = "/api/v1/recordings/" + std::to_string(s.id) + "/download";
        items.push_back(it);
    }
    Value d = Value::object();
    d["total"] = int64_t(total);
    d["page"] = int64_t(page);
    d["page_size"] = int64_t(page_size);
    d["items"] = items;
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_download(const net::Request& req) {
    std::smatch m;
    std::regex re(R"(/api/v1/recordings/([0-9]+)/download)");
    std::regex_search(req.path, m, re);
    int64_t rid = std::atoll(m[1].str().c_str());
    auto seg = ctx_->db->get_by_id(rid);
    bool exists = seg && fs::exists(seg->path);
    LOG_INFO("[rest] download rid=%lld seg=%d path=%s exists=%d", (long long)rid,
             seg ? 1 : 0, seg ? seg->path.c_str() : "-", exists ? 1 : 0);
    if (!seg || !exists) {
        return {404, "application/json", err_body(40401, "录像不存在或已被覆盖")};
    }
    net::Response resp;
    resp.file_path = seg->path;
    resp.content_type = "video/mp4";
    return resp;
}

net::Response RestService::handle_delete(const net::Request& req) {
    std::smatch m;
    std::regex re(R"(/api/v1/recordings/([0-9]+))");
    std::regex_search(req.path, m, re);
    int64_t rid = std::atoll(m[1].str().c_str());
    auto seg = ctx_->db->get_by_id(rid);
    if (!seg) return {404, "application/json", err_body(40401, "录像不存在")};
    if (fs::exists(seg->path)) fs::remove(seg->path);
    ctx_->db->remove_by_id(rid);
    log_op(ctx_, seg->camera_id, "recording_delete",
           "删除录像 #" + std::to_string(rid) + " " + seg->path, 1);
    Value d = Value::object();
    d["deleted"] = int64_t(rid);
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_storage(const net::Request&) {
    std::error_code ec;
    fs::create_directories(ctx_->cfg.data_dir, ec);
    struct statvfs st {};
    uint64_t total = 0, free = 0;
    if (::statvfs(ctx_->cfg.data_dir.c_str(), &st) == 0) {
        total = static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
        free = static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
    }
    int cnt = 0;
    uint64_t bytes = 0;
    ctx_->recorder->stats(&cnt, &bytes);
    Value d = Value::object();
    d["mount"] = ctx_->cfg.data_dir;
    d["total_bytes"] = static_cast<int64_t>(total);
    d["free_bytes"] = static_cast<int64_t>(free);
    d["used_percent"] = total ? static_cast<double>(static_cast<double>(total - free) / total * 100.0) : 0.0;
    d["recordings_count"] = int64_t(cnt);
    d["recordings_bytes"] = static_cast<int64_t>(bytes);
    d["quota_bytes"] = static_cast<int64_t>(ctx_->cfg.quota_mb * 1024 * 1024);
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_stats(const net::Request&) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    Value arr = Value::array();
    for (const auto& [id, _] : ctx_->pipelines) {
        arr.push_back(camera_status_json(ctx_, id));
    }
    Value d = Value::object();
    d["cameras"] = arr;
    d["uptime_ms"] = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() -
        static_cast<int64_t>(ctx_->started_ms));
    return {200, "application/json", ok_body(d)};
}


// ---------- 照片列表/浏览 ----------
net::Response RestService::handle_photos(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/photos)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    int page = 1, page_size = 20;
    auto q = [&](const std::string& k) -> std::string {
        size_t p = req.query.find(k + "=");
        if (p == std::string::npos) return "";
        size_t v = p + k.size() + 1, e = req.query.find('&', v);
        return req.query.substr(v, e == std::string::npos ? std::string::npos : e - v);
    };
    if (!q("page").empty()) page = std::atoi(q("page").c_str());
    if (!q("page_size").empty()) page_size = std::atoi(q("page_size").c_str());
    const std::string tagf = url_decode(q("tag"));   // 按事件标签筛选（URL 解码）
    // 磁盘<->索引同步：外部删除（NFS/清理）后清孤儿行，新文件回填（INSERT OR IGNORE 幂等）
    {
        std::string dir = camera_photo_dir(ctx_, id);
        std::set<std::string> ondisk;
        if (fs::is_directory(dir)) {
            for (const auto& e : fs::directory_iterator(dir)) {
                if (e.path().extension() != ".jpg" || e.path().filename() == "latest.jpg") continue;
                std::string nm = e.path().filename().string();
                ondisk.insert(nm);
                ctx_->db->add_photo(id, nm, photo_ts_ms(nm), "手动抓拍", "manual");
            }
        }
        int t0 = 0;
        auto all = ctx_->db->list_photos(id, "", 1, 500, &t0);
        for (auto& r : all)
            if (!ondisk.count(r.name)) ctx_->db->delete_photo_by_name(id, r.name);
    }
    int total = 0;
    auto rows = ctx_->db->list_photos(id, tagf, page, page_size, &total);
    Value items = Value::array();
    for (const auto& r : rows) {
        Value it = Value::object();
        it["name"] = r.name;
        it["url"] = "/api/v1/cameras/" + std::to_string(id) + "/photos/" + r.name;
        it["ts_ms"] = static_cast<int64_t>(r.ts_ms);
        it["tag"] = r.tag;
        it["source"] = r.source;
        items.push_back(it);
    }
    Value d = Value::object();
    d["total"] = int64_t(total);
    d["page"] = int64_t(page);
    d["page_size"] = int64_t(page_size);
    d["items"] = items;
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_photo_file(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/photos/([^/]+\.jpg))");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    std::string path = camera_photo_dir(ctx_, id) + "/" + m[2].str();
    if (!fs::exists(path)) return {404, "application/json", err_body(40401, "照片不存在")};
    net::Response resp;
    resp.file_path = path;
    resp.content_type = "image/jpeg";
    return resp;
}

// ---------- 相机参数配置（热重配） ----------
static bool reconfigure_camera(Context* ctx, int id, const json::Value& body) {
    if (!ctx->pipelines.count(id)) return false;
    PipelineParams p = ctx->params[id];
    std::string& name = ctx->names[id];
    if (body.contains("name")) {
        std::string nn = sanitize_name(body.get("name").as_string(name));
        if (nn.empty()) nn = name;
        p.name = nn;   // OSD 等使用的新名
        if (nn != name) {
            const std::string old_n = name;
            name = nn;   // 先更新内存名，rename_camera 持久化时才写对新名
            // 停流并封当前段，再迁移目录/DB，最后持久化
            Pipeline* p1 = ctx->pipelines[id].get();
            bool was = p1->running();
            p1->stop();
            ctx->recorder->stop_camera(id);
            rename_camera(ctx, id, old_n, nn);
            log_op(ctx, id, "camera_rename", "相机改名 " + old_n + " -> " + nn, 1);
            if (!p1->open(p)) return false;
            ctx->params[id] = p;
            ctx->recorder->start_camera(id, name, p1->encoder_params());
            if (was) p1->start();
            return true;
        }
    }
    if (body.contains("width")) p.width = static_cast<int>(body.get("width").as_int(p.width));
    if (body.contains("height")) p.height = static_cast<int>(body.get("height").as_int(p.height));
    if (body.contains("fps")) p.fps = static_cast<int>(body.get("fps").as_int(p.fps));
    if (body.contains("bitrate_kbps")) p.bitrate_kbps = static_cast<int>(body.get("bitrate_kbps").as_int(p.bitrate_kbps));
    if (body.contains("gop")) p.gop = static_cast<int>(body.get("gop").as_int(p.gop));
    if (body.contains("input_format")) p.input_format = body.get("input_format").as_string(p.input_format);
    if (p.width < 64 || p.height < 64 || p.fps < 1 || p.fps > 120 || p.bitrate_kbps < 100) return false;
    Pipeline* pipe = ctx->pipelines[id].get();
    bool was = pipe->running();
    pipe->stop();
    if (!pipe->open(p)) return false;
    ctx->params[id] = p;
    log_op(ctx, id, "camera_config",
           "参数更新 " + std::to_string(p.width) + "x" + std::to_string(p.height) +
           "@" + std::to_string(p.fps) + " " + std::to_string(p.bitrate_kbps) + "kbps", 1);
    ctx->recorder->start_camera(id, name, pipe->encoder_params());
    if (was) pipe->start();
    return true;
}

net::Response RestService::handle_config_get(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/config)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    const PipelineParams& p = ctx_->params[id];
    Value d = Value::object();
    d["name"] = ctx_->names[id];
    d["device"] = p.device;
    d["width"] = int64_t(p.width);
    d["height"] = int64_t(p.height);
    d["fps"] = int64_t(p.fps);
    d["bitrate_kbps"] = int64_t(p.bitrate_kbps);
    d["gop"] = int64_t(p.gop);
    d["input_format"] = p.input_format;
    d["encoder"] = find_pipeline(ctx_, id)->status();
    d["uuid"] = ctx_->uuids.count(id) ? ctx_->uuids[id] : "";
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_config_put(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/config)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    try {
        Value body = json::parse(req.body);
        if (!reconfigure_camera(ctx_, id, body)) {
            return {400, "application/json", err_body(40003, "参数无效")};
        }
        return {200, "application/json", ok_body(json::parse(handle_config_get(req).body))};
    } catch (...) {
        return {400, "application/json", err_body(40003, "JSON 解析失败")};
    }
}

// ---------- V4L2 控件 ----------
net::Response RestService::handle_controls_get(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/controls)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    Value items = Value::array();
    for (auto& c : v4l2_list_controls(ctx_->params[id].device)) {
        Value v = Value::object();
        v["id"] = static_cast<int64_t>(c.id);
        v["name"] = c.name;
        v["min"] = static_cast<int64_t>(c.min);
        v["max"] = static_cast<int64_t>(c.max);
        v["step"] = static_cast<int64_t>(c.step);
        v["default"] = static_cast<int64_t>(c.def);
        v["value"] = static_cast<int64_t>(c.value);
        v["menu"] = c.menu;
        items.push_back(v);
    }
    Value d = Value::object();
    d["items"] = items;
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_controls_put(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/controls)");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    try {
        Value body = json::parse(req.body);
        uint32_t cid = static_cast<uint32_t>(body.get("id").as_int(0));
        int64_t val = body.get("value").as_int(0);
        bool ok = v4l2_set_control(ctx_->params[id].device, cid, val);
        log_op(ctx_, id, "control_set", "设置控件 id=" + std::to_string(cid) +
               " 值=" + std::to_string(val), ok ? 1 : 0);
        Value d = Value::object();
        d["ok"] = ok;
        return {200, "application/json", ok_body(d)};
    } catch (...) {
        return {400, "application/json", err_body(40003, "JSON 解析失败")};
    }
}

// ---------- ONVIF 状态/配置 ----------
net::Response RestService::handle_onvif_get(const net::Request&) {
    Value d = Value::object();
    d["enabled"] = ctx_->onvif ? ctx_->onvif->enabled() : false;
    d["discovery"] = ctx_->onvif ? ctx_->onvif->discovery_enabled() : false;
    d["xaddr"] = "http://" + ctx_->cfg.host_ip + ":" + std::to_string(ctx_->cfg.http_port) + "/onvif/device_service";
    d["rtsp_port"] = int64_t(ctx_->cfg.rtsp_port);
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_onvif_put(const net::Request& req) {
    try {
        Value body = json::parse(req.body);
        if (ctx_->onvif) {
            if (body.contains("discovery")) {
                bool on = body.get("discovery").as_bool(ctx_->onvif_discovery);
                ctx_->onvif_discovery = on;
                ctx_->onvif->set_discovery(on);
            }
            if (body.contains("enabled")) {
                bool on = body.get("enabled").as_bool(ctx_->onvif_enabled);
                ctx_->onvif_enabled = on;
                ctx_->onvif->set_enabled(on);
            }
        }
        log_op(ctx_, 0, "onvif_config", "ONVIF 配置更新", 1);
        return {200, "application/json", ok_body(Value::object())};
    } catch (...) {
        return {400, "application/json", err_body(40003, "JSON 解析失败")};
    }
}


// ---------- 照片删除 ----------
net::Response RestService::handle_photo_delete(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/photos/([^/]+\.jpg))");
    if (!std::regex_search(req.path, m, re))
        return {404, "application/json", err_body(40401, "照片不存在")};
    int id = std::atoi(m[1].str().c_str());
    std::string name = m[2].str();
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    std::string path = camera_photo_dir(ctx_, id) + "/" + name;
    if (!fs::exists(path)) return {404, "application/json", err_body(40401, "照片不存在")};
    fs::remove(path);
    ctx_->db->delete_photo_by_name(id, name);
    log_op(ctx_, id, "photo_delete", "删除照片 " + name, 1);
    Value d = Value::object();
    d["deleted"] = name;
    return {200, "application/json", ok_body(d)};
}

// ---------- 照片事件标签列表（供浏览页筛选） ----------
net::Response RestService::handle_photo_tags(const net::Request& req) {
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/photos/tags)");
    if (!std::regex_search(req.path, m, re))
        return {404, "application/json", err_body(40401, "Not Found")};
    int id = std::atoi(m[1].str().c_str());
    if (!find_pipeline(ctx_, id)) return {404, "application/json", err_body(40001, "相机不存在")};
    auto tags = ctx_->db->list_photo_tags(id);
    Value arr = Value::array();
    for (const auto& t : tags) arr.push_back(t);
    Value d = Value::object();
    d["tags"] = arr;
    return {200, "application/json", ok_body(d)};
}

// ---------- 系统设置（NVR 名称等，持久化到统一 config.json） ----------
net::Response RestService::handle_settings_get(const net::Request&) {
    Value d = Value::object();
    d["nvr_name"] = load_nvr_name(ctx_);
    d["rest_public"] = ctx_->rest_public.load();
    return {200, "application/json", ok_body(d)};
}

net::Response RestService::handle_settings_put(const net::Request& req) {
    try {
        Value body = json::parse(req.body);
        std::string name = body.get("nvr_name").as_string("camera_server NVR");
        if (name.empty()) name = "camera_server NVR";
        auto rj = load_recorder_json(ctx_->cfg.config_path);
        rj.nvr_name = name;
        if (body.contains("rest_public")) {
            bool rp = body.get("rest_public").as_bool(ctx_->rest_public.load());
            ctx_->rest_public = rp;
            rj.rest_public = rp;
        }
        save_recorder_json(ctx_->cfg.config_path, rj);
        log_op(ctx_, 0, "settings_update",
               "NVR 名称=" + name + (body.contains("rest_public") ?
               (std::string)(" 免鉴权REST=" + std::string(ctx_->rest_public.load() ? "开" : "关")) : ""), 1);
        Value d = Value::object();
        d["nvr_name"] = name;
        d["rest_public"] = ctx_->rest_public.load();
        return {200, "application/json", ok_body(d)};
    } catch (...) {
        return {400, "application/json", err_body(40003, "JSON 解析失败")};
    }
}

// ---------- 清空数据（录像 / 全部含照片），二次确认由前端负责 ----------
net::Response RestService::handle_clear(const net::Request& req) {
    std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
    std::string scope = "recordings";
    try {
        Value body = json::parse(req.body);
        scope = body.get("scope").as_string("recordings");
    } catch (...) {}
    // 严格校验，仅允许明确范围，防止误删
    if (scope != "recordings" && scope != "all")
        return {400, "application/json", err_body(40003, "scope 需为 recordings 或 all")};
    // 1) 停止所有相机录像（封段入库）
    for (auto& [id, p] : ctx_->pipelines) {
        if (p->running()) p->stop();
        ctx_->recorder->stop_camera(id);
    }
    // 2) 删除录像文件与索引
    int recs = ctx_->recorder->clear_recordings();
    // 3) 可选：删除照片
    int phos = 0;
    if (scope == "all") {
        for (auto& [id, name] : ctx_->names) {
            std::string dir = camera_photo_dir(ctx_, id);
            std::error_code ec;
            if (fs::is_directory(dir)) {
                phos += static_cast<int>(std::distance(fs::directory_iterator(dir),
                                                       fs::directory_iterator()));
                fs::remove_all(dir, ec);
            }
        }
    }
    // 4) 恢复录像
    for (auto& [id, p] : ctx_->pipelines) {
        ctx_->recorder->start_camera(id, ctx_->names[id], p->encoder_params());
        p->start();
    }
    log_op(ctx_, 0, "data_clear",
           "清空数据 scope=" + scope + " 录像" + std::to_string(recs) +
           "段 照片" + std::to_string(phos) + "张", 1);
    Value d = Value::object();
    d["scope"] = scope;
    d["deleted_recordings"] = int64_t(recs);
    d["deleted_photos"] = int64_t(phos);
    return {200, "application/json", ok_body(d)};
}

// ---------- 操作日志查询（DB + Node 服务动作日志合并） ----------
net::Response RestService::handle_logs(const net::Request& req) {
    int page = 1, page_size = 50;
    auto q = [&](const std::string& k) -> std::string {
        size_t p = req.query.find(k + "=");
        if (p == std::string::npos) return "";
        size_t v = p + k.size() + 1, e = req.query.find('&', v);
        return req.query.substr(v, e == std::string::npos ? std::string::npos : e - v);
    };
    if (!q("page").empty()) page = std::atoi(q("page").c_str());
    if (!q("page_size").empty()) page_size = std::atoi(q("page_size").c_str());
    if (page_size < 1 || page_size > 200) page_size = 50;
    struct Item { uint64_t ts; int camera_id; std::string action, detail; int result; };
    int db_total = 0;
    auto rows = ctx_->db->list_ops(1, 500, &db_total);
    std::vector<Item> all;
    for (auto& r : rows)
        all.push_back({r.ts_ms, r.camera_id, r.action, r.detail, r.result});
    // Node 侧服务动作日志（jsonl）
    int node_total = 0;
    std::ifstream f(ctx_->cfg.data_dir + "/oplog_node.jsonl");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try {
            auto v = json::parse(line);
            Item it;
            it.ts = static_cast<uint64_t>(v.get("ts").as_int(0));
            it.camera_id = 0;
            it.action = v.get("action").as_string("");
            it.detail = v.get("detail").as_string("");
            it.result = v.get("result").as_int(1);
            all.push_back(it);
            node_total++;
        } catch (...) {}
    }
    std::sort(all.begin(), all.end(),
              [](const Item& a, const Item& b) { return a.ts > b.ts; });
    int total = db_total + node_total;
    Value items = Value::array();
    for (size_t i = static_cast<size_t>((page - 1) * page_size);
         i < all.size() && items.size() < static_cast<size_t>(page_size); i++) {
        Value it = Value::object();
        it["ts_ms"] = static_cast<int64_t>(all[i].ts);
        it["camera_id"] = int64_t(all[i].camera_id);
        it["action"] = all[i].action;
        it["detail"] = all[i].detail;
        it["result"] = int64_t(all[i].result);
        items.push_back(it);
    }
    Value d = Value::object();
    d["total"] = int64_t(total);
    d["page"] = int64_t(page);
    d["page_size"] = int64_t(page_size);
    d["items"] = items;
    return {200, "application/json", ok_body(d)};
}

// ---------- 设备发现（供"添加相机"选择/刷新） ----------
net::Response RestService::handle_discover(const net::Request&) {
    std::set<std::string> known;
    {
        std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
        for (const auto& [id, u] : ctx_->uuids) if (!u.empty()) known.insert(u);
    }
    Value arr = Value::array();
    for (auto& d : discover_uvc_cameras(true)) {
        if (!d.is_uvc || d.uuid.empty()) continue;
        if (known.count(d.uuid)) continue;   // 已注册相机跳过
        Value it = Value::object();
        it["device"] = d.v4l2_device;
        it["uuid"] = d.uuid;
        it["product"] = d.product.empty() ? d.manufacturer : d.product;
        it["manufacturer"] = d.manufacturer;
        it["usb"] = d.usb_path;
        // 推荐格式：最大分辨率@最高帧率
        int bw = 0, bh = 0, bf = 0;
        std::string fmt = "mjpeg";
        for (auto& f : d.formats) {
            if (f.width < 640 || f.fps < 15) continue;
            if (f.width * f.height > bw * bh) {
                bw = f.width; bh = f.height; bf = f.fps;
                fmt = (f.fourcc == 0x34363248u) ? "h264" : "mjpeg";
            }
        }
        if (bw == 0) { bw = 1920; bh = 1080; bf = 30; }
        it["width"] = int64_t(bw);
        it["height"] = int64_t(bh);
        it["fps"] = int64_t(bf);
        it["input_format"] = fmt;
        arr.push_back(it);
    }
    Value d = Value::object();
    d["items"] = arr;
    return {200, "application/json", ok_body(d)};
}

// ---------- 添加相机（手动注册，等价热插拔注册） ----------
net::Response RestService::handle_camera_add(const net::Request& req) {
    try {
        Value body = json::parse(req.body);
        std::string device = body.get("device").as_string("");
        if (device.empty()) return {400, "application/json", err_body(40003, "device 必填")};
        // 设备是否已被占用
        {
            std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
            for (const auto& [id, p] : ctx_->params)
                if (p.device == device)
                    return {409, "application/json", err_body(40002, "该设备已被相机使用")};
        }
        CameraSpec sp;
        sp.id = 0;
        sp.name = body.get("name").as_string("");
        sp.device = device;
        sp.width = static_cast<int>(body.get("width").as_int(1920));
        sp.height = static_cast<int>(body.get("height").as_int(1080));
        sp.fps = static_cast<int>(body.get("fps").as_int(30));
        sp.bitrate_kbps = static_cast<int>(body.get("bitrate_kbps").as_int(3000));
        sp.gop = static_cast<int>(body.get("gop").as_int(60));
        sp.input_format = body.get("input_format").as_string("mjpeg");
        sp.capture = body.get("capture").as_string("gst");
        sp.osd = body.get("osd").as_bool(true);
        if (sp.name.empty()) sp.name = "CAM" + std::to_string(next_camera_id(*ctx_));
        std::string err;
        if (!launch_camera(*ctx_, ctx_->recorder, sp, ctx_->cfg.autostart, &err))
            return {500, "application/json", err_body(50002, "添加失败: " + err)};
        log_op(ctx_, sp.id, "camera_add", "手动添加相机 " + sp.name + " " + device, 1);
        return {200, "application/json", ok_body(camera_status_json(ctx_, sp.id))};
    } catch (...) {
        return {400, "application/json", err_body(40003, "JSON 解析失败")};
    }
}

// ---------- 删除相机（停止录像，保留数据目录） ----------
net::Response RestService::handle_camera_delete(const net::Request& req) {
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+))");
    std::regex_search(req.path, m, re);
    int id = std::atoi(m[1].str().c_str());
    {
        std::lock_guard<std::recursive_mutex> lk(ctx_->cam_mu);
        if (!ctx_->pipelines.count(id))
            return {404, "application/json", err_body(40401, "相机不存在")};
    }
    std::string err;
    if (!remove_camera(*ctx_, ctx_->recorder, id, &err))
        return {500, "application/json", err_body(50002, "删除失败: " + err)};
    log_op(ctx_, id, "camera_delete", "删除相机 id=" + std::to_string(id), 1);
    Value d = Value::object();
    d["deleted"] = int64_t(id);
    return {200, "application/json", ok_body(d)};
}

void RestService::stream_mjpeg(int fd, const net::Request& req) {
    std::smatch m;
    std::regex re(R"(/api/v1/cameras/([0-9]+)/mjpeg)");
    if (!std::regex_search(req.path, m, re)) {
        const std::string body = R"({"code":40401,"message":"Not Found"})";
        send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body);
        ::close(fd);
        return;
    }
    const int id = std::atoi(m[1].str().c_str());
    Pipeline* p = find_pipeline(ctx_, id);
    if (!p) {
        const std::string body = R"({"code":40001,"message":"相机不存在"})";
        send_all(fd, "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\n\r\n" + body);
        ::close(fd);
        return;
    }
    // 等待首帧（gst 管道首关键帧后才出帧）
    const int64_t t0 = now_ms_steady();
    while (ctx_->stopping.load() == false && !p->running()) {
        if (now_ms_steady() - t0 > 10000) break;
        ::usleep(200 * 1000);
    }
    const std::string head = "HTTP/1.1 200 OK\r\n"
                             "Server: camera_server\r\n"
                             "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                             "Cache-Control: no-cache, no-store\r\n"
                             "Access-Control-Allow-Origin: *\r\n"
                             "Connection: close\r\n\r\n";
    if (!send_all(fd, head)) { ::close(fd); return; }

    const int64_t interval_us = 66000;  // ~15fps 预览帧率
    int64_t last = now_ms_steady();
    std::vector<uint8_t> jpg;
    while (ctx_->stopping.load() == false && p->running()) {
        const int64_t now = now_ms_steady();
        const int64_t wait_us = interval_us - (now - last) * 1000;
        if (wait_us > 0) ::usleep(static_cast<useconds_t>(wait_us));
        last = now_ms_steady();
        jpg.clear();
        if (!p->preview_jpeg(jpg, 640)) {
            ::usleep(20000);  // 暂无新帧，稍候
            continue;
        }
        std::string part = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                           std::to_string(jpg.size()) + "\r\n\r\n";
        if (!send_all(fd, part)) break;
        if (!send_all(fd, jpg.data(), jpg.size())) break;
        if (!send_all(fd, "\r\n")) break;
    }
    ::close(fd);
}

}  // namespace camera
