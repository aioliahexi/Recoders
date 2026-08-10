cmake_minimum_required(VERSION 3.16)
project(camera_server CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libavdevice)

# 可选：GStreamer nvv4l2 硬件编码后端（Jetson）
pkg_check_modules(GSTREAMER QUIET IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

set(APP_SOURCES
  src/main.cpp
  src/config.cpp
  src/db.cpp
  src/device/device_discovery.cpp
  src/media/ffmpeg_pipeline.cpp
  src/media/v4l2_controls.cpp
  src/camera_manager.cpp
  src/media/recorder.cpp
  src/net/http_server.cpp
  src/onvif/onvif_service.cpp
  src/rest/rest_service.cpp
  src/rtsp/rtsp_server.cpp
  src/web/web_ui.cpp
)

if(GSTREAMER_FOUND)
  message(STATUS "GStreamer 可用 -> 启用 nvv4l2 硬件编码/采集后端")
  list(APPEND APP_SOURCES src/media/gst_encoder.cpp src/media/gst_capture.cpp)
endif()

add_executable(camera_server ${APP_SOURCES})

target_include_directories(camera_server PRIVATE src)
target_link_libraries(camera_server PRIVATE PkgConfig::FFMPEG sqlite3 Threads::Threads)
if(GSTREAMER_FOUND)
  target_compile_definitions(camera_server PRIVATE HAVE_GSTREAMER=1)
  target_link_libraries(camera_server PRIVATE PkgConfig::GSTREAMER)
endif()
target_compile_options(camera_server PRIVATE -Wall -Wextra)

# 可选：RK 板卡硬件编码 h264_rkmpp（Rockchip BSP 自带 FFmpeg）
# pkg_check_modules(RKMPP REQUIRED rockchip_mpp)
# target_link_libraries(camera_server PRIVATE PkgConfig::RKMPP)
