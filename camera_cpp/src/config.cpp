// 统一 JSON 配置实现：config.json（系统 + 用户）+ 旧 ini/recorder.json 自动迁移
#include "config.h"

#include <cstdio>
#include <set>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "util/json.h"
#include "util/log.h"

namespace fs = std::filesystem;

namespace camera {

namespace {

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_bool(const std::string& v) {
    return v == "true" || v == "1" || v == "yes";
}

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file_atomic(const std::string& path, const std::string& data) {
    std::error_code ec;
    const auto slash = path.find_last_of('/');
    if (slash != std::string::npos)
        std::filesystem::create_directories(path.substr(0, slash), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return false;
        f << data;
        f.flush();
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

// ---------- 相机 <-> JSON ----------
json::Value camera_to_json(const CameraCfg& c) {
    auto e = json::Value::object();
    e["id"] = static_cast<int64_t>(c.id);
    e["uuid"] = c.uuid;
    e["name"] = c.name;
    e["device"] = c.device;
    e["mock"] = c.mock;
    e["width"] = static_cast<int64_t>(c.width);
    e["height"] = static_cast<int64_t>(c.height);
    e["fps"] = static_cast<int64_t>(c.fps);
    e["input_format"] = c.input_format;
    e["bitrate_kbps"] = static_cast<int64_t>(c.bitrate_kbps);
    e["gop"] = static_cast<int64_t>(c.gop);
    e["capture"] = c.capture;
    e["osd"] = c.osd;
    e["enabled"] = c.enabled;
    return e;
}

void camera_from_json(const json::Value& e, CameraCfg* c) {
    c->id = static_cast<int>(e.get("id").as_int(0));
    c->uuid = e.get("uuid").as_string("");
    c->name = e.get("name").as_string(c->name);
    c->device = e.get("device").as_string(c->device);
    c->mock = e.get("mock").as_bool(false);
    c->width = static_cast<int>(e.get("width").as_int(c->width));
    c->height = static_cast<int>(e.get("height").as_int(c->height));
    c->fps = static_cast<int>(e.get("fps").as_int(c->fps));
    c->input_format = e.get("input_format").as_string(c->input_format);
    c->bitrate_kbps = static_cast<int>(e.get("bitrate_kbps").as_int(c->bitrate_kbps));
    c->gop = static_cast<int>(e.get("gop").as_int(c->gop));
    c->capture = e.get("capture").as_string(c->capture);
    c->osd = e.get("osd").as_bool(c->osd);
    c->enabled = e.get("enabled").as_bool(c->enabled);
}

// 为无 id 的相机从 1 开始顺序分配（跳过已占用 id），并按 id/uuid 去重
void assign_camera_ids(std::vector<CameraCfg>* cams) {
    std::set<int> used;
    for (auto& c : *cams)
        if (c.id > 0) used.insert(c.id);
    int next = 1;
    for (auto& c : *cams) {
        if (c.id > 0) continue;
        while (used.count(next)) next++;
        c.id = next++;
    }
    // 按 id 去重（保留第一个；合并同 uuid 的重复项）
    std::vector<CameraCfg> out;
    for (auto& c : *cams) {
        bool dup = false;
        for (auto& o : out)
            if (o.id == c.id || (!c.uuid.empty() && o.uuid == c.uuid)) { dup = true; break; }
        if (!dup) out.push_back(c);
    }
    *cams = std::move(out);
}

// ---------- 旧 ini 解析（仅用于迁移） ----------
AppCfg parse_ini(const std::string& path) {
    AppCfg cfg;
    std::ifstream in(path);
    CameraCfg cur;
    bool in_camera = false;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line == "[camera]") {
            if (in_camera) cfg.cameras.push_back(cur);
            cur = CameraCfg{};
            in_camera = true;
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq));
        std::string v = trim(line.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (!in_camera) {
            if (k == "data_dir") cfg.data_dir = v;
            else if (k == "quota_mb") cfg.quota_mb = std::strtoull(v.c_str(), nullptr, 10);
            else if (k == "segment_time_s") cfg.segment_time_s = std::atoi(v.c_str());
            else if (k == "encoder") cfg.encoder = v;
            else if (k == "http_port") cfg.http_port = std::atoi(v.c_str());
            else if (k == "rtsp_port") cfg.rtsp_port = std::atoi(v.c_str());
            else if (k == "api_key") cfg.api_key = v;
            else if (k == "host_ip") cfg.host_ip = v;
            else if (k == "autostart") cfg.autostart = parse_bool(v);
            else if (k == "onvif_enabled") cfg.onvif_enabled = parse_bool(v);
            else if (k == "onvif_discovery") cfg.onvif_discovery = parse_bool(v);
        } else {
            if (k == "name") cur.name = v;
            else if (k == "device") cur.device = v;
            else if (k == "mock") cur.mock = parse_bool(v);
            else if (k == "input_format") cur.input_format = v;
            else if (k == "width") cur.width = std::atoi(v.c_str());
            else if (k == "height") cur.height = std::atoi(v.c_str());
            else if (k == "fps") cur.fps = std::atoi(v.c_str());
            else if (k == "bitrate_kbps") cur.bitrate_kbps = std::atoi(v.c_str());
            else if (k == "gop") cur.gop = std::atoi(v.c_str());
            else if (k == "enabled") cur.enabled = parse_bool(v);
            else if (k == "osd") cur.osd = parse_bool(v);
            else if (k == "capture") cur.capture = v;
        }
    }
    if (in_camera) cfg.cameras.push_back(cur);
    return cfg;
}

// ---------- 旧 data_dir 运行时注册表（recorder.json + 更早 3 个 json） ----------
// 仅迁移用；正常运行时配置统一在 config.json
RecorderJson load_legacy_recorder_json(const std::string& data_dir) {
    RecorderJson out;
    const std::string path = data_dir + "/recorder.json";
    std::string txt = read_file(path);
    if (!txt.empty()) {
        try {
            auto v = json::parse(txt);
            out.nvr_name = v.get("nvr_name").as_string("camera_server NVR");
            out.rest_public = v.get("rest_public").as_bool(false);
            if (v.contains("cameras")) {
                for (const auto& e : v.get("cameras").items()) {
                    CameraCfg r;
                    camera_from_json(e, &r);
                    if (r.id > 0) out.cameras.push_back(r);
                }
            }
        } catch (...) {}
        return out;
    }
    // 更早版本：nvr_settings / camera_registry / camera_names
    out.needs_save = true;
    {
        std::ifstream f2(data_dir + "/nvr_settings.json");
        if (f2) {
            std::stringstream ss2; ss2 << f2.rdbuf();
            try {
                auto v = json::parse(ss2.str());
                out.nvr_name = v.get("nvr_name").as_string("camera_server NVR");
            } catch (...) {}
        }
    }
    {
        std::ifstream f2(data_dir + "/camera_registry.json");
        if (f2) {
            std::stringstream ss2; ss2 << f2.rdbuf();
            try {
                auto v = json::parse(ss2.str());
                for (const auto& [uuid, e] : v.members()) {
                    CameraCfg r;
                    r.uuid = uuid;
                    r.id = static_cast<int>(e.get("id").as_int(0));
                    r.name = e.get("name").as_string("");
                    r.device = e.get("device").as_string("");
                    if (!uuid.empty() && r.id > 0) out.cameras.push_back(r);
                }
            } catch (...) {}
        }
    }
    {
        std::ifstream f2(data_dir + "/camera_names.json");
        if (f2) {
            std::stringstream ss2; ss2 << f2.rdbuf();
            try {
                auto v = json::parse(ss2.str());
                for (const auto& [k, val] : v.members()) {
                    int id = std::atoi(k.c_str());
                    if (id <= 0) continue;
                    std::string nm = val.as_string("");
                    bool found = false;
                    for (auto& r : out.cameras)
                        if (r.id == id) { if (!nm.empty()) r.name = nm; found = true; break; }
                    if (!found && !nm.empty()) {
                        CameraCfg r;
                        r.id = id; r.name = nm;
                        out.cameras.push_back(r);
                    }
                }
            } catch (...) {}
        }
    }
    return out;
}

bool any_legacy_json_exists(const std::string& data_dir) {
    for (const char* fn : {"recorder.json", "camera_names.json",
                           "camera_registry.json", "nvr_settings.json"}) {
        std::error_code ec;
        if (fs::exists(data_dir + "/" + fn, ec)) return true;
    }
    return false;
}

// 旧 data_dir/recorder.json 等合并进 AppCfg（以运行时注册表为准覆盖）
void merge_legacy_recorder_json(AppCfg* cfg) {
    if (!any_legacy_json_exists(cfg->data_dir)) return;
    RecorderJson rj = load_legacy_recorder_json(cfg->data_dir);
    if (!rj.nvr_name.empty()) cfg->nvr_name = rj.nvr_name;
    cfg->rest_public = rj.rest_public;
    for (auto& r : rj.cameras) {
        CameraCfg* found = nullptr;
        for (auto& c : cfg->cameras)
            if ((!r.uuid.empty() && c.uuid == r.uuid) || (r.id > 0 && c.id == r.id) ||
                (!r.device.empty() && c.device == r.device)) {
                found = &c; break;
            }
        if (found) {
            if (!r.name.empty()) found->name = r.name;
            found->uuid = r.uuid;
            if (r.id > 0) found->id = r.id;
            if (!r.device.empty()) found->device = r.device;
            found->width = r.width; found->height = r.height; found->fps = r.fps;
            found->bitrate_kbps = r.bitrate_kbps; found->gop = r.gop;
            found->input_format = r.input_format; found->capture = r.capture;
            found->osd = r.osd;
        } else {
            cfg->cameras.push_back(r);
        }
    }
    remove_legacy_json(cfg->data_dir);
    LOG_INFO("[config] 已合并旧 %s/recorder.json 等配置文件到 %s",
             cfg->data_dir.c_str(), cfg->config_path.c_str());
}

}  // namespace

// ---------- 统一 JSON 配置读写 ----------
void save_json_config(const std::string& config_path, const AppCfg& cfg) {
    auto obj = json::Value::object();
    obj["data_dir"] = cfg.data_dir;
    obj["quota_mb"] = static_cast<int64_t>(cfg.quota_mb);
    obj["segment_time_s"] = static_cast<int64_t>(cfg.segment_time_s);
    obj["encoder"] = cfg.encoder;
    obj["http_port"] = static_cast<int64_t>(cfg.http_port);
    obj["rtsp_port"] = static_cast<int64_t>(cfg.rtsp_port);
    obj["api_key"] = cfg.api_key;
    obj["host_ip"] = cfg.host_ip;
    obj["autostart"] = cfg.autostart;
    obj["onvif_enabled"] = cfg.onvif_enabled;
    obj["onvif_discovery"] = cfg.onvif_discovery;
    obj["nvr_name"] = cfg.nvr_name;
    obj["rest_public"] = cfg.rest_public;
    auto arr = json::Value::array();
    for (const auto& c : cfg.cameras) arr.push_back(camera_to_json(c));
    obj["cameras"] = arr;
    if (write_file_atomic(config_path, json::dump_pretty(obj)))
        LOG_INFO("[config] 已保存统一配置 %s", config_path.c_str());
    else
        LOG_ERROR("[config] 保存配置失败 %s", config_path.c_str());
}

RecorderJson load_recorder_json(const std::string& config_path) {
    RecorderJson out;
    std::string txt = read_file(config_path);
    if (txt.empty()) return out;
    try {
        auto v = json::parse(txt);
        out.nvr_name = v.get("nvr_name").as_string("camera_server NVR");
        out.rest_public = v.get("rest_public").as_bool(false);
        if (v.contains("cameras")) {
            for (const auto& e : v.get("cameras").items()) {
                CameraCfg r;
                camera_from_json(e, &r);
                if (r.id > 0) out.cameras.push_back(r);
            }
        }
    } catch (...) {}
    return out;
}

void save_recorder_json(const std::string& config_path, const RecorderJson& cfg) {
    // 读-改-写：保留 config.json 中 system 字段（data_dir/端口/编码器等）
    AppCfg cur;
    cur.nvr_name = cfg.nvr_name;
    cur.rest_public = cfg.rest_public;
    cur.cameras = cfg.cameras;
    std::string txt = read_file(config_path);
    if (!txt.empty()) {
        try {
            auto v = json::parse(txt);
            cur.data_dir = v.get("data_dir").as_string(cur.data_dir);
            cur.quota_mb = static_cast<uint64_t>(v.get("quota_mb").as_int(static_cast<int64_t>(cur.quota_mb)));
            cur.segment_time_s = static_cast<int>(v.get("segment_time_s").as_int(cur.segment_time_s));
            cur.encoder = v.get("encoder").as_string(cur.encoder);
            cur.http_port = static_cast<int>(v.get("http_port").as_int(cur.http_port));
            cur.rtsp_port = static_cast<int>(v.get("rtsp_port").as_int(cur.rtsp_port));
            cur.api_key = v.get("api_key").as_string(cur.api_key);
            cur.host_ip = v.get("host_ip").as_string(cur.host_ip);
            cur.autostart = v.get("autostart").as_bool(cur.autostart);
            cur.onvif_enabled = v.get("onvif_enabled").as_bool(cur.onvif_enabled);
            cur.onvif_discovery = v.get("onvif_discovery").as_bool(cur.onvif_discovery);
        } catch (...) {}
    }
    save_json_config(config_path, cur);
}

void remove_legacy_json(const std::string& data_dir) {
    for (const char* fn : {"recorder.json", "camera_names.json",
                           "camera_registry.json", "nvr_settings.json"}) {
        std::error_code ec;
        std::filesystem::remove(data_dir + "/" + fn, ec);
    }
}

// ---------- 主入口 ----------
AppCfg load_config(int argc, char** argv) {
    AppCfg cfg;
    bool any_camera_defined = false;
    bool force_mock = false;
    std::string config_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--mock") force_mock = true;
        else if (a == "--duration" && i + 1 < argc) cfg.run_seconds = std::atoi(argv[++i]);
        else if (a == "--list") cfg.list_devices = true;
        else if (a == "--snapshot") cfg.snapshot = true;
        else if (a == "--help") {
            printf("用法: camera_server [--config file(.json|.ini)] [--mock] [--duration N] [--list]\n");
            std::exit(0);
        }
    }

