#!/usr/bin/env bash
# 三种部署横向性能对比：单实例 / SO_REUSEPORT 8 worker / Nginx+3 backend 集群
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX_CONF="$DEPLOY/nginx-lb-3.conf"
PORT="${PORT:-8080}"
WORKERS="${WORKERS:-8}"
DURATION="${DURATION:-20}"
WRK_THREADS=8
WRK_CONNECTIONS=500
WRK_PATH="/page_005.html"
REPORT="$DEPLOY/bench-all-compare-latest.txt"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

stop_all_services() {
    log "停止所有服务..."
    nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
    rm -f "$DEPLOY/nginx-3.pid"
    pkill -f "${BIN} 808" 2>/dev/null || true
    sleep 0.5
}

apply_sysctl() {
    log "系统内核/句柄调优"
    echo 1024 65535 > /proc/sys/net/ipv4/ip_local_port_range
    echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
    echo 0 > /proc/sys/net/ipv4/tcp_timestamps
    ulimit -n 1048576 2>/dev/null || ulimit -n 65535
}

parse_wrk_log() {
    local f=$1
    local qps lat_avg lat_max errors
    qps=$(grep 'Requests/sec' "$f" | awk '{print $2}')
    read -r lat_avg lat_max <<< "$(grep 'Latency' "$f" | head -1 | awk '{print $2, $4}')"
    errors=$(grep 'Socket errors' "$f" 2>/dev/null | sed 's/^  //' || echo "Socket errors: (none)")
    echo "$qps ${lat_avg} ${lat_max} ${errors}"
}

avg_backend_cpu() {
    local pattern=$1
    local sum=0 count=0 cpu
    while read -r _ cpu _; do
        sum=$(awk -v a="$sum" -v b="$cpu" 'BEGIN{print a+b}')
        count=$((count + 1))
    done < <(pgrep -af "$pattern" 2>/dev/null | while read -r pid _; do
        ps -p "$pid" -o pid=,pcpu=,cmd= --no-headers 2>/dev/null || true
    done)
    if (( count == 0 )); then
        echo "n/a"
    else
        awk -v s="$sum" -v c="$count" 'BEGIN{printf "%.1f", s/c}'
    fi
}

sample_single_cpu() {
    local tag=$1
    local sample_file=$2
    {
        echo "=== $tag $(date '+%H:%M:%S') ==="
        pgrep -af "${BIN} ${PORT} " | while read -r pid _; do
            ps -p "$pid" -o pid=,pcpu=,cmd= --no-headers 2>/dev/null || true
        done
        echo "  avg CPU: $(avg_backend_cpu "${BIN} ${PORT} ")%"
    } >> "$sample_file"
}

run_wrk_sampled() {
    local url=$1
    local log_file=$2
    local sample_file=$3
    local label=$4

    log "${label}: wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${DURATION}s ${url}"
    rm -f "$sample_file"
    : > "$sample_file"

    wrk -t"${WRK_THREADS}" -c"${WRK_CONNECTIONS}" -d"${DURATION}s" "$url" > "$log_file" 2>&1 &
    local wrk_pid=$!
    for _ in $(seq 1 "$DURATION"); do
        sleep 1
        sample_single_cpu "t=${_}s" "$sample_file"
    done
    wait "$wrk_pid"
}

run_single_instance() {
    log "场景1: 单实例 (port ${PORT})"
    stop_all_services
    (cd "$BUILD" && "$BIN" "$PORT" "$WWW" 1) &
    sleep 1
    curl -sf -o /dev/null "http://127.0.0.1:${PORT}${WRK_PATH}" || {
        echo "URL 不可用: http://127.0.0.1:${PORT}${WRK_PATH}"
        exit 1
    }

    local log="$DEPLOY/bench-single-instance.log"
    local cpu="$DEPLOY/cpu-sample-single.txt"
    run_wrk_sampled "http://127.0.0.1:${PORT}${WRK_PATH}" "$log" "$cpu" "单实例"
    stop_all_services
}

run_reuseport() {
    log "场景2: SO_REUSEPORT ${WORKERS} worker (port ${PORT})"
    stop_all_services
    (cd "$BUILD" && "$BIN" "$PORT" "$WWW" "$WORKERS") &
    sleep 1
    curl -sf -o /dev/null "http://127.0.0.1:${PORT}${WRK_PATH}" || {
        echo "URL 不可用: http://127.0.0.1:${PORT}${WRK_PATH}"
        exit 1
    }

    local log="$DEPLOY/bench-reuseport-8.log"
    local cpu="$DEPLOY/cpu-sample-reuseport.txt"
    run_wrk_sampled "http://127.0.0.1:${PORT}${WRK_PATH}" "$log" "$cpu" "SO_REUSEPORT"
    stop_all_services
}

run_nginx_cluster() {
    log "场景3: Nginx + 3 backend 集群"
    DURATION="$DURATION" WRK_URL="http://127.0.0.1:8090${WRK_PATH}" \
        "$ROOT/scripts/bench_3wrk_cluster.sh" > "$DEPLOY/bench-nginx-cluster-run.txt" 2>&1 || true
}

