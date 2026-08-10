# camera-recorder：多路 USB 摄像头录像 + 拍照 API（可运行原型）

> **说明**：这是早期的 Python/FastAPI **原型**，已由 `camera_cpp/`（C++17，Jetson Xavier NX）取代，仅保留作接口验证参考。

基于 **FFmpeg 分段录像 + FastAPI** 的可运行 MVP，对应设计文档 02 的方案 A。
在 RK 板卡上配置 `encoder: auto` 会自动使用 `h264_rkmpp`（Rockchip 硬件编码）；
无硬件时可用 `mock: true` 用合成视频源在任意电脑（含 macOS）上调试全部 API。

## 目录结构

```
camera-recorder/
├── app/
│   ├── main.py        # FastAPI 入口 + 全部 REST 接口
│   ├── config.py      # YAML 配置加载
│   ├── recorder.py    # 每路一个 ffmpeg 子进程（录像 + 每秒抓拍）
│   ├── storage.py     # 录像索引扫描、配额循环覆盖、存储统计
│   ├── db.py          # SQLite（WAL）
│   └── schemas.py     # pydantic 请求模型
├── config.example.yaml
├── requirements.txt
└── scripts/
    ├── run.sh                  # 开发启停
    └── camera-recorder.service # systemd（部署到 RK 板卡）
```

## 快速开始（本机 mock 调试，无需摄像头）

```bash
cd camera-recorder
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# 生成 mock 配置（短分段便于观察录像文件生成）
cat > config.yaml <<'YAML'
server: {host: 0.0.0.0, port: 8000, api_key: ""}
storage: {data_dir: /tmp/cam-demo, quota_gb: 1, segment_time_s: 5, autostart: false}
encoder: libx264
mock: true
cameras:
  - {id: 1, name: CAM01, device: /dev/video0, width: 1280, height: 720, fps: 30, input_format: mjpeg, bitrate_kbps: 2000, gop: 60}
  - {id: 2, name: CAM02, device: /dev/video1, width: 640,  height: 360, fps: 30, input_format: mjpeg, bitrate_kbps: 1200, gop: 60}
YAML

.venv/bin/python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
# 浏览器打开 http://localhost:8000/docs 看 Swagger 文档
```

## API 一览（与设计文档 02 §6 一致）

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | /health | 健康检查 |
| GET | /api/v1/cameras | 相机列表与状态 |
| POST | /api/v1/cameras/{id}/photo | 抓拍一张照片 |
| GET | /api/v1/cameras/{id}/photo/latest | 获取最新照片（jpeg） |
| POST | /api/v1/cameras/{id}/record/start | 开始录像 |
| POST | /api/v1/cameras/{id}/record/stop | 停止录像 |
| GET | /api/v1/cameras/{id}/recordings | 查询录像（时间/分页） |
| GET | /api/v1/recordings/{rid}/download | 下载/播放录像（支持 Range） |
| DELETE | /api/v1/recordings/{rid} | 删除录像 |
| GET | /api/v1/system/storage | 存储状态 |
| GET | /api/v1/system/stats | 系统状态 |

curl 示例：

```bash
# 开始录像
curl -X POST http://localhost:8000/api/v1/cameras/1/record/start -H 'Content-Type: application/json' -d '{"duration_s": 0}'
# 抓拍
curl -X POST http://localhost:8000/api/v1/cameras/1/photo -H 'Content-Type: application/json' -d '{"quality": 90}'
# 最新照片
curl -o latest.jpg http://localhost:8000/api/v1/cameras/1/photo/latest
# 录像列表
curl http://localhost:8000/api/v1/cameras/1/recordings
# 下载（id 换成上面返回的 id）
curl -o clip.mp4 http://localhost:8000/api/v1/recordings/1/download
```

## 部署到 RK 板卡

1. 系统装 ffmpeg（含 rkmpp）：`sudo apt install ffmpeg`，并确认 `ffmpeg -encoders | grep h264_rkmpp` 存在（Rockchip 官方 BSP/Ubuntu 通常自带）；
2. `git clone` 本项目到 `/opt/camera-recorder`，创建 `.venv` 并安装依赖；
3. 复制 `config.example.yaml` 为 `config.yaml`，按实际摄像头填设备名（建议 udev 固定为 `/dev/cam01...`）；
4. 部署 systemd：
   ```bash
   sudo cp scripts/camera-recorder.service /etc/systemd/system/
   sudo systemctl daemon-reload && sudo systemctl enable --now camera-recorder
   ```
5. 验收：`curl localhost:8000/api/v1/cameras` 应看到 12 路在线，`record/start` 后 `/data/recordings` 出现 mp4。

## 说明与限制（MVP）

- 码率/分辨率等参数在 `config.yaml` 配置，启动时生效（`record/start` 请求体中的 `bitrate_kbps` 当前为预留字段）；
- 抓拍机制：录像进程每 1 秒刷新 `latest.jpg`（不打断录像）；`POST /photo` 会单独抓一帧存为时间戳文件；
- 录像分段默认 5 分钟，后台每 60 秒扫描入库 + 按配额删除最旧录像；
- 生产建议：相机改用 H.264 直出（`input_format: h264`）可省掉转码；RK 上 `encoder: h264_rkmpp` 走硬件编码，12 路 CPU 占用 <20%。
