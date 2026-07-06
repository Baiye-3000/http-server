#!/usr/bin/env bash
# Nginx + 8 backend，单 wrk / 8 路 wrk 并行压测
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX_CONF="$DEPLOY/nginx-lb-8.conf"
LB_PORT=8090
BACKEND_PORTS=(8081 8082 8083 8084 8085 8086 8087 8088)
DURATION="${DURATION:-20}"
WRK_THREADS="${WRK_THREADS:-8}"
CONCURRENCY="${CONCURRENCY:-500}"
WRK_CLIENTS="${WRK_CLIENTS:-8}"   # 并行 wrk 路数，1=单路
WRK_PATH="${WRK_PATH:-/page_005.html}"
WRK_URL="http://127.0.0.1:${LB_PORT}${WRK_PATH}"
REPORT="$DEPLOY/bench-nginx-8-latest.txt"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

stop_all() {
    log "停止旧进程..."
    nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
    nginx -c "$DEPLOY/nginx-lb-3.conf" -s stop 2>/dev/null || true
    rm -f "$DEPLOY/nginx-8.pid" "$DEPLOY/nginx-3.pid"
    pkill -f "${BIN} 808" 2>/dev/null || true
    pkill -x wrk 2>/dev/null || true
    sleep 0.5
}

apply_sysctl() {
    echo 1024 65535 > /proc/sys/net/ipv4/ip_local_port_range
    echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
    ulimit -n 1048576 2>/dev/null || ulimit -n 65535
}

count_estab() {
    ss -H -tan "sport = :$1" 2>/dev/null | awk '$1=="ESTAB"{n++} END{print n+0}'
}

sample_backends() {
    local tag=$1
    local out=$2
    {
        echo "=== $tag $(date '+%H:%M:%S') ==="
        for p in "${BACKEND_PORTS[@]}"; do
            pid=$(pgrep -f "${BIN} ${p} " | head -1)
            cpu="n/a"
            [[ -n "$pid" ]] && cpu=$(ps -p "$pid" -o pcpu= 2>/dev/null | tr -d ' ')
            echo "  backend $p: CPU=${cpu}% ESTAB=$(count_estab "$p")"
        done
        echo "  wrk 进程数: $(pgrep -c -x wrk 2>/dev/null || echo 0)"
        ps -C nginx -o pcpu= --no-headers 2>/dev/null | awk '{s+=$1} END{printf "  nginx worker CPU 合计: %.1f%%\n", s}'
    } >> "$out"
}

parse_wrk_log() {
    local f=$1
    local qps lat_avg lat_max
    qps=$(grep 'Requests/sec' "$f" | awk '{print $2}')
    read -r lat_avg lat_max <<< "$(grep 'Latency' "$f" | head -1 | awk '{print $2, $4}')"
    echo "$qps $lat_avg $lat_max"
}

run_wrk_phase() {
    local phase=$1
    local clients=$2
    local cpu_log=$3
    rm -f "$cpu_log"
    : > "$cpu_log"

    log "${phase}: ${clients} 路 wrk -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s"
    local pids=()
    local i
    for ((i = 1; i <= clients; i++)); do
        wrk -t"${WRK_THREADS}" -c"${CONCURRENCY}" -d"${DURATION}s" "$WRK_URL" \
            > "$DEPLOY/wrk-nginx-8_${clients}x_${i}.log" 2>&1 &
        pids+=($!)
    done

    for ((t = 1; t <= DURATION; t++)); do
        sleep 1
        sample_backends "t=${t}s" "$cpu_log"
    done

    for pid in "${pids[@]}"; do
        wait "$pid"
    done
}

[[ -x "$BIN" ]] || { echo "请先编译: cd build && cmake .. && make"; exit 1; }
command -v wrk >/dev/null || { echo "未找到 wrk"; exit 1; }

stop_all
apply_sysctl

log "启动 8 backend + Nginx"
for p in "${BACKEND_PORTS[@]}"; do
    (cd "$BUILD" && "$BIN" "$p" "$WWW") &
done
sleep 1
nginx -t -c "$NGINX_CONF"
nginx -c "$NGINX_CONF"
sleep 0.5
curl -sf -o /dev/null "$WRK_URL" || { echo "URL 不可用: $WRK_URL"; stop_all; exit 1; }

