"""配置加载：YAML -> dataclass"""
from __future__ import annotations

import dataclasses
import os
from typing import List, Optional

import yaml


@dataclasses.dataclass
class CameraConfig:
    id: int
    name: str
    device: str = "/dev/video0"
    width: int = 1280
    height: int = 720
    fps: int = 30
    input_format: str = "mjpeg"  # mjpeg / h264 / yuy2
    bitrate_kbps: int = 2000
    gop: int = 60
    enabled: bool = True

    @classmethod
    def from_dict(cls, d: dict) -> "CameraConfig":
        fields = {f.name: f for f in dataclasses.fields(cls)}
        return cls(**{k: v for k, v in d.items() if k in fields})


@dataclasses.dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8000
    api_key: str = ""  # 留空关闭鉴权


@dataclasses.dataclass
class StorageConfig:
    data_dir: str = "/data"
    quota_gb: float = 2000.0
    segment_time_s: int = 300
    autostart: bool = False


@dataclasses.dataclass
class AppConfig:
    server: ServerConfig
    storage: StorageConfig
    encoder: str = "auto"  # auto / h264_rkmpp / libx264
    mock: bool = False  # true 时用 ffmpeg testsrc 模拟摄像头（无硬件也能调试）
    cameras: List[CameraConfig] = dataclasses.field(default_factory=list)

    @classmethod
    def load(cls, path: Optional[str] = None) -> "AppConfig":
        path = path or os.environ.get("CAMERA_RECORDER_CONFIG", "config.yaml")
        with open(path, "r", encoding="utf-8") as f:
            raw = yaml.safe_load(f) or {}
        server = ServerConfig(**raw.get("server", {}))
        storage = StorageConfig(**raw.get("storage", {}))
        cameras = [CameraConfig.from_dict(c) for c in raw.get("cameras", [])]
        return cls(
            server=server,
            storage=storage,
            encoder=raw.get("encoder", "auto"),
            mock=raw.get("mock", False),
            cameras=[c for c in cameras if c.enabled],
        )
