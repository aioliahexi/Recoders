// 极简日志（生产可换 spdlog）
#pragma once

#include <cstdio>
#include <ctime>
#include <string>

namespace camera {

inline std::string now_str() {
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

#define LOG_INFO(...)  do { std::printf("[%s INFO ] ", camera::now_str().c_str()); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)
#define LOG_ERROR(...) do { std::printf("[%s ERROR] ", camera::now_str().c_str()); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)

}  // namespace camera
