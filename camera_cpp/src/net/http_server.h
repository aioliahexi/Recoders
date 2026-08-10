// 极简 HTTP/1.1 服务器（POSIX socket + 每连接线程，零依赖）
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace camera::net {

struct Request {
    std::string method;                    // GET/POST/DELETE...
    std::string path;                      // 不含 query
    std::string query;                     // query 原文
    std::map<std::string, std::string> headers;  // key 小写
    std::string body;
    int64_t content_length = 0;

    std::string header(const std::string& k) const {
        auto it = headers.find(k);
        return it == headers.end() ? "" : it->second;
    }
};

struct Response {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string file_path;   // 设置后以文件方式返回（支持 Range）

    Response() = default;
    Response(int s, std::string ct, std::string b)
        : status(s), content_type(std::move(ct)), body(std::move(b)) {}
};

using Handler = std::function<Response(const Request&)>;

// 流式路由：handler 自行写 HTTP 响应头与 body（如 multipart/x-mixed-replace 实时流），
// 并负责关闭 fd（长连接，不设 Content-Length）
using StreamHandler = std::function<void(int fd, const Request&)>;

struct Route {
    std::string method;
    std::regex pattern;
    Handler handler;
};

struct StreamRoute {
    std::string method;
    std::regex pattern;
    StreamHandler handler;
};

class HttpServer {
public:
    ~HttpServer();

    // 注册路由：method 为 "GET" 等；pattern 为正则（匹配完整 path）
    void route(const std::string& method, const std::string& pattern, Handler h);

    // 注册流式路由（长连接，handler 自行写响应头/body 并关闭 fd）
    void route_stream(const std::string& method, const std::string& pattern, StreamHandler h);

    // 全局鉴权钩子：返回 false 则直接 401
    std::function<bool(const Request&)> auth_check;

    bool start(int port, const std::string& host = "0.0.0.0");
    void stop();
    int port() const { return port_; }
    bool running() const { return running_; }

private:
    void accept_loop();
    void handle_client(int fd);
    bool try_stream(int fd, const Request& req);  // 命中流式路由则处理并返回 true（fd 由 handler 关闭）
    Response dispatch(const Request& req);
    bool send_response(int fd, const Request& req, const Response& resp);

    std::vector<Route> routes_;
    std::vector<StreamRoute> stream_routes_;
    std::vector<std::thread> threads_;
    std::mutex clients_mu_;
    std::vector<int> clients_;   // 活动连接（stop 时主动关闭以解除阻塞）
    int listen_fd_ = -1;
    int port_ = 0;
    volatile bool running_ = false;
};

}  // namespace camera::net
