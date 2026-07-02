#!/usr/bin/env bash
# 方案 0：零代码改动负载均衡 —— 启动/压测/停止
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX_CONF="$DEPLOY/nginx-lb.conf"
PORTS=(8081 8082 8083 8084)
LB_PORT=8090

stop_all() {
    echo "==> 停止 Nginx LB..."
    if [[ -f "$DEPLOY/nginx.pid" ]]; then
        nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
        rm -f "$DEPLOY/nginx.pid"
    fi
    echo "==> 停止 backend 进程..."
    for p in "${PORTS[@]}"; do
        pkill -f "${BIN} ${p} " 2>/dev/null || true
    done
    sleep 0.5
}

start_backends() {
    if [[ ! -x "$BIN" ]]; then
        echo "请先编译: cd build && cmake .. && make"
        exit 1
    fi
    echo "==> 启动 4 个 backend (8081-8084)..."
    for p in "${PORTS[@]}"; do
        (cd "$BUILD" && "$BIN" "$p" "$WWW") &
        echo "    pid $!  port $p"
    done
    sleep 1
}

start_nginx() {
    echo "==> 启动 Nginx LB (0.0.0.0:$LB_PORT)..."
    nginx -c "$NGINX_CONF"
}

run_wrk() {
    local label="$1"
    local url="$2"
    echo ""
    echo "======== wrk: $label ========"
    echo "URL: $url"
    wrk -t4 -c200 -d10s "$url" || true
}

case "${1:-demo}" in
start)
    stop_all
    start_backends
    start_nginx
    echo ""
    echo "就绪:"
    echo "  单 backend 示例: curl http://127.0.0.1:8081/"
    echo "  负载均衡入口:    curl http://127.0.0.1:$LB_PORT/"
    ;;
stop)
    stop_all
    echo "已停止"
    ;;
demo)
    stop_all
    start_backends

    echo ""
    echo "############################################"
    echo "# 方案 0 压测对比（10s，4线程，200连接）"
    echo "############################################"

    run_wrk "单实例 baseline (8081)" "http://127.0.0.1:8081/"

    # 重启 backend，避免 baseline 压测后 8081 仍占热连接
    stop_all
    start_backends
    start_nginx

    run_wrk "Nginx LB → 4 实例 (8090)" "http://127.0.0.1:$LB_PORT/"

    echo ""
    echo "==> 各 backend CPU 占用（负载是否分摊到多进程）:"
    ps aux | grep "$BIN" | grep -v grep | awk '{
        port="?"
        for (i=11; i<=NF; i++) if ($i ~ /^808[1-4]$/) port=$i
        printf "  PID %-6s  CPU %5s%%  port %s\n", $2, $3, port
    }'

    echo ""
    echo "停止: $0 stop"
    ;;
*)
    echo "用法: $0 {start|stop|demo}"
    exit 1
    ;;
esac
