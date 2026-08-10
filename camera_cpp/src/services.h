// 各服务共享的运行时上下文
#pragma once

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

#include "config.h"
#include "db.h"
#include "media/pipeline.h"
#include "media/recorder.h"

namespace camera {

class OnvifService;  // 前向声明

// 探测本机 IPv4（非回环）
inline std::string detect_host_ip() {
    struct ifaddrs* ifa = nullptr;
    if (::getifaddrs(&ifa) != 0) return "127.0.0.1";
    std::string ip = "127.0.0.1";
    for (auto* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<struct sockaddr_in*>(p->ifa_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) { ip = buf; break; }
    }
    ::freeifaddrs(ifa);
    return ip;
}

struct Context {
    AppCfg cfg;
    Db* db = nullptr;
    Recorder* recorder = nullptr;
    std::recursive_mutex cam_mu;   // 保护 pipelines/params/names/uuids（热插拔线程与 REST 并发）
    std::map<int, std::unique_ptr<Pipeline>> pipelines;
    std::map<int, PipelineParams> params;
    std::map<int, std::string> names;   // camera_id -> 相机名
    std::map<int, std::string> uuids;   // camera_id -> 物理设备 UUID
    std::map<int, std::pair<uint16_t, uint16_t>> vp;  // camera_id -> (vid,pid) 换口重绑定用
    uint64_t started_ms = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> rest_public{false};   // 免鉴权 RESTful（调试用，auth_check 每请求读取）
    OnvifService* onvif = nullptr;   // Web 配置页动态控制 ONVIF
    bool onvif_enabled = true;       // 配置持久化
    bool onvif_discovery = true;
};

// 操作日志（线程安全）：写入 SQLite oplog 表
inline void log_operation(Context* ctx, int camera_id, const std::string& action,
                          const std::string& detail, int result) {
    if (!ctx || !ctx->db) return;
    uint64_t ts = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    ctx->db->add_op(ts, camera_id, action, detail, result);
}

}  // namespace camera
