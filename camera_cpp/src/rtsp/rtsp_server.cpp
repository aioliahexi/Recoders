#include "rtsp_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <sstream>

#include "../util/base64.h"
#include "../util/log.h"

namespace camera {

namespace {

constexpr int kRtpPayloadType = 96;
constexpr int kMtu = 1400;

struct RtpHeader {
    uint8_t v_p_x_cc = 0x80;   // version 2
    uint8_t m_pt = kRtpPayloadType;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
};
static_assert(sizeof(RtpHeader) == 12);

uint32_t make_ssrc() {
    static std::atomic<uint32_t> n{0x12345678};
    return n.fetch_add(997);
}

// 从编码器 extradata 提取 SPS/PPS（兼容 avcC 与 annex-b 两种格式）
void extract_sps_pps(const std::vector<uint8_t>& ex, std::vector<uint8_t>* sps, std::vector<uint8_t>* pps) {
    if (ex.size() < 4) return;
    if (ex[0] == 0x01) {
        // avcC：configVersion=1
        size_t p = 5;
        if (p >= ex.size()) return;
        uint8_t nsps = ex[p++];
        for (int i = 0; i < nsps && p + 2 <= ex.size(); i++) {
            uint16_t len = (ex[p] << 8) | ex[p + 1];
            p += 2;
            if (p + len > ex.size()) return;
            if (i == 0) sps->assign(ex.begin() + p, ex.begin() + p + len);
            p += len;
        }
        if (p >= ex.size()) return;
        uint8_t npps = ex[p++];
        for (int i = 0; i < npps && p + 2 <= ex.size(); i++) {
            uint16_t len = (ex[p] << 8) | ex[p + 1];
            p += 2;
            if (p + len > ex.size()) return;
            if (i == 0) pps->assign(ex.begin() + p, ex.begin() + p + len);
            p += len;
        }
        return;
    }
    // annex-b：按起始码切分
    auto find_start_code = [&](size_t from, size_t* code_pos, size_t* nal_pos) -> bool {
        size_t p = from;
        while (p + 3 < ex.size()) {
            if (ex[p] == 0 && ex[p + 1] == 0 && ex[p + 2] == 1) {
                *code_pos = p; *nal_pos = p + 3; return true;
            }
            if (p + 4 < ex.size() && ex[p] == 0 && ex[p + 1] == 0 &&
                ex[p + 2] == 0 && ex[p + 3] == 1) {
                *code_pos = p; *nal_pos = p + 4; return true;
            }
            p++;
        }
        return false;
    };
    size_t code_pos = 0, nal_pos = 0;
    if (!find_start_code(0, &code_pos, &nal_pos)) return;
    while (nal_pos < ex.size()) {
        size_t next_code = ex.size(), next_nal = ex.size();
        bool found = find_start_code(nal_pos, &next_code, &next_nal);
        size_t e = found ? next_code : ex.size();  // 当前 NAL 止于下一个起始码
        while (e > nal_pos && ex[e - 1] == 0) e--;  // 去尾部填充 00
        if (e > nal_pos) {
            uint8_t type = ex[nal_pos] & 0x1f;
            if (type == 7 && sps->empty())
                sps->assign(ex.begin() + nal_pos, ex.begin() + e);
            else if (type == 8 && pps->empty())
                pps->assign(ex.begin() + nal_pos, ex.begin() + e);
        }
        if (!found) break;
        nal_pos = next_nal;
    }
}

// 从 annex-b H.264 包解析 NAL 单元（libx264 GLOBAL_HEADER 实际输出格式）
struct NalView { const uint8_t* data; size_t size; uint8_t type; };
std::vector<NalView> split_nals(const uint8_t* data, size_t size) {
    std::vector<NalView> out;
    auto find_code = [&](size_t from, size_t* code_len, size_t* nal_pos) -> bool {
        size_t p = from;
        while (p + 3 <= size) {
            if (data[p] == 0 && data[p + 1] == 0 && data[p + 2] == 1) {
                *code_len = 3; *nal_pos = p + 3; return true;
            }
            if (p + 4 <= size && data[p] == 0 && data[p + 1] == 0 &&
                data[p + 2] == 0 && data[p + 3] == 1) {
                *code_len = 4; *nal_pos = p + 4; return true;
            }
            p++;
        }
        return false;
    };
    size_t code_len = 0, nal_pos = 0;
    if (!find_code(0, &code_len, &nal_pos)) return out;
    while (nal_pos <= size) {
        size_t next_code_len = 0, next_nal = size;
        bool found = find_code(nal_pos, &next_code_len, &next_nal);
        size_t end = found ? next_nal - next_code_len : size;  // NAL 止于下一起始码
        if (end > nal_pos)
            out.push_back({data + nal_pos, end - nal_pos,
                           static_cast<uint8_t>(data[nal_pos] & 0x1f)});
        if (!found) break;
        nal_pos = next_nal;
    }
    return out;
}

}  // namespace

struct RtspServer::Session {
    int fd = -1;
    int cam_id = 0;
    std::string cam_name;
    std::string session_id;
    enum class Transport { None, Tcp, Udp } transport = Transport::None;
    int interleaved_channel = 0;
    int udp_sock = -1;
    sockaddr_in udp_dst {};
    uint32_t ssrc = make_ssrc();
    uint16_t seq = 0;
    std::atomic<bool> alive{true};
    std::atomic<bool> playing{false};
    size_t listener_token = 0;
    std::vector<uint8_t> sps, pps;   // 由 SETUP 从编码器参数解析

