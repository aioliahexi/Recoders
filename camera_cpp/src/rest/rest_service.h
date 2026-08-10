// RESTful 服务（补充协议）：相机/抓拍/录像/存储接口
#pragma once

#include <string>

#include "../net/http_server.h"
#include "../services.h"

namespace camera {

class RestService {
public:
    explicit RestService(Context* ctx) : ctx_(ctx) {}

    // 注册路由到 http 服务
    void setup(net::HttpServer& http);

private:
    net::Response handle_cameras(const net::Request& req);
    net::Response handle_camera_detail(const net::Request& req);
    net::Response handle_photo(const net::Request& req);
    net::Response handle_photo_latest(const net::Request& req);
    net::Response handle_record_start(const net::Request& req);
    net::Response handle_record_stop(const net::Request& req);
    net::Response handle_recordings(const net::Request& req);
    net::Response handle_download(const net::Request& req);
    net::Response handle_delete(const net::Request& req);
    net::Response handle_storage(const net::Request& req);
    net::Response handle_stats(const net::Request& req);
    net::Response handle_photos(const net::Request& req);
    net::Response handle_photo_file(const net::Request& req);
    net::Response handle_config_get(const net::Request& req);
    net::Response handle_config_put(const net::Request& req);
    net::Response handle_controls_get(const net::Request& req);
    net::Response handle_controls_put(const net::Request& req);
    net::Response handle_onvif_get(const net::Request& req);
    net::Response handle_onvif_put(const net::Request& req);
    net::Response handle_photo_delete(const net::Request& req);
    net::Response handle_photo_tags(const net::Request& req);
    net::Response handle_settings_get(const net::Request& req);
    net::Response handle_settings_put(const net::Request& req);
    net::Response handle_clear(const net::Request& req);
    net::Response handle_logs(const net::Request& req);
    net::Response handle_discover(const net::Request& req);
    net::Response handle_camera_add(const net::Request& req);
    net::Response handle_camera_delete(const net::Request& req);

    // MJPEG 实时预览流（multipart/x-mixed-replace，长连接）
    void stream_mjpeg(int fd, const net::Request& req);

    Context* ctx_;
};

}  // namespace camera
