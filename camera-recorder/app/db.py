"""SQLite 索引：录像记录、配额统计（WAL 模式，线程安全）"""
from __future__ import annotations

import os
import sqlite3
import threading
from typing import List, Optional, Tuple


class DB:
    def __init__(self, path: str):
        self._lock = threading.Lock()
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.execute("PRAGMA synchronous=NORMAL")
        self._init()

    def _init(self):
        with self._lock:
            self.conn.execute(
                """
                CREATE TABLE IF NOT EXISTS recordings (
                    id          INTEGER PRIMARY KEY AUTOINCREMENT,
                    camera_id   INTEGER NOT NULL,
                    camera_name TEXT,
                    start_time  TEXT NOT NULL,
                    end_time    TEXT,
                    duration_s  REAL DEFAULT 0,
                    size_bytes  INTEGER DEFAULT 0,
                    path        TEXT NOT NULL UNIQUE,
                    event       TEXT DEFAULT ''
                )
                """
            )
            self.conn.commit()

    def upsert_recording(self, camera_id, camera_name, start_time, end_time,
                         duration_s, size_bytes, path, event=""):
        with self._lock:
            self.conn.execute(
                """
                INSERT INTO recordings(camera_id, camera_name, start_time, end_time,
                                       duration_s, size_bytes, path, event)
                VALUES (?,?,?,?,?,?,?,?)
                ON CONFLICT(path) DO UPDATE SET
                    end_time=excluded.end_time,
                    duration_s=excluded.duration_s,
                    size_bytes=excluded.size_bytes
                """,
                (camera_id, camera_name, start_time, end_time,
                 duration_s, size_bytes, path, event),
            )
            self.conn.commit()

    def list_recordings(self, camera_id: Optional[int] = None,
                        start: Optional[str] = None, end: Optional[str] = None,
                        page: int = 1, page_size: int = 20) -> Tuple[int, List[dict]]:
        where, args = [], []
        if camera_id is not None:
            where.append("camera_id=?")
            args.append(camera_id)
        if start:
            where.append("start_time>=?")
            args.append(start)
        if end:
            where.append("start_time<=?")
            args.append(end)
        cond = (" WHERE " + " AND ".join(where)) if where else ""
        page = max(page, 1)
        page_size = min(max(page_size, 1), 200)
        with self._lock:
            total = self.conn.execute(
                f"SELECT COUNT(*) FROM recordings{cond}", args).fetchone()[0]
            rows = self.conn.execute(
                f"SELECT * FROM recordings{cond} ORDER BY start_time DESC LIMIT ? OFFSET ?",
                args + [page_size, (page - 1) * page_size],
            ).fetchall()
        cols = [d[0] for d in self.conn.execute("SELECT * FROM recordings LIMIT 0").description]
        return total, [dict(zip(cols, r)) for r in rows]

    def get_recording(self, rid: int) -> Optional[dict]:
        with self._lock:
            row = self.conn.execute("SELECT * FROM recordings WHERE id=?", (rid,)).fetchone()
        if not row:
            return None
        cols = [d[0] for d in self.conn.execute("SELECT * FROM recordings LIMIT 0").description]
        return dict(zip(cols, row))

    def get_by_path(self, path: str) -> Optional[dict]:
        with self._lock:
            row = self.conn.execute(
                "SELECT * FROM recordings WHERE path=?", (path,)).fetchone()
        if not row:
            return None
        cols = [d[0] for d in self.conn.execute("SELECT * FROM recordings LIMIT 0").description]
        return dict(zip(cols, row))

    def delete_recording(self, rid: int) -> Optional[str]:
        with self._lock:
            row = self.conn.execute("SELECT path FROM recordings WHERE id=?", (rid,)).fetchone()
            if not row:
                return None
            self.conn.execute("DELETE FROM recordings WHERE id=?", (rid,))
            self.conn.commit()
        return row[0]

    def oldest_recordings(self, limit: int = 200) -> List[dict]:
        with self._lock:
            rows = self.conn.execute(
                "SELECT * FROM recordings ORDER BY start_time ASC LIMIT ?", (limit,)
            ).fetchall()
        cols = [d[0] for d in self.conn.execute("SELECT * FROM recordings LIMIT 0").description]
        return [dict(zip(cols, r)) for r in rows]

    def total_size(self) -> int:
        with self._lock:
            row = self.conn.execute("SELECT COALESCE(SUM(size_bytes),0) FROM recordings").fetchone()
        return int(row[0])

    def count(self) -> int:
        with self._lock:
            return self.conn.execute("SELECT COUNT(*) FROM recordings").fetchone()[0]
