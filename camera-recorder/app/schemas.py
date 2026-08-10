"""API 请求/响应模型"""
from __future__ import annotations

from pydantic import BaseModel, Field


class PhotoRequest(BaseModel):
    quality: int = Field(default=90, ge=1, le=100, description="JPEG 质量 1-100")


class RecordStartRequest(BaseModel):
    bitrate_kbps: int = Field(default=6000, ge=100, le=20000)
    duration_s: int = Field(default=0, ge=0, description="0=一直录到 stop")
    event: str = "manual"


class RecordStopRequest(BaseModel):
    session_id: str | None = None


class ApiResponse(BaseModel):
    code: int = 0
    message: str = "ok"
    data: dict | list | None = None
