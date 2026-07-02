#!/usr/bin/env bash
# 方案 1：SO_REUSEPORT + fork，无 Nginx
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/build/simple_http_server"
WWW="$ROOT/www"
PORT="${PORT:-8080}"
WORKERS="${WORKERS:-8}"

stop_server() {
    pkill -f "${BIN} ${PORT} " 2>/dev/null || true
    sleep 0.5
}

case "${1:-start}" in
start)
    stop_server
    if [[ ! -x "$BIN" ]]; then
        echo "请先编译: cd build && cmake .. && make"
        exit 1
    fi
    echo "==> 启动 ${WORKERS} worker，共享端口 ${PORT}（SO_REUSEPORT）"
    (cd "$ROOT/build" && "$BIN" "$PORT" "$WWW" "$WORKERS") &
    echo "    master pid $!"
    sleep 1
    ss -tlnp | grep ":${PORT} " || true
    echo ""
    echo "压测: wrk -t8 -c500 -d10s http://127.0.0.1:${PORT}/"
    ;;
stop)
    stop_server
    echo "已停止"
    ;;
demo)
    stop_server
    echo "==> 单 worker baseline"
    (cd "$ROOT/build" && "$BIN" "$PORT" "$WWW" 1) &
    sleep 1
    wrk -t8 -c500 -d10s "http://127.0.0.1:${PORT}/" || true
    stop_server
    sleep 1
    echo ""
    echo "==> ${WORKERS} workers SO_REUSEPORT"
    (cd "$ROOT/build" && "$BIN" "$PORT" "$WWW" "$WORKERS") &
    sleep 1
    wrk -t8 -c500 -d10s "http://127.0.0.1:${PORT}/" || true
    echo ""
    ps aux | grep "$BIN $PORT" | grep -v grep
    ;;
*)
    echo "用法: PORT=8080 WORKERS=8 $0 {start|stop|demo}"
    exit 1
    ;;
esac
