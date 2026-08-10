#include "onvif_service.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <regex>
#include <sstream>

#include "../util/log.h"

namespace camera {

namespace {

constexpr int kDiscoveryPort = 3702;
constexpr const char* kMulticast = "239.255.255.250";
constexpr const char* kDeviceUuid = "uuid:86e10b40-58b5-4a1b-8b0a-camera-server-0001";

std::string soap_envelope(const std::string& action, const std::string& body) {
    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
       << "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
       << "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
       << "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
       << "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
       << "xmlns:wsa=\"http://www.w3.org/2005/08/addressing\">"
       << "<s:Header><wsa:Action>" << action << "</wsa:Action></s:Header>"
       << "<s:Body>" << body << "</s:Body></s:Envelope>";
    return os.str();
}

std::string xml_escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string utc_now_xml() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace

bool OnvifService::start(int http_port, int rtsp_port, const std::string& host_ip) {
    http_port_ = http_port;
    rtsp_port_ = rtsp_port;
    host_ip_ = host_ip.empty() ? detect_host_ip() : host_ip;
    running_ = true;
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ >= 0) {
        int one = 1;
        ::setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(kDiscoveryPort);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0) {
            // 用实际网卡地址加入组播组（INADDR_ANY 在部分系统不可靠）
            struct ip_mreq mreq {};
            mreq.imr_multiaddr.s_addr = ::inet_addr(kMulticast);
            mreq.imr_interface.s_addr = ::inet_addr(host_ip_.c_str());
            if (::setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
                // 回退：INADDR_ANY
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
                ::setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
            }
            // 发送组播也走该网卡
            ::setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_IF,
                         &mreq.imr_interface.s_addr, sizeof(mreq.imr_interface.s_addr));
        }
        disc_thread_ = std::thread([this] { discovery_loop(); });
    } else {
        running_ = false;
        return false;
    }
    send_hello();
    LOG_INFO("[onvif] 启动 | XAddr=http://%s:%d/onvif/device_service rtsp=rtsp://%s:%d",
             host_ip_.c_str(), http_port_, host_ip_.c_str(), rtsp_port_);
    return true;
}

void OnvifService::stop() {
    running_ = false;
    if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
    if (disc_thread_.joinable()) disc_thread_.join();
}

void OnvifService::set_enabled(bool on) {
    if (on && !running_) {
        start(http_port_, rtsp_port_, host_ip_);
    } else if (!on && running_) {
        stop();
    }
}

void OnvifService::set_discovery(bool on) {
    discovery_enabled_.store(on);
    if (on) send_hello();
    LOG_INFO("[onvif] WS-Discovery %s", on ? "启用" : "停用");
}

void OnvifService::send_hello() {
    if (sock_ < 0) return;
    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
       << "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
       << "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
       << "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
       << "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
       << "<e:Header><w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello</w:Action>"
       << "<w:MessageID>" << kDeviceUuid << "-hello</w:MessageID>"
       << "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To></e:Header>"
       << "<e:Body><d:Hello><d:EndpointReference><w:Address>" << kDeviceUuid << "</w:Address></d:EndpointReference>"
       << "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
       << "<d:Scopes>onvif://www.onvif.org/name/camera_server onvif://www.onvif.org/Profile/Streaming</d:Scopes>"
       << "<d:XAddrs>http://" << host_ip_ << ":" << http_port_ << "/onvif/device_service</d:XAddrs>"
       << "<d:MetadataVersion>1</d:MetadataVersion></d:Hello></e:Body></e:Envelope>";
    struct sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(kDiscoveryPort);
    dst.sin_addr.s_addr = ::inet_addr(kMulticast);
    ::sendto(sock_, os.str().data(), os.str().size(), 0,
             reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst));
}

void OnvifService::discovery_loop() {
    char buf[8192];
    while (running_ && sock_ >= 0) {
        struct sockaddr_in from {};
        socklen_t flen = sizeof(from);
        ssize_t n = ::recvfrom(sock_, buf, sizeof(buf) - 1, 0,
                               reinterpret_cast<struct sockaddr*>(&from), &flen);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string msg(buf);
        if (msg.find("Probe") == std::string::npos) continue;
        if (!discovery_enabled_.load()) continue;  // 发现已停用

        std::string relates = "";
        std::smatch m;
        std::regex mid(R"(<w:MessageID[^>]*>([^<]+))");
        if (std::regex_search(msg, m, mid)) relates = m[1].str();

        std::ostringstream os;
        os << "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
           << "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
           << "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
           << "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
           << "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
           << "<e:Header>"
           << "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</w:Action>"
           << "<w:MessageID>" << kDeviceUuid << "-probe</w:MessageID>"
           << (relates.empty() ? "" : "<w:RelatesTo>" + relates + "</w:RelatesTo>")
           << "</e:Header>"
           << "<e:Body><d:ProbeMatches><d:ProbeMatch>"
           << "<d:EndpointReference><w:Address>" << kDeviceUuid << "</w:Address></d:EndpointReference>"
           << "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
           << "<d:Scopes>onvif://www.onvif.org/name/camera_server onvif://www.onvif.org/Profile/Streaming</d:Scopes>"
           << "<d:XAddrs>http://" << host_ip_ << ":" << http_port_ << "/onvif/device_service</d:XAddrs>"
           << "<d:MetadataVersion>1</d:MetadataVersion>"
           << "</d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>";
        ::sendto(sock_, os.str().data(), os.str().size(), 0,
                 reinterpret_cast<struct sockaddr*>(&from), flen);
        LOG_INFO("[onvif] 响应 WS-Discovery Probe");
    }
}

