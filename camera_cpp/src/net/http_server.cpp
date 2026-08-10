#include "http_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

namespace camera::net {

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& pattern, Handler h) {
    routes_.push_back({method, std::regex(pattern), std::move(h)});
}

void HttpServer::route_stream(const std::string& method, const std::string& pattern, StreamHandler h) {
    stream_routes_.push_back({method, std::regex(pattern), std::move(h)});
}

bool HttpServer::start(int port, const std::string& host) {
    if (running_) return false;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (host != "0.0.0.0") ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return false;
    }
    if (::listen(fd, 32) < 0) {
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    port_ = port;
    running_ = true;
    threads_.emplace_back([this] { accept_loop(); });
    return true;
}

void HttpServer::stop() {
    if (!running_) return;
    running_ = false;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    // 主动关闭所有活动连接，解除 recv 阻塞，避免 join 卡死
    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        for (int fd : clients_) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        clients_.clear();
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void HttpServer::accept_loop() {
    while (running_) {
        struct sockaddr_in cli {};
        socklen_t len = sizeof(cli);
        int fd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&cli), &len);
        if (fd < 0) {
            if (!running_) break;
            continue;
        }
        threads_.emplace_back([this, fd] { handle_client(fd); });
    }
}

static bool read_until(int fd, std::string& buf, const std::string& delim, size_t max = 1 << 20) {
    while (buf.find(delim) == std::string::npos) {
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
        if (buf.size() > max) return false;
    }
    return true;
}

static std::string url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(s[i + 1]), l = hex(s[i + 2]);
            if (h >= 0 && l >= 0) { out += static_cast<char>((h << 4) | l); i += 2; continue; }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

void HttpServer::handle_client(int fd) {
    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        clients_.push_back(fd);
    }
    std::string raw;
    if (!read_until(fd, raw, "\r\n\r\n")) { ::close(fd); return; }
    size_t head_end = raw.find("\r\n\r\n");
    std::string head = raw.substr(0, head_end);
    std::string rest = raw.substr(head_end + 4);

    std::istringstream hs(head);
    std::string reqline;
    std::getline(hs, reqline);
    std::istringstream rs(reqline);
    std::string method, target;
    rs >> method >> target;

    Request req;
    req.method = method;
    auto qpos = target.find('?');
    if (qpos == std::string::npos) req.path = url_decode(target);
    else {
        req.path = url_decode(target.substr(0, qpos));
        req.query = target.substr(qpos + 1);
    }
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c);
        for (auto& ch : k) ch = static_cast<char>(::tolower(ch));
        std::string v = line.substr(c + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        req.headers[k] = v;
    }
    req.content_length = std::atoll(req.header("content-length").c_str());
    if (req.content_length > 0 && static_cast<int64_t>(rest.size()) < req.content_length) {
        // 补齐 body
        while (static_cast<int64_t>(rest.size()) < req.content_length) {
            char tmp[4096];
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            rest.append(tmp, static_cast<size_t>(n));
        }
    }
    if (req.content_length > 0) rest.resize(static_cast<size_t>(req.content_length));
    req.body = rest;

    if (!try_stream(fd, req)) {
        Response resp = dispatch(req);
        send_response(fd, req, resp);
        ::close(fd);
    }
    // 流式路由由 handler 负责关闭 fd；此处仅从活动连接表移除
    {
        std::lock_guard<std::mutex> lk(clients_mu_);
        for (auto it = clients_.begin(); it != clients_.end(); ++it)
            if (*it == fd) { clients_.erase(it); break; }
    }
}

bool HttpServer::try_stream(int fd, const Request& req) {
    if (auth_check && !auth_check(req)) return false;  // 401 交给普通 dispatch 处理
    for (const auto& r : stream_routes_) {
        if (r.method != "*" && r.method != req.method) continue;
        std::smatch m;
        if (std::regex_match(req.path, m, r.pattern)) {
            r.handler(fd, req);  // handler 自行写响应头/body 并关闭 fd
            return true;
        }
    }
    return false;
}

