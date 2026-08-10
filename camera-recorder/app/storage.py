"""磁盘管理：索引扫描、配额循环覆盖、存储统计"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from datetime import datetime, timezone

from .db import DB
from .recorder import CameraRecorder

_SEG_RE = re.compile(r"^(\d{8})_(\d{6})\.mp4$")


def _probe_duration(path: str) -> float:
    try:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=1", path],
            capture_output=True, text=True, timeout=10).stdout.strip()
        return float(out) if out else 0.0
    except Exception:
        return 0.0


def parse_segment_name(name: str) -> datetime | None:
    m = _SEG_RE.match(name)
    if not m:
        return None
    try:
        return datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S").replace(tzinfo=timezone.utc)
    except ValueError:
        return None


def scan_and_index(db: DB, recorder: CameraRecorder):
    """扫描录像目录，把未入库（或大小有变化）的 mp4 段写入 SQLite"""
    rec_dir = recorder.rec_dir
    if not os.path.isdir(rec_dir):
        return 0
    n = 0
    for name in os.listdir(rec_dir):
        if not name.endswith(".mp4"):
            continue
        path = os.path.join(rec_dir, name)
        try:
            size = os.path.getsize(path)
        except OSError:
            continue
        start = parse_segment_name(name)
        if not start:
            continue
        start_iso = start.isoformat(timespec="seconds")
        # 已入库且大小未变 → 跳过（避免反复 ffprobe 正在写入的段）
        exist = db.get_by_path(path)
        if exist and exist["size_bytes"] == size:
            continue
        duration = _probe_duration(path) or (size * 8.0 / (recorder.cfg.bitrate_kbps * 1000))
        db.upsert_recording(
            camera_id=recorder.cfg.id,
            camera_name=recorder.cfg.name,
            start_time=start_iso,
            end_time=start.isoformat(timespec="seconds"),  # 占位，靠 duration 修正
            duration_s=round(duration, 1),
            size_bytes=size,
            path=path,
        )
        n += 1
    return n


def enforce_quota(db: DB, quota_bytes: int):
    """超过配额时按最旧录像循环删除"""
    if quota_bytes <= 0:
        return 0
    deleted = 0
    while db.total_size() > quota_bytes:
        rows = db.oldest_recordings(limit=10)
        if not rows:
            break
        for r in rows:
            path = r["path"]
            if os.path.exists(path):
                try:
                    os.remove(path)
                except OSError:
                    pass
            db.delete_recording(r["id"])
            deleted += 1
            if db.total_size() <= quota_bytes:
                break
    return deleted


def storage_stats(db: DB, data_dir: str) -> dict:
    os.makedirs(data_dir, exist_ok=True)
    usage = shutil.disk_usage(data_dir)
    rec_size = db.total_size()
    total = usage.total
    free = usage.free
    used = usage.used
    used_pct = round(used / total * 100, 1) if total else 0.0
    return {
        "mount": data_dir,
        "total_bytes": total,
        "used_bytes": used,
        "free_bytes": free,
        "used_percent": used_pct,
        "recordings_bytes": rec_size,
        "recordings_count": db.count(),
    }