    // 探测配置文件：优先 config.json，其次旧 config.ini（自动迁移）
    if (config_path.empty()) {
        if (fs::exists("config.json")) config_path = "config.json";
        else if (fs::exists("config.ini")) config_path = "config.ini";
    }
    // --config 指向的 .json 尚不存在，但同目录同名 .ini 存在
    // （部署首次启动：ExecStart 已指向 config.json）-> 自动从 ini 迁移
    if (!config_path.empty() && config_path.size() > 5 &&
        config_path.compare(config_path.size() - 5, 5, ".json") == 0 &&
        !fs::exists(config_path)) {
        std::string ini_path = config_path.substr(0, config_path.size() - 5) + ".ini";
        if (fs::exists(ini_path)) {
            LOG_INFO("[config] %s 不存在，改用旧 %s 迁移", config_path.c_str(), ini_path.c_str());
            config_path = ini_path;
        }
    }

    bool is_ini = !config_path.empty() &&
                  (config_path.size() >= 4 &&
                   config_path.compare(config_path.size() - 4, 4, ".ini") == 0);

    if (!config_path.empty() && fs::exists(config_path)) {
        if (is_ini) {
            // 旧 ini -> 迁移到同名 .json
            cfg = parse_ini(config_path);
            std::string json_path = config_path.substr(0, config_path.size() - 4) + ".json";
            cfg.config_path = json_path;
            // 合并旧 data_dir 运行时注册表后再写 JSON（一次性迁移）
            merge_legacy_recorder_json(&cfg);
            assign_camera_ids(&cfg.cameras);
            save_json_config(json_path, cfg);
            LOG_INFO("[config] 已从旧 %s 迁移到统一 %s", config_path.c_str(), json_path.c_str());
        } else {
            cfg.config_path = config_path;
            std::string txt = read_file(config_path);
            if (!txt.empty()) {
                try {
                    auto v = json::parse(txt);
                    cfg.data_dir = v.get("data_dir").as_string(cfg.data_dir);
                    cfg.quota_mb = static_cast<uint64_t>(v.get("quota_mb").as_int(static_cast<int64_t>(cfg.quota_mb)));
                    cfg.segment_time_s = static_cast<int>(v.get("segment_time_s").as_int(cfg.segment_time_s));
                    cfg.encoder = v.get("encoder").as_string(cfg.encoder);
                    cfg.http_port = static_cast<int>(v.get("http_port").as_int(cfg.http_port));
                    cfg.rtsp_port = static_cast<int>(v.get("rtsp_port").as_int(cfg.rtsp_port));
                    cfg.api_key = v.get("api_key").as_string(cfg.api_key);
                    cfg.host_ip = v.get("host_ip").as_string(cfg.host_ip);
                    cfg.autostart = v.get("autostart").as_bool(cfg.autostart);
                    cfg.onvif_enabled = v.get("onvif_enabled").as_bool(cfg.onvif_enabled);
                    cfg.onvif_discovery = v.get("onvif_discovery").as_bool(cfg.onvif_discovery);
                    cfg.nvr_name = v.get("nvr_name").as_string(cfg.nvr_name);
                    cfg.rest_public = v.get("rest_public").as_bool(cfg.rest_public);
                    if (v.contains("cameras")) {
                        for (const auto& e : v.get("cameras").items()) {
                            CameraCfg c;
                            camera_from_json(e, &c);
                            cfg.cameras.push_back(c);
                            if (c.id > 0) any_camera_defined = true;
                        }
                    }
                } catch (...) {
                    LOG_ERROR("[config] 解析 %s 失败，使用默认配置", config_path.c_str());
                }
            }
            // 合并旧 data_dir/recorder.json（存在则迁移并删除）
            if (any_legacy_json_exists(cfg.data_dir)) {
                merge_legacy_recorder_json(&cfg);
                assign_camera_ids(&cfg.cameras);
                save_json_config(config_path, cfg);
            }
        }
    } else if (!config_path.empty()) {
        LOG_ERROR("[config] 配置文件不存在: %s，使用默认配置", config_path.c_str());
    }

    // 无配置文件时的兜底：运行时写回 data_dir/config.json（行为同旧 recorder.json）
    if (cfg.config_path.empty()) cfg.config_path = cfg.data_dir + "/config.json";

    assign_camera_ids(&cfg.cameras);

    // 默认给一路 mock 相机，仅当完全没有定义过相机时
    if (cfg.cameras.empty() && !any_camera_defined) {
        CameraCfg c;
        c.mock = true;
        c.name = "CAM01";
        c.capture = "auto";
        cfg.cameras.push_back(c);
    }
    // --mock 参数强制所有相机 mock（无配置文件时默认补一路 mock 相机）
    if (force_mock) {
        for (auto& c : cfg.cameras) c.mock = true;
    }
    // 只保留 enabled=true 的相机
    std::vector<CameraCfg> enabled_cams;
    for (auto& c : cfg.cameras) if (c.enabled) enabled_cams.push_back(c);
    cfg.cameras = std::move(enabled_cams);
    return cfg;
}

}  // namespace camera