Response HttpServer::dispatch(const Request& req) {
    if (auth_check && !auth_check(req)) {
        Response resp;
        resp.status = 401;
        resp.body = R"({"code":40101,"message":"认证失败"})";
        return resp;
    }
    for (const auto& r : routes_) {
        if (r.method != "*" && r.method != req.method) continue;
        std::smatch m;
        if (std::regex_match(req.path, m, r.pattern)) {
            return r.handler(req);
        }
    }
    Response resp;
    resp.status = 404;
    resp.body = R"({"code":40401,"message":"Not Found"})";
    return resp;
}

static std::string http_date() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

bool HttpServer::send_response(int fd, const Request& req, const Response& resp) {
    std::ostringstream out;
    out << "HTTP/1.1 " << resp.status << " " << (resp.status == 200 ? "OK" : resp.status == 404 ? "Not Found" : resp.status == 400 ? "Bad Request" : resp.status == 401 ? "Unauthorized" : resp.status == 409 ? "Conflict" : resp.status == 500 ? "Internal Server Error" : "Status") << "\r\n";
    out << "Server: camera_server\r\nDate: " << http_date() << "\r\nConnection: close\r\n";

    // 文件响应（支持单 Range）
    if (!resp.file_path.empty()) {
        std::ifstream f(resp.file_path, std::ios::binary | std::ios::ate);
        if (!f) {
            std::string b = R"({"code":40401,"message":"file not found"})";
            out << "Content-Type: application/json\r\nContent-Length: " << b.size() << "\r\n\r\n" << b;
            std::string s = out.str();
            ::send(fd, s.data(), s.size(), 0);
            return false;
        }
        int64_t total = static_cast<int64_t>(f.tellg());
        f.seekg(0);
        int64_t start = 0, end = total - 1;
        bool partial = false;
        std::string range = req.header("range");
        if (!range.empty() && range.rfind("bytes=", 0) == 0) {
            std::string spec = range.substr(6);
            auto dash = spec.find('-');
            if (dash != std::string::npos) {
                std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
                if (a.empty()) {  // suffix
                    int64_t n = std::atoll(b.c_str());
                    start = std::max<int64_t>(0, total - n);
                } else {
                    start = std::atoll(a.c_str());
                    if (!b.empty()) end = std::atoll(b.c_str());
                }
                if (start >= total) {
                    std::string body = "Range Not Satisfiable";
                    out << "Content-Type: text/plain\r\nContent-Range: bytes */" << total << "\r\nContent-Length: " << body.size() << "\r\n\r\n" << body;
                    std::string s = out.str();
                    ::send(fd, s.data(), s.size(), 0);
                    return false;
                }
                end = std::min(end, total - 1);
                partial = true;
                out.str("");
                out << "HTTP/1.1 206 Partial Content\r\nServer: camera_server\r\nConnection: close\r\n";
            }
        }
        int64_t length = end - start + 1;
        out << "Content-Type: " << resp.content_type << "\r\n";
        out << "Accept-Ranges: bytes\r\n";
        if (partial) out << "Content-Range: bytes " << start << "-" << end << "/" << total << "\r\n";
        out << "Content-Length: " << length << "\r\n\r\n";
        std::string head = out.str();
        ::send(fd, head.data(), head.size(), 0);
        f.seekg(static_cast<std::streamoff>(start));
        char buf[65536];
        int64_t left = length;
        while (left > 0) {
            f.read(buf, std::min<int64_t>(sizeof buf, left));
            ssize_t n = static_cast<ssize_t>(f.gcount());
            if (n <= 0) break;
            ::send(fd, buf, static_cast<size_t>(n), 0);
            left -= n;
        }
        return true;
    }

    for (const auto& [k, v] : resp.headers) {
        out << k << ": " << v << "\r\n";
    }
    out << "Content-Type: " << resp.content_type << "\r\n";
    out << "Content-Length: " << resp.body.size() << "\r\n\r\n";
    out << resp.body;
    std::string s = out.str();
    ::send(fd, s.data(), s.size(), 0);
    return true;
}

}  // namespace camera::net