    std::mutex mu;
    struct Cached { uint64_t pts_ms; bool key; std::vector<uint8_t> data; };
    std::vector<Cached> gop;
    bool have_key = false;

    ~Session() {
        alive.store(false);
        if (udp_sock >= 0) ::close(udp_sock);
    }

    void cache(const EncodeFrame& f) {
        if (f.key) {
            gop.clear();
            have_key = true;
        }
        if (!have_key) return;
        constexpr size_t kMaxCache = 60;  // ~2s @30fps
        if (gop.size() >= kMaxCache) gop.erase(gop.begin());
        gop.push_back({f.pts_ms, f.key, std::vector<uint8_t>(f.data, f.data + f.size)});
    }

    // 发送一帧（必须已持有 mu）
    void send_frame_locked(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
                           const EncodeFrame& f) {
        auto nals = split_nals(f.data, f.size);
        if (nals.empty()) return;
        uint32_t ts = static_cast<uint32_t>((f.pts_ms * 90) & 0xffffffff);
        // 关键帧先发 SPS/PPS
        if (f.key) {
            if (!sps.empty()) send_nal_locked(sps.data(), sps.size(), 7, ts, false);
            if (!pps.empty()) send_nal_locked(pps.data(), pps.size(), 8, ts, false);
        }
        for (size_t i = 0; i < nals.size(); i++) {
            bool last = (i == nals.size() - 1);
            send_nal_locked(nals[i].data, nals[i].size, nals[i].type, ts, last);
        }
    }

    void send_nal_locked(const uint8_t* nal, size_t len, uint8_t nal_type, uint32_t ts, bool marker) {
        if (transport == Transport::None) return;
        // 小 NAL：单包
        if (len + 12 <= kMtu) {
            std::vector<uint8_t> pkt(12 + len);
            build_rtp(pkt.data(), ts, marker, nal, len);
            write_rtp(pkt.data(), pkt.size());
            return;
        }
        // FU-A 分片
        uint8_t fu_ind = static_cast<uint8_t>(0x60 | 28);          // NRI 保留 + type 28
        size_t off = 1;
        const size_t chunk = kMtu - 12 - 2;
        bool first = true;
        while (off < len) {
            size_t n = std::min(chunk, len - off);
            std::vector<uint8_t> pkt(12 + 2 + n);
            uint8_t h = static_cast<uint8_t>((first ? 0x80 : 0) | ((off + n >= len) ? 0x40 : 0) | (nal_type & 0x1f));
            build_rtp(pkt.data(), ts, off + n >= len, nullptr, 0);
            pkt[12] = fu_ind;
            pkt[13] = h;
            std::memcpy(pkt.data() + 14, nal + off, n);
            write_rtp(pkt.data(), pkt.size());
            off += n;
            first = false;
        }
    }

    void build_rtp(uint8_t* out, uint32_t ts, bool marker, const uint8_t* payload, size_t plen) {
        RtpHeader h;
        h.v_p_x_cc = 0x80;
        h.m_pt = static_cast<uint8_t>((marker ? 0x80 : 0) | kRtpPayloadType);
        h.seq = htons(seq++);
        h.ts = htonl(ts);
        h.ssrc = htonl(ssrc);
        std::memcpy(out, &h, 12);
        if (payload && plen) std::memcpy(out + 12, payload, plen);
    }

