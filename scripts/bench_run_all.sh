#!/usr/bin/env bash
# 集成压测：按顺序跑完 wrk 相关测试，汇总成一份报告
# 用法:
#   bash scripts/bench_run_all.sh              # 默认全流程（DURATION=10）
#   QUICK=1 bash scripts/bench_run_all.sh    # 快速：跳过 nginx8 多路 wrk
#   FULL=1 bash scripts/bench_run_all.sh     # 完整：含 nginx8 八路 wrk
#   SKIP_CTEST=1 bash scripts/bench_run_all.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX3="$DEPLOY/nginx-lb-3.conf"
NGINX8="$DEPLOY/nginx-lb-8.conf"
PORT="${PORT:-8080}"
WORKERS="${WORKERS:-8}"
LB_PORT=8090
BACKEND3=(8081 8082 8083)
BACKEND8=(8081 8082 8083 8084 8085 8086 8087 8088)
DURATION="${DURATION:-10}"
WRK_THREADS="${WRK_THREADS:-8}"
CONCURRENCY="${CONCURRENCY:-500}"
WRK_PATH="${WRK_PATH:-/page_005.html}"
REPORT="$DEPLOY/bench-run-all-latest.txt"
QUICK="${QUICK:-0}"
FULL="${FULL:-0}"
SKIP_CTEST="${SKIP_CTEST:-0}"
WRK_PIDS=()

log() { echo "[$(date '+%H:%M:%S')] $*"; }

usage() {
    cat <<EOF
用法: $0

环境变量:
  DURATION=10        每路 wrk 时长（秒）
  WRK_THREADS=8      wrk 线程数
  CONCURRENCY=500    wrk 并发连接数
  WRK_PATH=/page_005.html
  QUICK=1            跳过 nginx8 八路 wrk
  FULL=1             启用 nginx8 八路 wrk（默认 QUICK 时跳过）
  SKIP_CTEST=1       跳过 ctest

阶段顺序:
  0  编译检查 + 系统调优
  1  ctest（功能/长连接回归）
  2  单实例 wrk baseline
  3  SO_REUSEPORT 1 worker vs ${WORKERS} workers
  4  Nginx+3 backend：单 wrk + 3 路 wrk 并行
  5  Nginx+8 backend：单 wrk baseline + 3 路 wrk 并行
  6  Nginx+8 backend：${WORKERS} 路 wrk 并行（FULL=1 且 QUICK=0）

报告: deploy/bench-run-all-latest.txt
EOF
}

[[ "${1:-}" == "-h" || "${1:-}" == "--help" ]] && { usage; exit 0; }

parse_wrk_log() {
    local f=$1
    local qps lat_avg lat_max errors
    qps=$(grep 'Requests/sec' "$f" 2>/dev/null | awk '{print $2}')
    read -r lat_avg lat_max <<< "$(grep 'Latency' "$f" 2>/dev/null | head -1 | awk '{print $2, $4}')"
    errors=$(grep 'Socket errors' "$f" 2>/dev/null | sed 's/^  //' || echo "(none)")
    [[ -z "$qps" ]] && qps="n/a"
    [[ -z "$lat_avg" ]] && lat_avg="n/a"
    [[ -z "$lat_max" ]] && lat_max="n/a"
    echo "$qps $lat_avg $lat_max $errors"
}

sum_qps() {
    awk -v a="$1" -v b="$2" -v c="$3" 'BEGIN{
        if (a=="n/a"||b=="n/a"||c=="n/a") print "n/a"; else printf "%.2f", a+b+c
    }'
}

stop_all() {
    nginx -c "$NGINX8" -s stop 2>/dev/null || true
    nginx -c "$NGINX3" -s stop 2>/dev/null || true
    nginx -c "$DEPLOY/nginx-lb.conf" -s stop 2>/dev/null || true
    rm -f "$DEPLOY/nginx-8.pid" "$DEPLOY/nginx-3.pid" "$DEPLOY/nginx.pid"
    pkill -f "${BIN} 808" 2>/dev/null || true
    pkill -f "${BIN} ${PORT} " 2>/dev/null || true
    pkill -x wrk 2>/dev/null || true
    sleep 0.5
}

