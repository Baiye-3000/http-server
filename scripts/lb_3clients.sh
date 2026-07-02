#!/usr/bin/env bash
# 3 个独立客户端并发压测，每个 wrk -t8 -c500（长连接）
# 使用优化 Nginx 配置：8 backend + reuseport + least_conn + keepalive 1024
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX_CONF="$DEPLOY/nginx-lb-8.conf"
LB_PORT=8090
BACKEND_PORTS=(8081 8082 8083 8084 8085 8086 8087 8088)
DURATION="${DURATION:-15}"
WRK_THREADS="${WRK_THREADS:-8}"
CONCURRENCY="${CONCURRENCY:-500}"

apply_sysctl() {
    echo "==> 内核参数调优"
    echo 1024 65535 > /proc/sys/net/ipv4/ip_local_port_range
    echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
    ulimit -n 1048576 2>/dev/null || ulimit -n 65535
    echo "    ip_local_port_range = $(cat /proc/sys/net/ipv4/ip_local_port_range)"
    echo "    ulimit -n = $(ulimit -n)"
}

stop_all() {
    nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
    nginx -c "$DEPLOY/nginx-lb-3.conf" -s stop 2>/dev/null || true
    rm -f "$DEPLOY/nginx-8.pid" "$DEPLOY/nginx-3.pid"
    pkill -f "${BIN} 808" 2>/dev/null || true
    sleep 1
    if ss -tlnp | grep -q ":${LB_PORT} "; then
        echo "警告: :${LB_PORT} 仍被占用，尝试清理 nginx..."
        pkill -f "nginx.*8090" 2>/dev/null || true
        sleep 1
    fi
}

start_stack() {
    echo "==> 启动 8 backend + Nginx LB (:${LB_PORT}, reuseport)"
    for p in "${BACKEND_PORTS[@]}"; do
        (cd "$BUILD" && "$BIN" "$p" "$WWW") &
    done
    sleep 1
    nginx -t -c "$NGINX_CONF"
    nginx -c "$NGINX_CONF"
    sleep 0.5
}

count_estab() {
    local port=$1
    ss -H -tan "sport = :${port}" 2>/dev/null | awk '$1 == "ESTAB" { c++ } END { print c + 0 }'
}

[[ -x "$BIN" ]] || { echo "请先编译: cd build && cmake .. && make"; exit 1; }

stop_all
apply_sysctl
start_stack

URL="http://127.0.0.1:${LB_PORT}/"
echo ""
echo "=============================================="
echo " 3 客户端并发压测"
echo " 每个: wrk -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s"
echo " 协议: HTTP/1.1 Keep-Alive (wrk 默认)"
echo " 目标: ${URL}"
echo " 总连接: 3 × ${CONCURRENCY} = $((3 * CONCURRENCY))"
echo "=============================================="
echo ""

echo "── baseline: 单 wrk ──"
wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$URL" | tee "$DEPLOY/wrk-single-baseline.out"
echo ""

wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$URL" >"$DEPLOY/wrk-client1.out" 2>&1 &
P1=$!
wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$URL" >"$DEPLOY/wrk-client2.out" 2>&1 &
P2=$!
wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$URL" >"$DEPLOY/wrk-client3.out" 2>&1 &
P3=$!

echo "── 3wrk 压测中采样 (每 2s) ──"
for tick in $(seq 1 $((DURATION / 2))); do
    sleep 2
    kill -0 "$P1" 2>/dev/null || kill -0 "$P2" 2>/dev/null || kill -0 "$P3" 2>/dev/null || break
    nginx_conn=$(count_estab "$LB_PORT")
    total=0
    line=""
    for p in "${BACKEND_PORTS[@]}"; do
        c=$(count_estab "$p")
        total=$((total + c))
        line+="  ${p}:${c}"
    done
    echo "  t=$((tick * 2))s  nginx:${LB_PORT}=${nginx_conn} ESTAB  backends:${line}  (sum=${total})"
done

wait "$P1" "$P2" "$P3"

echo ""
echo "=============================================="
echo " 各客户端 wrk 结果"
echo "=============================================="
total_qps=0
total_req=0
for i in 1 2 3; do
    echo ""
    echo "--- 客户端 ${i} ---"
    grep -E 'Running|threads|Requests/sec|Latency|requests in' "$DEPLOY/wrk-client${i}.out"
    qps=$(grep 'Requests/sec' "$DEPLOY/wrk-client${i}.out" | awk '{print $2}')
    req=$(grep 'requests in' "$DEPLOY/wrk-client${i}.out" | awk '{print $1}')
    total_qps=$(awk -v a="$total_qps" -v b="$qps" 'BEGIN { print a + b }')
    total_req=$(awk -v a="$total_req" -v b="$req" 'BEGIN { print a + b }')
done

echo ""
echo "=============================================="
echo " 汇总"
echo "=============================================="
echo "  合计请求: ${total_req}"
echo "  合计 QPS:  ${total_qps} req/s"
echo ""
echo "── 各 backend 连接 (压测刚结束) ──"
for p in "${BACKEND_PORTS[@]}"; do
    echo "  port ${p}: $(count_estab "$p") ESTAB"
done