    void write_rtp(const uint8_t* data, size_t len) {
        if (transport == Transport::Tcp) {
            // interleaved framing: $ <channel> <2-byte len> <rtp>
            uint8_t hdr[4];
            hdr[0] = 0x24;
            hdr[1] = static_cast<uint8_t>(interleaved_channel);
            hdr[2] = static_cast<uint8_t>((len >> 8) & 0xff);
            hdr[3] = static_cast<uint8_t>(len & 0xff);
            ::send(fd, hdr, 4, MSG_NOSIGNAL);
            ::send(fd, data, len, MSG_NOSIGNAL);
        } else if (transport == Transport::Udp) {
            if (udp_sock >= 0)
                ::sendto(udp_sock, data, len, 0, reinterpret_cast<struct sockaddr*>(&udp_dst), sizeof(udp_dst));
        }
    }
};

RtspServer::~RtspServer() { stop(); }

bool RtspServer::start(int port, const std::string& host_ip) {
    host_ip_ = host_ip;
    port_ = port;
    for (const auto& [id, name] : ctx_->names) name_to_id_[name] = id;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(fd, 16) < 0) {
        ::close(fd);
        return false;
    }
    listen_fd_ = fd;
    running_ = true;
    threads_.emplace_back([this] { accept_loop(); });
    LOG_INFO("[rtsp] 启动 | rtsp://%s:%d/<camera>", host_ip_.c_str(), port);
    return true;
}

void RtspServer::stop() {
    if (!running_) return;
    running_ = false;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    // 关闭所有会话连接，解除 recv 阻塞
    {
        std::lock_guard<std::mutex> lk(sessions_mu_);
        for (auto& w : sessions_) {
            if (auto s = w.lock()) {
                if (s->fd >= 0) { ::shutdown(s->fd, SHUT_RDWR); ::close(s->fd); s->fd = -1; }
                s->alive.store(false);
            }
        }
    }
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
}

