// 嵌入式 Web 管理页面：API Key 登录 + 相机实时画面 + 录像控制/下载
#pragma once

#include <string>

#include "../net/http_server.h"
#include "../services.h"

namespace camera {

// 注册 Web 页面路由（GET /）
void setup_web_ui(net::HttpServer& http, Context* ctx);

}  // namespace camera