std::string OnvifService::operation_from(const net::Request& req) const {
    // 优先 SOAPAction 头
    std::string action = req.header("soapaction");
    if (!action.empty()) {
        for (auto& c : action) if (c == '"') c = ' ';
        auto pos = action.rfind('/');
        if (pos != std::string::npos) {
            std::string op = action.substr(pos + 1);
            op.erase(op.find_last_not_of(" \t") + 1);
            if (!op.empty() && op.find("http") == std::string::npos) return op;
        }
    }
    // 回退：从 body 里找 <GetXxx 或 :GetXxx
    std::smatch m;
    std::regex re(R"(<(?:\w+:)?(Get[A-Za-z0-9]+))");
    if (std::regex_search(req.body, m, re)) return m[1].str();
    return "";
}

net::Response OnvifService::handle_soap(const net::Request& req) {
    std::string op = operation_from(req);
    std::string ns_dev = "http://www.onvif.org/ver10/device/wsdl";
    std::string ns_media = "http://www.onvif.org/ver10/media/wsdl";
    std::string body;
    std::string ns;

    if (op == "GetDeviceInformation") {
        ns = ns_dev;
        body = "<GetDeviceInformationResponse>"
               "<Manufacturer>Hangzhou Hanlu Subsea</Manufacturer>"
               "<Model>camera_server v1.0 (RK3588/Jetson)</Model>"
               "<FirmwareVersion>1.0.0</FirmwareVersion>"
               "<SerialNumber>" + std::string(kDeviceUuid) + "</SerialNumber>"
               "<HardwareId>camera-server-rk</HardwareId>"
               "</GetDeviceInformationResponse>";
    } else if (op == "GetSystemDateAndTime") {
        ns = ns_dev;
        body = "<GetSystemDateAndTimeResponse><SystemDateAndTime>"
               "<DateTimeType>Manual</DateTimeType>"
               "<DaylightSavings>false</DaylightSavings>"
               "<TimeZone><TZ>UTC+08:00</TZ></TimeZone>"
               "<UTCDateTime><Time>" + utc_now_xml() + "</Time></UTCDateTime>"
               "</SystemDateAndTime></GetSystemDateAndTimeResponse>";
    } else if (op == "GetUsers") {
        ns = ns_dev;
        body = "<GetUsersResponse><User><Username>admin</Username>"
               "<UserLevel>Administrator</UserLevel></User></GetUsersResponse>";
    } else if (op == "GetNetworkInterfaces") {
        ns = ns_dev;
        body = "<GetNetworkInterfacesResponse><NetworkInterfaces token=\"eth0\">"
               "<Name>eth0</Name><IPv4><Enabled>true</Enabled><Config><Manual>"
               "<Address>" + host_ip_ + "</Address><PrefixLength>24</PrefixLength>"
               "</Manual></Config></IPv4></NetworkInterfaces></GetNetworkInterfacesResponse>";
    } else if (op == "GetVideoSources") {
        ns = ns_media;
        std::ostringstream os;
        os << "<GetVideoSourcesResponse>";
        for (const auto& [id, _] : ctx_->pipelines) {
            os << "<VideoSources token=\"video" << id << "\">"
               << "<Name>" << xml_escape(ctx_->names[id]) << "</Name>"
               << "<Resolution><Width>" << ctx_->params[id].width
               << "</Width><Height>" << ctx_->params[id].height << "</Height></Resolution>"
               << "<Framerate>" << ctx_->params[id].fps << "</Framerate>"
               << "</VideoSources>";
        }
        os << "</GetVideoSourcesResponse>";
        body = os.str();
    } else if (op == "GetProfiles") {
        ns = ns_media;
        std::ostringstream os;
        os << "<GetProfilesResponse>";
        for (const auto& [id, _] : ctx_->pipelines) {
            os << "<Profiles token=\"" << xml_escape(ctx_->names[id]) << "\" fixed=\"true\">"
               << "<Name>" << xml_escape(ctx_->names[id]) << "</Name>"
               << "<VideoEncoderConfiguration token=\"" << xml_escape(ctx_->names[id]) << "enc\" fixed=\"true\">"
               << "<Name>" << xml_escape(ctx_->names[id]) << "Encoder</Name>"
               << "<Encoding>H264</Encoding>"
               << "<Resolution><Width>" << ctx_->params[id].width
               << "</Width><Height>" << ctx_->params[id].height << "</Height></Resolution>"
               << "<RateControl><FrameRateLimit>" << ctx_->params[id].fps
               << "</FrameRateLimit><EncodingInterval>1</EncodingInterval>"
               << "<BitrateLimit>" << ctx_->params[id].bitrate_kbps << "</BitrateLimit></RateControl>"
               << "<H264><GovLength>" << ctx_->params[id].gop
               << "</GovLength><H264Profile>Baseline</H264Profile></H264>"
               << "</VideoEncoderConfiguration>"
               << "</Profiles>";
        }
        os << "</GetProfilesResponse>";
        body = os.str();
    } else if (op == "GetStreamUri") {
        ns = ns_media;
        std::string token;
        std::smatch m;
        std::regex re(R"(<ProfileToken[^>]*>([^<]+))");
        if (std::regex_search(req.body, m, re)) token = m[1].str();
        if (token.empty()) token = "CAM01";
        std::string cam = token;
        // 若 token 带空格/非法，回退第一个
        int id = 0;
        for (const auto& [cid, _] : ctx_->pipelines) { id = cid; break; }
        body = "<GetStreamUriResponse><MediaUri>"
               "<Uri>rtsp://" + host_ip_ + ":" + std::to_string(rtsp_port_) + "/" + xml_escape(cam) + "</Uri>"
               "<InvalidAfterConnect>false</InvalidAfterConnect><InvalidAfterReboot>false</InvalidAfterReboot><Timeout>PT5S</Timeout>"
               "</MediaUri></GetStreamUriResponse>";
        (void)id;
    } else if (op == "GetSnapshotUri") {
        ns = ns_media;
        std::string token;
        std::smatch m;
        std::regex re(R"(<ProfileToken[^>]*>([^<]+))");
        if (std::regex_search(req.body, m, re)) token = m[1].str();
        int id = 1;
        for (const auto& [cid, _] : ctx_->pipelines) { id = cid; break; }
        body = "<GetSnapshotUriResponse><MediaUri>"
               "<Uri>http://" + host_ip_ + ":" + std::to_string(http_port_) +
               "/api/v1/cameras/" + std::to_string(id) + "/photo/latest</Uri>"
               "</MediaUri></GetSnapshotUriResponse>";
        (void)token;
    } else if (op == "GetServices") {
        ns = ns_dev;
        body = "<GetServicesResponse>"
               "<Service><Namespace>http://www.onvif.org/ver10/device/wsdl</Namespace>"
               "<XAddr>http://" + host_ip_ + ":" + std::to_string(http_port_) + "/onvif/device_service</XAddr>"
               "<Version><Major>2</Major><Minor>6</Minor></Version></Service>"
               "<Service><Namespace>http://www.onvif.org/ver10/media/wsdl</Namespace>"
               "<XAddr>http://" + host_ip_ + ":" + std::to_string(http_port_) + "/onvif/media_service</XAddr>"
               "<Version><Major>2</Major><Minor>6</Minor></Version></Service>"
               "</GetServicesResponse>";
    } else if (op == "GetCapabilities") {
        ns = ns_dev;
        body = "<GetCapabilitiesResponse><Capabilities><Device><XAddr>http://" + host_ip_ + ":" +
               std::to_string(http_port_) + "/onvif/device_service</XAddr></Device>"
               "<Media><XAddr>http://" + host_ip_ + ":" + std::to_string(http_port_) +
               "/onvif/media_service</XAddr></Media>"
               "<Events><XAddr>http://" + host_ip_ + ":" + std::to_string(http_port_) +
               "/onvif/event_service</XAddr></Events>"
               "</Capabilities></GetCapabilitiesResponse>";
    } else {
        net::Response resp;
        resp.status = 400;
        resp.content_type = "text/xml; charset=utf-8";
        resp.body = soap_envelope("http://www.onvif.org/ver10/device/wsdl/Fault",
                                  "<Fault><Code><Value>s:Receiver</Value></Code>"
                                  "<Reason><Text>Unsupported operation: " + xml_escape(op) + "</Text></Reason></Fault>");
        return resp;
    }

    net::Response resp;
    resp.status = 200;
    resp.content_type = "application/soap+xml; charset=utf-8";
    resp.body = soap_envelope(ns + "/" + op + "Response", body);
    return resp;
}

}  // namespace camera