void RtspServer::accept_loop() {
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

static bool read_line(int fd, std::string& out, std::string& buf) {
    while (true) {
        auto pos = buf.find("\n");
        if (pos != std::string::npos) {
            out = buf.substr(0, pos);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            buf.erase(0, pos + 1);
            return true;
        }
        char tmp[4096];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
}

std::shared_ptr<RtspServer::Session> RtspServer::find_or_create(int fd, const std::string& cam_name) {
    std::lock_guard<std::mutex> lk(sessions_mu_);
    auto it = name_to_id_.find(cam_name);
    if (it == name_to_id_.end()) return nullptr;
    auto s = std::make_shared<Session>();
    s->fd = fd;
    s->cam_id = it->second;
    s->cam_name = cam_name;
    s->session_id = "camsrv-" + std::to_string(next_session_id_++);
    sessions_.push_back(s);
    return s;
}

void RtspServer::handle_client(int fd) {
    std::string buf;
    std::shared_ptr<Session> sess;
    std::string line;
    while (running_) {
        if (!read_line(fd, line, buf)) break;
        if (line.empty()) {
            // 请求头结束（本实现 RTSP 请求无 body，直接处理；已处理的行在 parse 阶段消费）
            continue;
        }
        // 解析请求行
        std::istringstream rs(line);
        std::string method, url, version;
        rs >> method >> url >> version;
        // 读头直到空行
        std::string cseq = "0", transport_hdr, session_hdr;
        std::string cam_name = "CAM01";
        auto slash = url.rfind('/');
        if (slash != std::string::npos && slash + 1 < url.size()) cam_name = url.substr(slash + 1);
        while (read_line(fd, line, buf)) {
            if (line.empty()) break;
            auto c = line.find(':');
            if (c == std::string::npos) continue;
            std::string k = line.substr(0, c), v = line.substr(c + 1);
            while (!v.empty() && v.front() == ' ') v.erase(v.begin());
            if (k == "CSeq") cseq = v;
            else if (k == "Transport") transport_hdr = v;
            else if (k == "Session") session_hdr = v;
        }

        auto reply = [&](int status, const std::string& extra_headers, const std::string& body) {
            std::ostringstream os;
            os << "RTSP/1.0 " << status << " "
               << (status == 200 ? "OK" : status == 404 ? "Not Found" : "Error") << "\r\n";
            os << "CSeq: " << cseq << "\r\n";
            os << extra_headers;
            os << "Content-Length: " << body.size() << "\r\n\r\n";
            os << body;
            std::string s = os.str();
            ::send(fd, s.data(), s.size(), MSG_NOSIGNAL);
        };

        if (method == "OPTIONS") {
            reply(200, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", "");
        } else if (method == "DESCRIBE") {
            if (!sess) sess = find_or_create(fd, cam_name);
            if (!sess) { reply(404, "", ""); continue; }
            auto pit = ctx_->pipelines.find(sess->cam_id);
            if (pit == ctx_->pipelines.end()) { reply(404, "", ""); continue; }
            const EncoderParams& enc = pit->second->encoder_params();
            std::vector<uint8_t> sps, pps;
            extract_sps_pps(enc.extradata, &sps, &pps);
            std::string profile_level = "";
            if (sps.size() >= 4) profile_level = hex2(sps.data() + 1, 3);
            std::ostringstream sdp;
            sdp << "v=0\r\n";
            sdp << "o=- 0 0 IN IP4 " << host_ip_ << "\r\n";
            sdp << "s=" << sess->cam_name << "\r\n";
            sdp << "t=0 0\r\n";
            sdp << "m=video 0 RTP/AVP " << kRtpPayloadType << "\r\n";
            sdp << "c=IN IP4 0.0.0.0\r\n";
            sdp << "a=control:trackID=0\r\n";
            sdp << "a=rtpmap:" << kRtpPayloadType << " H264/90000\r\n";
            sdp << "a=framerate:" << enc.fps << "\r\n";
            sdp << "a=fmtp:" << kRtpPayloadType << " packetization-mode=1";
            if (!profile_level.empty()) sdp << "; profile-level-id=" << profile_level;
            if (!sps.empty() && !pps.empty())
                sdp << "; sprop-parameter-sets=" << base64_encode(sps.data(), sps.size())
                    << "," << base64_encode(pps.data(), pps.size());
            sdp << "\r\n";
            reply(200, "Content-Type: application/sdp\r\n", sdp.str());
        } else if (method == "SETUP") {
            if (!sess) sess = find_or_create(fd, cam_name);
            if (!sess) { reply(404, "", ""); continue; }
            sess->transport = Session::Transport::Tcp;
            std::string resp_transport;
            if (transport_hdr.find("RTP/AVP/TCP") != std::string::npos ||
                transport_hdr.find("TCP") != std::string::npos) {
                sess->transport = Session::Transport::Tcp;
                sess->interleaved_channel = 0;
                resp_transport = "RTP/AVP/TCP;unicast;interleaved=0-1";
            } else {
                // UDP：解析 client_port
                sess->transport = Session::Transport::Udp;
                int cport = 0;
                auto cp = transport_hdr.find("client_port=");
                if (cp != std::string::npos) cport = std::atoi(transport_hdr.c_str() + cp + 12);
                if (cport <= 0) { reply(461, "", ""); continue; }
                sess->udp_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
                if (sess->udp_sock < 0) { reply(500, "", ""); continue; }
                ::sockaddr_in src {};
                src.sin_family = AF_INET;
                src.sin_port = 0;
                src.sin_addr.s_addr = htonl(INADDR_ANY);
                ::bind(sess->udp_sock, reinterpret_cast<struct sockaddr*>(&src), sizeof(src));
                ::sockaddr_in dst {};
                dst.sin_family = AF_INET;
                dst.sin_port = htons(static_cast<uint16_t>(cport));
                dst.sin_addr.s_addr = htonl(INADDR_ANY);  // 客户端地址在 PLAY 时获取
                sess->udp_dst = dst;
                resp_transport = "RTP/AVP/UDP;unicast;client_port=" + std::to_string(cport) +
                                 "-" + std::to_string(cport + 1);
            }
            // 注册帧监听：缓存 GOP；PLAY 后实时发送
            auto pit = ctx_->pipelines.find(sess->cam_id);
            if (pit != ctx_->pipelines.end()) {
                extract_sps_pps(pit->second->encoder_params().extradata, &sess->sps, &sess->pps);
                sess->listener_token = pit->second->add_frame_listener(
                    [sess](const EncodeFrame& f) {
                        if (!sess->alive.load()) return;
                        std::lock_guard<std::mutex> lk(sess->mu);
                        sess->cache(f);
                        if (sess->playing.load()) {
                            sess->send_frame_locked(sess->sps, sess->pps, f);
                        }
                    });
            }
            reply(200, "Transport: " + resp_transport + "\r\nSession: " + sess->session_id + "\r\n", "");
        } else if (method == "PLAY") {
            if (!sess) { reply(454, "", ""); continue; }
            sess->playing.store(true);
            // 立即发送缓存的 GOP（从最近关键帧开始）
            {
                std::lock_guard<std::mutex> lk(sess->mu);
                if (sess->have_key) {
                    for (auto& c : sess->gop) {
                        EncodeFrame f{c.pts_ms, c.data.data(), c.data.size(), c.key};
                        sess->send_frame_locked(sess->sps, sess->pps, f);
                    }
                }
            }
            reply(200, "Session: " + sess->session_id + "\r\nRTP-Info: url=rtsp://" + host_ip_ +
                       ":" + std::to_string(port_) + "/" + sess->cam_name + "/trackID=0\r\n", "");
        } else if (method == "TEARDOWN") {
            reply(200, "", "");
            break;
        } else if (method == "GET_PARAMETER") {
            reply(200, "", "");
        } else {
            reply(501, "", "");
        }
    }

    // 清理
    if (sess) {
        sess->alive.store(false);
        auto pit = ctx_->pipelines.find(sess->cam_id);
        if (pit != ctx_->pipelines.end() && sess->listener_token != 0) {
            pit->second->remove_frame_listener(sess->listener_token);
        }
    }
    ::close(fd);
}

}  // namespace camera