CPU_SINGLE="$DEPLOY/cpu-sample-nginx-8-single.txt"
CPU_MULTI="$DEPLOY/cpu-sample-nginx-8-8wrk.txt"

run_wrk_phase "单 wrk 对照" 1 "$CPU_SINGLE"
read -r SINGLE_QPS SINGLE_LAT SINGLE_MAX <<< "$(parse_wrk_log "$DEPLOY/wrk-nginx-8_1x_1.log")"

run_wrk_phase "8 路 wrk 并行" "$WRK_CLIENTS" "$CPU_MULTI"

TOTAL_QPS=0
TOTAL_LAT=0
GLOBAL_MAX=0
for ((i = 1; i <= WRK_CLIENTS; i++)); do
    read -r qps lat_avg lat_max <<< "$(parse_wrk_log "$DEPLOY/wrk-nginx-8_${WRK_CLIENTS}x_${i}.log")"
    TOTAL_QPS=$(awk -v a="$TOTAL_QPS" -v b="$qps" 'BEGIN{print a+b}')
    lat_num=${lat_avg%ms}
    max_num=${lat_max%ms}
    TOTAL_LAT=$(awk -v a="$TOTAL_LAT" -v b="$lat_num" 'BEGIN{print a+b}')
    awk -v m="$max_num" -v g="$GLOBAL_MAX" 'BEGIN{if(m>g) exit 1; exit 0}' || GLOBAL_MAX=$max_num
done
AVG_LAT=$(awk -v t="$TOTAL_LAT" -v n="$WRK_CLIENTS" 'BEGIN{printf "%.2f", t/n}')

{
    echo "Nginx + 8 backend 压测（单 wrk vs ${WRK_CLIENTS} 路 wrk）"
    echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
    echo "每路 wrk: -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s"
    echo "总并发:   ${WRK_CLIENTS} × ${CONCURRENCY} = $((WRK_CLIENTS * CONCURRENCY))"
    echo "URL:      ${WRK_URL}"
    echo ""
    echo "=============================================="
    echo " 对比表"
    echo "=============================================="
    printf "| %-22s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
        "场景" "线程" "并发" "QPS" "平均延迟" "最大延迟"
    printf "| %-22s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
        "单 wrk" "$WRK_THREADS" "$CONCURRENCY" "$SINGLE_QPS" "$SINGLE_LAT" "$SINGLE_MAX"
    printf "| %-22s | %-4s | %-6s | %-12s | %-10sms | %-10sms |\n" \
        "${WRK_CLIENTS} 路 wrk 合计" "$((WRK_THREADS * WRK_CLIENTS))" "$((CONCURRENCY * WRK_CLIENTS))" \
        "$(awk -v q="$TOTAL_QPS" 'BEGIN{printf "%.2f", q}')" "$AVG_LAT" "$GLOBAL_MAX"
    echo ""
    echo "=============================================="
    echo " 各路 wrk QPS"
    echo "=============================================="
    for ((i = 1; i <= WRK_CLIENTS; i++)); do
        qps=$(grep 'Requests/sec' "$DEPLOY/wrk-nginx-8_${WRK_CLIENTS}x_${i}.log" | awk '{print $2}')
        echo "  wrk_${i}: ${qps}"
    done
    echo ""
    echo "=============================================="
    echo " 单 wrk 原始输出"
    echo "=============================================="
    cat "$DEPLOY/wrk-nginx-8_1x_1.log"
    echo ""
    echo "=============================================="
    echo " 8 路 wrk 原始输出"
    echo "=============================================="
    for ((i = 1; i <= WRK_CLIENTS; i++)); do
        echo "--- wrk_${i} ---"
        cat "$DEPLOY/wrk-nginx-8_${WRK_CLIENTS}x_${i}.log"
        echo ""
    done
    echo "=============================================="
    echo " CPU 采样（8 路 wrk 末 5 次）"
    echo "=============================================="
    tail -40 "$CPU_MULTI"
} | tee "$REPORT"

stop_all
log "完成。报告: $REPORT"
