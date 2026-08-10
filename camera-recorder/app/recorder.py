"""每路摄像头一个 ffmpeg 子进程：分段录像 + 每秒刷新 latest.jpg 抓拍"""
from __future__ import annotations

import os
import shutil
import signal
import subprocess
import threading
import time
from datetime import datetime, timezone

from .config import CameraConfig, AppConfig


def detect_encoder(configured: str) -> str:
    """auto: 优先 Rockchip MPP 硬件编码，否则回退软件 libx264"""
    if configured not in ("auto", "h264_rkmpp", "libx264"):
        return "libx264"
    if configured == "libx264":
        return "libx264"
    try:
        out = subprocess.run(["ffmpeg", "-hide_banner", "-encoders"],
                             capture_output=True, text=True, timeout=10).stdout
        if "h264_rkmpp" in out:
            return "h264_rkmpp"
    except Exception:
        pass
    return "libx264"


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class CameraRecorder:
    def __init__(self, cfg: CameraConfig, app_cfg: AppConfig):
        self.cfg = cfg
        self.app_cfg = app_cfg
        self.encoder = detect_encoder(app_cfg.encoder)
        self.proc: subprocess.Popen | None = None
        self.started_at: str | None = None
        self.rec_dir = os.path.join(app_cfg.storage.data_dir, "recordings", cfg.name)
        self.photo_dir = os.path.join(app_cfg.storage.data_dir, "photos", cfg.name)
        self.log_path = os.path.join(app_cfg.storage.data_dir, "logs", f"{cfg.name}.log")
        self._lock = threading.Lock()

    # ---------- 路径 ----------
    @property
    def latest_photo(self) -> str:
        return os.path.join(self.photo_dir, "latest.jpg")

    # ---------- 输入参数 ----------
    def _input_args(self) -> list[str]:
        c = self.cfg
        if self.app_cfg.mock:
            # -re：按实时帧率出帧，模拟真实摄像头节奏
            return ["-re", "-f", "lavfi", "-i", f"testsrc2=size={c.width}x{c.height}:rate={c.fps}"]
        args = ["-f", "v4l2"]
        if c.input_format == "mjpeg":
            args += ["-input_format", "mjpeg"]
        elif c.input_format == "h264":
            args += ["-input_format", "h264"]
        args += ["-video_size", f"{c.width}x{c.height}",
                 "-framerate", str(c.fps), "-i", c.device]
        return args

    # ---------- 录像 ----------
    def _record_cmd(self) -> list[str]:
        c = self.cfg
        os.makedirs(self.rec_dir, exist_ok=True)
        os.makedirs(self.photo_dir, exist_ok=True)
        seg_pattern = os.path.join(self.rec_dir, "%Y%m%d_%H%M%S.mp4")
        main = (["ffmpeg", "-hide_banner", "-loglevel", "warning", "-y"]
                + self._input_args()
                + ["-map", "0:v:0", "-c:v", self.encoder,
                   "-b:v", f"{c.bitrate_kbps}k", "-g", str(c.gop),
                   "-f", "segment", "-segment_time", str(self.app_cfg.storage.segment_time_s),
                   "-reset_timestamps", "1", "-strftime", "1", seg_pattern])
        snap = (["-map", "0:v:0", "-c:v", "mjpeg", "-q:v", "5", "-vf", "fps=1",
                 "-f", "image2", "-update", "1", self.latest_photo])
        return main + snap

    def start(self) -> bool:
        with self._lock:
            if self.proc and self.proc.poll() is None:
                return False
            os.makedirs(os.path.dirname(self.log_path), exist_ok=True)
            logf = open(self.log_path, "ab")
            cmd = self._record_cmd()
            self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                         stdout=logf, stderr=logf,
                                         start_new_session=True)
            self.started_at = now_iso()
            self._logf = logf
        return True

    def stop(self) -> bool:
        with self._lock:
            if not self.proc or self.proc.poll() is not None:
                return False
            try:
                # ffmpeg 收到 'q' 后正常收尾（写完当前段）
                self.proc.stdin.write(b"q")
                self.proc.stdin.flush()
                self.proc.wait(timeout=15)
            except Exception:
                self.proc.send_signal(signal.SIGINT)
                try:
                    self.proc.wait(timeout=10)
                except Exception:
                    self.proc.kill()
            self.started_at = None
            if hasattr(self, "_logf"):
                self._logf.close()
        return True

    def is_running(self) -> bool:
        return bool(self.proc and self.proc.poll() is None)

    # ---------- 抓拍（一次性） ----------
    def snapshot(self, quality: int = 90) -> str | None:
        c = self.cfg
        os.makedirs(self.photo_dir, exist_ok=True)
        ts = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
        out = os.path.join(self.photo_dir, f"{ts}.jpg")
        q = max(2, min(quality, 31))  # mjpeg q 范围 2-31（数值越小质量越高）
        cmd = (["ffmpeg", "-y", "-hide_banner", "-loglevel", "error"]
               + self._input_args()
               + ["-frames:v", "1", "-q:v", str(q), out])
        try:
            subprocess.run(cmd, capture_output=True, timeout=20)
        except Exception:
            return None
        if os.path.exists(out) and os.path.getsize(out) > 0:
            shutil.copyfile(out, self.latest_photo)
            return out
        return None

    # ---------- 状态 ----------
    def status(self) -> dict:
        online = self.app_cfg.mock or os.path.exists(self.cfg.device)
        return {
            "id": self.cfg.id,
            "name": self.cfg.name,
            "device": self.cfg.device,
            "online": online,
            "recording": self.is_running(),
            "pid": self.proc.pid if self.proc else None,
            "started_at": self.started_at,
            "input_format": self.cfg.input_format,
            "resolution": f"{self.cfg.width}x{self.cfg.height}",
            "fps": self.cfg.fps,
            "bitrate_kbps": self.cfg.bitrate_kbps,
            "encoder": self.encoder,
        }
