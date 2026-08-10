"""摄像机录像系统 - FastAPI 入口

启动：uvicorn app.main:app --host 0.0.0.0 --port 8000
接口文档：http://<ip>:8000/docs
"""
from __future__ import annotations

import asyncio
import contextlib
import os
import signal
import threading
import time

from fastapi import Depends, FastAPI, Header, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse

from .config import AppConfig
from .db import DB
from .recorder import CameraRecorder
from .storage import enforce_quota, scan_and_index, storage_stats
from .schemas import ApiResponse, PhotoRequest, RecordStartRequest, RecordStopRequest

app = FastAPI(title="Camera Recording API", version="1.0.0")

# ---------- 全局状态 ----------
CONFIG_PATH = os.environ.get("CAMERA_RECORDER_CONFIG", "config.yaml")
cfg = AppConfig.load(CONFIG_PATH)
db = DB(os.path.join(cfg.storage.data_dir, "db", "records.db"))
recorders: dict[int, CameraRecorder] = {
    c.id: CameraRecorder(c, cfg) for c in cfg.cameras
}

_scan_thread: threading.Thread | None = None
_stop_scan = threading.Event()

# ---------- 鉴权 ----------
def require_key(authorization: str | None = Header(default=None)):
    if not cfg.server.api_key:
        return
    expected = f"Bearer {cfg.server.api_key}"
    if authorization != expected:
        raise HTTPException(status_code=401, detail={"code": 40101, "message": "认证失败"})


def ok(data=None, message: str = "ok") -> dict:
    return {"code": 0, "message": message, "data": data}


def err(status: int, code: int, message: str):
    raise HTTPException(status_code=status, detail={"code": code, "message": message})


def get_recorder(camera_id: int) -> CameraRecorder:
    r = recorders.get(camera_id)
    if not r:
        err(404, 40001, f"相机 {camera_id} 不存在")
    return r


# ---------- 后台扫描/配额 ----------
def _bg_loop():
    while not _stop_scan.is_set():
        try:
            for r in recorders.values():
                scan_and_index(db, r)
            enforce_quota(db, int(cfg.storage.quota_gb * 1024**3))
        except Exception as exc:  # 不因单次异常退出
            import logging
            logging.getLogger("bg").exception("后台扫描失败: %s", exc)
        _stop_scan.wait(60)


@app.on_event("startup")
def startup():
    os.makedirs(os.path.join(cfg.storage.data_dir, "db"), exist_ok=True)
    os.makedirs(os.path.join(cfg.storage.data_dir, "logs"), exist_ok=True)
    for r in recorders.values():
        scan_and_index(db, r)
    if cfg.storage.autostart:
        for r in recorders.values():
            r.start()
    global _scan_thread
    _scan_thread = threading.Thread(target=_bg_loop, daemon=True)
    _scan_thread.start()


@app.on_event("shutdown")
def shutdown():
    _stop_scan.set()
    for r in recorders.values():
        r.stop()


# ---------- 基础 ----------
@app.get("/health")
def health():
    return ok({"status": "ok", "time": time.strftime("%Y-%m-%dT%H:%M:%S%z")})


@app.get("/api/v1/cameras")
def list_cameras(_=Depends(require_key)):
    return ok({"items": [r.status() for r in recorders.values()]})


@app.get("/api/v1/cameras/{camera_id}")
def camera_detail(camera_id: int, _=Depends(require_key)):
    return ok(get_recorder(camera_id).status())


# ---------- 抓拍 ----------
@app.post("/api/v1/cameras/{camera_id}/photo")
def take_photo(camera_id: int, req: PhotoRequest | None = None, _=Depends(require_key)):
    r = get_recorder(camera_id)
    if not r.status()["online"]:
        err(409, 40001, f"相机 {camera_id} 离线")
    quality = (req.quality if req else 90)
    path = r.snapshot(quality)  # 阻塞 IO，单次 <1s
    if not path:
        err(500, 50002, "抓拍失败")
    return ok({
        "camera_id": camera_id,
        "path": path,
        "url": f"/api/v1/cameras/{camera_id}/photo/latest",
    })


