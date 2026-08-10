// 极简 RTSP/H.264 服务器（RTP over TCP interleaved / UDP）
// 每路摄像头 = rtsp://<ip>:<port>/<camera_name>
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../media/pipeline.h"
#include "../services.h"

namespace camera {

class RtspServer {
public:
    explicit RtspServer(Context* ctx) : ctx_(ctx) {}
    ~RtspServer();

    bool start(int port, const std::string& host_ip);
    void stop();
    int port() const { return port_; }

private:
    struct Session;
    void accept_loop();
    void handle_client(int fd);
    std::shared_ptr<Session> find_or_create(int fd, const std::string& cam_name);

    Context* ctx_;
    int listen_fd_ = -1;
    int port_ = 8554;
    std::string host_ip_ = "127.0.0.1";
    volatile bool running_ = false;
    std::vector<std::thread> threads_;
    std::map<std::string, int> name_to_id_;
    std::mutex sessions_mu_;
    std::vector<std::weak_ptr<Session>> sessions_;
    int next_session_id_ = 1;
};

}  // namespace camera
