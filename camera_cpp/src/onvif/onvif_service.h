// 最小 ONVIF 服务（Profile S 子集）：
//   - WS-Discovery（UDP 组播 239.255.255.250:3702）
//   - SOAP: Device(GetDeviceInformation/GetSystemDateAndTime/GetUsers)
//           Media(GetVideoSources/GetProfiles/GetStreamUri/GetSnapshotUri)
// 每路 USB 摄像头 = 一个 MediaProfile；RTSP 指向本机 rtsp://ip:port/<camera>
#pragma once

#include <atomic>
#include <string>
#include <thread>

#include "../net/http_server.h"
#include "../services.h"

namespace camera {

class OnvifService {
public:
    explicit OnvifService(Context* ctx) : ctx_(ctx) {}

    bool start(int http_port, int rtsp_port, const std::string& host_ip);
    void stop();
    // 动态开关（NVR Web 配置页用）
    void set_enabled(bool on);
    void set_discovery(bool on);
    bool discovery_enabled() const { return discovery_enabled_.load(); }
    bool enabled() const { return running_; }

    // SOAP HTTP 端点（注册到 HttpServer）
    net::Response handle_soap(const net::Request& req);

private:
    void discovery_loop();
    void send_hello();
    std::string soap_response(const std::string& operation, const std::string& body_xml,
                              const std::string& ns) const;
    std::string operation_from(const net::Request& req) const;

    Context* ctx_;
    std::thread disc_thread_;
    int sock_ = -1;
    int http_port_ = 8000;
    int rtsp_port_ = 8554;
    std::string host_ip_;
    volatile bool running_ = false;
    std::atomic<bool> discovery_enabled_{true};
};

}  // namespace camera