@app.get("/api/v1/cameras/{camera_id}/photo/latest")
def photo_latest(camera_id: int, _=Depends(require_key)):
    r = get_recorder(camera_id)
    if not os.path.exists(r.latest_photo):
        err(404, 40401, "还没有照片，请先调用 POST /photo 或启动录像")
    return FileResponse(r.latest_photo, media_type="image/jpeg")


# ---------- 录像控制 ----------
@app.post("/api/v1/cameras/{camera_id}/record/start")
def record_start(camera_id: int, req: RecordStartRequest | None = None, _=Depends(require_key)):
    r = get_recorder(camera_id)
    if not r.status()["online"]:
        err(409, 40001, f"相机 {camera_id} 离线")
    if not r.start():
        err(409, 40002, f"相机 {camera_id} 正在录像中")
    started = r.started_at
    if req and req.duration_s > 0:
        threading.Timer(req.duration_s, r.stop).start()
    return ok({
        "session_id": f"{camera_id}-{started}",
        "camera_id": camera_id,
        "start_time": started,
        "bitrate_kbps": r.cfg.bitrate_kbps,
    })


@app.post("/api/v1/cameras/{camera_id}/record/stop")
def record_stop(camera_id: int, req: RecordStopRequest | None = None, _=Depends(require_key)):
    r = get_recorder(camera_id)
    if not r.stop():
        err(409, 40002, f"相机 {camera_id} 未在录像")
    scan_and_index(db, r)
    return ok({"camera_id": camera_id, "stopped": True})


# ---------- 录像查询/下载 ----------
@app.get("/api/v1/cameras/{camera_id}/recordings")
def list_recordings(camera_id: int,
                    start: str | None = Query(default=None),
                    end: str | None = Query(default=None),
                    page: int = Query(default=1, ge=1),
                    page_size: int = Query(default=20, ge=1, le=200),
                    _=Depends(require_key)):
    get_recorder(camera_id)
    total, items = db.list_recordings(camera_id, start, end, page, page_size)
    data = []
    for it in items:
        data.append({
            "id": it["id"],
            "camera_id": it["camera_id"],
            "start_time": it["start_time"],
            "duration_s": it["duration_s"],
            "size_bytes": it["size_bytes"],
            "event": it["event"],
            "url": f"/api/v1/recordings/{it['id']}/download",
        })
    return ok({"total": total, "page": page, "page_size": page_size, "items": data})


@app.get("/api/v1/recordings/{rec_id}/download")
def download_recording(rec_id: int, _=Depends(require_key)):
    rec = db.get_recording(rec_id)
    if not rec or not os.path.exists(rec["path"]):
        err(404, 40401, "录像不存在或已被循环覆盖")
    name = os.path.basename(rec["path"])
    return FileResponse(rec["path"], media_type="video/mp4",
                        filename=name)


@app.delete("/api/v1/recordings/{rec_id}")
def delete_recording(rec_id: int, _=Depends(require_key)):
    path = db.delete_recording(rec_id)
    if not path:
        err(404, 40401, "录像不存在")
    if os.path.exists(path):
        with contextlib.suppress(OSError):
            os.remove(path)
    return ok({"deleted": rec_id})


# ---------- 系统状态 ----------
@app.get("/api/v1/system/storage")
def system_storage(_=Depends(require_key)):
    return ok(storage_stats(db, cfg.storage.data_dir))


@app.get("/api/v1/system/stats")
def system_stats(_=Depends(require_key)):
    def read(path):
        try:
            with open(path) as f:
                return f.read().strip()
        except Exception:
            return None
    info = {
        "cameras": [r.status() for r in recorders.values()],
        "cpu_temp_c": read("/sys/class/thermal/thermal_zone0/temp"),
        "uptime_s": time.time() - _boot_time(),
    }
    return ok(info)


def _boot_time() -> float:
    try:
        with open("/proc/stat") as f:
            for line in f:
                if line.startswith("btime"):
                    return float(line.split()[1])
    except Exception:
        pass
    return time.time()


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host=cfg.server.host, port=cfg.server.port)