apply_sysctl() {
    echo 1024 65535 > /proc/sys/net/ipv4/ip_local_port_range
    echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
    echo 0 > /proc/sys/net/ipv4/tcp_timestamps 2>/dev/null || true
    ulimit -n 1048576 2>/dev/null || ulimit -n 65535
}

run_wrk() {
    local url=$1 log=$2
    wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$url" >"$log" 2>&1
}

run_wrk_bg() {
    local url=$1 log=$2
    wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$url" >"$log" 2>&1 &
    WRK_PIDS+=($!)
}

wait_wrk_all() {
    local pid
    for pid in "${WRK_PIDS[@]}"; do
        wait "$pid" 2>/dev/null || true
    done
    WRK_PIDS=()
}

start_backends() {
    local -n ports=$1
    for p in "${ports[@]}"; do
        (cd "$BUILD" && "$BIN" "$p" "$WWW") >/dev/null 2>&1 &
    done
    sleep 1
}

start_nginx() {
    local conf=$1
    nginx -t -c "$conf"
    nginx -c "$conf"
    sleep 0.5
}

preflight() {
    [[ -x "$BIN" ]] || { echo "请先编译: cd build && cmake .. && make"; exit 1; }
    command -v wrk >/dev/null || { echo "未找到 wrk"; exit 1; }
    command -v nginx >/dev/null || { echo "未找到 nginx"; exit 1; }
    mkdir -p "$DEPLOY"
}

phase_ctest() {
    log "阶段 1/6: ctest"
    (cd "$BUILD" && ctest --output-on-failure) | tee "$DEPLOY/run-all-01-ctest.log"
}

phase_single() {
    log "阶段 2/6: 单实例 baseline (port ${PORT})"
    stop_all
    (cd "$BUILD" && "$BIN" "$PORT" "$WWW" 1) >/dev/null 2>&1 &
    sleep 1
    curl -sf -o /dev/null "http://127.0.0.1:${PORT}${WRK_PATH}"
    run_wrk "http://127.0.0.1:${PORT}${WRK_PATH}" "$DEPLOY/run-all-02-single.log"
    stop_all
}

phase_reuseport() {
    log "阶段 3/6: SO_REUSEPORT 1 worker vs ${WORKERS} workers"
    stop_all
    (cd "$BUILD" && "$BIN" "$PORT" "$WWW" 1) >/dev/null 2>&1 &
    sleep 1
    run_wrk "http://127.0.0.1:${PORT}/" "$DEPLOY/run-all-03-reuseport-1.log"
    stop_all
    sleep 0.5

    (cd "$BUILD" && "$BIN" "$PORT" "$WWW" "$WORKERS") >/dev/null 2>&1 &
    sleep 1
    run_wrk "http://127.0.0.1:${PORT}/" "$DEPLOY/run-all-04-reuseport-${WORKERS}.log"
    stop_all
}

phase_nginx3() {
    log "阶段 4/6: Nginx+3 backend (单 wrk + 3 路 wrk)"
    stop_all
    start_backends BACKEND3
    start_nginx "$NGINX3"
    local url="http://127.0.0.1:${LB_PORT}${WRK_PATH}"
    curl -sf -o /dev/null "$url"

    run_wrk "$url" "$DEPLOY/run-all-05-nginx3-single.log"

    WRK_PIDS=()
    run_wrk_bg "$url" "$DEPLOY/run-all-06-nginx3-3wrk-1.log"
    run_wrk_bg "$url" "$DEPLOY/run-all-06-nginx3-3wrk-2.log"
    run_wrk_bg "$url" "$DEPLOY/run-all-06-nginx3-3wrk-3.log"
    wait_wrk_all
    stop_all
}

phase_nginx8_3clients() {
    log "阶段 5/6: Nginx+8 backend (单 wrk + 3 路 wrk)"
    stop_all
    start_backends BACKEND8
    start_nginx "$NGINX8"
    local url="http://127.0.0.1:${LB_PORT}${WRK_PATH}"
    curl -sf -o /dev/null "$url"

    run_wrk "$url" "$DEPLOY/run-all-07-nginx8-single.log"

    WRK_PIDS=()
    run_wrk_bg "$url" "$DEPLOY/run-all-08-nginx8-3wrk-1.log"
    run_wrk_bg "$url" "$DEPLOY/run-all-08-nginx8-3wrk-2.log"
    run_wrk_bg "$url" "$DEPLOY/run-all-08-nginx8-3wrk-3.log"
    wait_wrk_all
    stop_all
}

