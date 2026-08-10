#!/usr/bin/env bash
# 启动/停止 摄像机录像 API 服务（开发调试用）
set -e
cd "$(dirname "$0")/.."

PY=./.venv/bin/python
[ -x "$PY" ] || PY=python3

start() {
  nohup "$PY" -m uvicorn app.main:app \
    --host 0.0.0.0 --port "${PORT:-8000}" \
    > logs/uvicorn.log 2>&1 &
  echo "started pid $!"
}

stop() {
  pkill -f "uvicorn app.main:app" && echo stopped || echo "not running"
}

case "${1:-start}" in
  start) start ;;
  stop) stop ;;
  restart) stop; sleep 1; start ;;
  *) echo "usage: $0 [start|stop|restart]"; exit 1 ;;
esac