write_report() {
    local s_log="$DEPLOY/bench-single-instance.log"
    local r_log="$DEPLOY/bench-reuseport-8.log"

    read -r s_qps s_lat_avg s_lat_max s_errors <<< "$(parse_wrk_log "$s_log")"
    read -r r_qps r_lat_avg r_lat_max r_errors <<< "$(parse_wrk_log "$r_log")"

    local qps1 qps2 qps3 total_qps avg_lat global_max single_qps single_lat
    qps1=$(grep 'Requests/sec' "$DEPLOY/wrk_1.log" | awk '{print $2}')
    qps2=$(grep 'Requests/sec' "$DEPLOY/wrk_2.log" | awk '{print $2}')
    qps3=$(grep 'Requests/sec' "$DEPLOY/wrk_3.log" | awk '{print $2}')
    total_qps=$(awk -v a="$qps1" -v b="$qps2" -v c="$qps3" 'BEGIN{printf "%.2f", a+b+c}')
    single_qps=$(grep 'Requests/sec' "$DEPLOY/wrk_single.log" | awk '{print $2}')
    single_lat=$(grep 'Latency' "$DEPLOY/wrk_single.log" | head -1 | awk '{print $2}')

    local total_lat=0
    global_max=0
    local i lat_avg lat_max
    for i in 1 2 3; do
        read -r lat_avg lat_max <<< "$(grep 'Latency' "$DEPLOY/wrk_${i}.log" | head -1 | awk '{print $2, $4}')"
        lat_num=${lat_avg%ms}
        max_num=${lat_max%ms}
        total_lat=$(awk -v a="$total_lat" -v b="$lat_num" 'BEGIN{print a+b}')
        awk -v m="$max_num" -v g="$global_max" 'BEGIN{if(m>g) exit 1; exit 0}' || global_max=$max_num
    done
    avg_lat=$(awk -v t="$total_lat" 'BEGIN{printf "%.2f", t/3}')

    local s_cpu r_cpu
    s_cpu=$(grep 'avg CPU' "$DEPLOY/cpu-sample-single.txt" | tail -1 | awk '{print $3}')
    r_cpu=$(grep 'avg CPU' "$DEPLOY/cpu-sample-reuseport.txt" | tail -1 | awk '{print $3}')

    {
        echo "性能测试：三种部署横向对比"
        echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "压测参数: wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${DURATION}s"
        echo "URL: ${WRK_PATH}"
        echo ""
        echo "=============================================="
        echo " 对比表"
        echo "=============================================="
        printf "| %-28s | %-4s | %-6s | %-12s | %-10s | %-10s | %-8s |\n" \
            "部署方案" "线程" "并发" "QPS" "平均延迟" "最大延迟" "CPU"
        printf "| %-28s | %-4s | %-6s | %-12s | %-10s | %-10s | %-8s |\n" \
            "单实例" "$WRK_THREADS" "$WRK_CONNECTIONS" "$s_qps" "$s_lat_avg" "$s_lat_max" "${s_cpu:-n/a}"
        printf "| %-28s | %-4s | %-6s | %-12s | %-10s | %-10s | %-8s |\n" \
            "SO_REUSEPORT x${WORKERS}" "$WRK_THREADS" "$WRK_CONNECTIONS" "$r_qps" "$r_lat_avg" "$r_lat_max" "${r_cpu:-n/a}"
        printf "| %-28s | %-4s | %-6s | %-12s | %-10s | %-10s | %-8s |\n" \
            "Nginx+3实例(3wrk并行)" "$((WRK_THREADS * 3))" "$((WRK_CONNECTIONS * 3))" "$total_qps" "${avg_lat}ms" "${global_max}ms" "见集群日志"
        echo ""
        echo "=============================================="
        echo " Socket errors"
        echo "=============================================="
        echo "单实例:     $s_errors"
        echo "SO_REUSEPORT: $r_errors"
        for i in 1 2 3; do
            echo "集群 wrk_${i}: $(grep 'Socket errors' "$DEPLOY/wrk_${i}.log" 2>/dev/null | sed 's/^  //' || echo '(none)')"
        done
        echo ""
        echo "=============================================="
        echo " 集群补充（单 wrk 对照）"
        echo "=============================================="
        echo "Nginx+3实例(单wrk): QPS=${single_qps} 延迟=${single_lat}"
        echo ""
        echo "=============================================="
        echo " wrk 原始日志"
        echo "=============================================="
        echo "--- 单实例 ---"
        cat "$s_log"
        echo ""
        echo "--- SO_REUSEPORT x${WORKERS} ---"
        cat "$r_log"
        echo ""
        echo "--- 集群 wrk_1/2/3 ---"
        for i in 1 2 3; do
            echo ""; echo "--- wrk_${i}.log ---"; cat "$DEPLOY/wrk_${i}.log"
        done
        echo ""
        echo "=============================================="
        echo " CPU 采样（单实例 / reuseport 末 3 次）"
        echo "=============================================="
        echo "--- 单实例 ---"
        tail -12 "$DEPLOY/cpu-sample-single.txt" 2>/dev/null || true
        echo "--- SO_REUSEPORT ---"
        tail -12 "$DEPLOY/cpu-sample-reuseport.txt" 2>/dev/null || true
        echo ""
        echo "=============================================="
        echo " 集群 CPU 采样（末 3 次）"
        echo "=============================================="
        tail -45 "$DEPLOY/cpu-sample.txt" 2>/dev/null || true
    } | tee "$REPORT"
}

# ── main ──
[[ -x "$BIN" ]] || { echo "请先编译: cd build && cmake .. && make"; exit 1; }
command -v wrk >/dev/null || { echo "未找到 wrk，请先安装"; exit 1; }

mkdir -p "$DEPLOY"
stop_all_services
apply_sysctl

run_single_instance
run_reuseport
run_nginx_cluster
write_report
stop_all_services

log "完成。报告: $REPORT"