phase_nginx8_multi() {
    log "阶段 6/6: Nginx+8 backend (${WORKERS} 路 wrk 并行)"
    stop_all
    start_backends BACKEND8
    start_nginx "$NGINX8"
    local url="http://127.0.0.1:${LB_PORT}${WRK_PATH}"
    curl -sf -o /dev/null "$url"

    WRK_PIDS=()
    local i
    for ((i = 1; i <= WORKERS; i++)); do
        run_wrk_bg "$url" "$DEPLOY/run-all-09-nginx8-${WORKERS}wrk-${i}.log"
    done
    wait_wrk_all
    stop_all
}

write_report() {
    read -r s_qps s_lat s_max s_err <<< "$(parse_wrk_log "$DEPLOY/run-all-02-single.log")"
    read -r r1_qps r1_lat r1_max r1_err <<< "$(parse_wrk_log "$DEPLOY/run-all-03-reuseport-1.log")"
    read -r r8_qps r8_lat r8_max r8_err <<< "$(parse_wrk_log "$DEPLOY/run-all-04-reuseport-${WORKERS}.log")"
    read -r n3s_qps n3s_lat n3s_max n3s_err <<< "$(parse_wrk_log "$DEPLOY/run-all-05-nginx3-single.log")"

    local n3_q1 n3_q2 n3_q3 n3_total
    n3_q1=$(grep 'Requests/sec' "$DEPLOY/run-all-06-nginx3-3wrk-1.log" | awk '{print $2}')
    n3_q2=$(grep 'Requests/sec' "$DEPLOY/run-all-06-nginx3-3wrk-2.log" | awk '{print $2}')
    n3_q3=$(grep 'Requests/sec' "$DEPLOY/run-all-06-nginx3-3wrk-3.log" | awk '{print $2}')
    n3_total=$(sum_qps "$n3_q1" "$n3_q2" "$n3_q3")

    read -r n8s_qps n8s_lat n8s_max n8s_err <<< "$(parse_wrk_log "$DEPLOY/run-all-07-nginx8-single.log")"
    local n8_q1 n8_q2 n8_q3 n8_3total
    n8_q1=$(grep 'Requests/sec' "$DEPLOY/run-all-08-nginx8-3wrk-1.log" | awk '{print $2}')
    n8_q2=$(grep 'Requests/sec' "$DEPLOY/run-all-08-nginx8-3wrk-2.log" | awk '{print $2}')
    n8_q3=$(grep 'Requests/sec' "$DEPLOY/run-all-08-nginx8-3wrk-3.log" | awk '{print $2}')
    n8_3total=$(sum_qps "$n8_q1" "$n8_q2" "$n8_q3")

    local n8_multi_total="n/a"
    if [[ "$FULL" == "1" && "$QUICK" != "1" ]]; then
        local sum=0 i q
        for ((i = 1; i <= WORKERS; i++)); do
            q=$(grep 'Requests/sec' "$DEPLOY/run-all-09-nginx8-${WORKERS}wrk-${i}.log" | awk '{print $2}')
            sum=$(awk -v a="$sum" -v b="$q" 'BEGIN{print a+b}')
        done
        n8_multi_total=$(awk -v s="$sum" 'BEGIN{printf "%.2f", s}')
    fi

    {
        echo "HTTP-SERVER 集成 wrk 压测报告"
        echo "生成时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "参数: wrk -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s  URL=${WRK_PATH}"
        echo "模式: QUICK=${QUICK}  FULL=${FULL}  SKIP_CTEST=${SKIP_CTEST}"
        echo ""
        echo "=============================================="
        echo " 汇总对比表"
        echo "=============================================="
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "场景" "线程" "并发" "QPS" "平均延迟" "最大延迟"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "单实例" "$WRK_THREADS" "$CONCURRENCY" "$s_qps" "$s_lat" "$s_max"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "SO_REUSEPORT 1 worker" "$WRK_THREADS" "$CONCURRENCY" "$r1_qps" "$r1_lat" "$r1_max"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "SO_REUSEPORT ${WORKERS} workers" "$WRK_THREADS" "$CONCURRENCY" "$r8_qps" "$r8_lat" "$r8_max"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "Nginx+3 backend 单 wrk" "$WRK_THREADS" "$CONCURRENCY" "$n3s_qps" "$n3s_lat" "$n3s_max"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "Nginx+3 backend 3wrk 合计" "$((WRK_THREADS * 3))" "$((CONCURRENCY * 3))" "$n3_total" "见分路日志" "见分路日志"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "Nginx+8 backend 单 wrk" "$WRK_THREADS" "$CONCURRENCY" "$n8s_qps" "$n8s_lat" "$n8s_max"
        printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
            "Nginx+8 backend 3wrk 合计" "$((WRK_THREADS * 3))" "$((CONCURRENCY * 3))" "$n8_3total" "见分路日志" "见分路日志"
        if [[ "$FULL" == "1" && "$QUICK" != "1" ]]; then
            printf "| %-32s | %-4s | %-6s | %-12s | %-10s | %-10s |\n" \
                "Nginx+8 backend ${WORKERS}wrk 合计" "$((WRK_THREADS * WORKERS))" "$((CONCURRENCY * WORKERS))" \
                "$n8_multi_total" "见分路日志" "见分路日志"
        fi
        echo ""
        echo "=============================================="
        echo " 分路 QPS"
        echo "=============================================="
        echo "Nginx+3  3wrk: ${n3_q1} + ${n3_q2} + ${n3_q3} = ${n3_total}"
        echo "Nginx+8  3wrk: ${n8_q1} + ${n8_q2} + ${n8_q3} = ${n8_3total}"
        if [[ "$FULL" == "1" && "$QUICK" != "1" ]]; then
            echo -n "Nginx+8 ${WORKERS}wrk: "
            for ((i = 1; i <= WORKERS; i++)); do
                q=$(grep 'Requests/sec' "$DEPLOY/run-all-09-nginx8-${WORKERS}wrk-${i}.log" | awk '{print $2}')
                echo -n "${q} "
            done
            echo "= ${n8_multi_total}"
        fi
        echo ""
        echo "=============================================="
        echo " Socket errors"
        echo "=============================================="
        echo "单实例:              $s_err"
        echo "reuseport 1:         $r1_err"
        echo "reuseport ${WORKERS}:        $r8_err"
        echo "nginx3 single:       $n3s_err"
        echo "nginx8 single:       $n8s_err"
        echo ""
        echo "=============================================="
        echo " 原始日志文件"
        echo "=============================================="
        ls -1 "$DEPLOY"/run-all-*.log 2>/dev/null || true
        echo ""
        echo "=============================================="
        echo " 执行顺序回顾"
        echo "=============================================="
        echo "1. ctest                     → run-all-01-ctest.log"
        echo "2. 单实例 wrk                → run-all-02-single.log"
        echo "3. reuseport 1 worker        → run-all-03-reuseport-1.log"
        echo "4. reuseport ${WORKERS} workers       → run-all-04-reuseport-${WORKERS}.log"
        echo "5. nginx3 单 wrk             → run-all-05-nginx3-single.log"
        echo "6. nginx3 3wrk               → run-all-06-nginx3-3wrk-{1,2,3}.log"
        echo "7. nginx8 单 wrk             → run-all-07-nginx8-single.log"
        echo "8. nginx8 3wrk               → run-all-08-nginx8-3wrk-{1,2,3}.log"
        if [[ "$FULL" == "1" && "$QUICK" != "1" ]]; then
            echo "9. nginx8 ${WORKERS}wrk              → run-all-09-nginx8-${WORKERS}wrk-*.log"
        fi
    } | tee "$REPORT"
}

# ── main ──
preflight
stop_all
apply_sysctl

echo ""
echo "=============================================="
echo " HTTP-SERVER 集成 wrk 压测"
echo " DURATION=${DURATION}s  THREADS=${WRK_THREADS}  CONC=${CONCURRENCY}"
echo "=============================================="
echo ""

if [[ "$SKIP_CTEST" != "1" ]]; then
    phase_ctest
else
    log "跳过 ctest (SKIP_CTEST=1)"
fi

phase_single
phase_reuseport
phase_nginx3
phase_nginx8_3clients

if [[ "$FULL" == "1" && "$QUICK" != "1" ]]; then
    phase_nginx8_multi
else
    log "跳过 nginx8 ${WORKERS} 路 wrk（设置 FULL=1 启用）"
fi

write_report
stop_all

log "完成。汇总报告: $REPORT"
