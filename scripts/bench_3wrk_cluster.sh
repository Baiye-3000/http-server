#!/usr/bin/env bash
# 根治方案：3 backend + Nginx + 3 路 wrk 并行压测
# 阶段1 系统调优 → 阶段2 Nginx → 阶段3 3wrk → 阶段4 汇总
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX_CONF="$DEPLOY/nginx-lb-3.conf"
LB_PORT=8090
BACKEND_PORTS=(8081 8082 8083)
DURATION="${DURATION:-10}"
WRK_URL="${WRK_URL:-http://127.0.0.1:8090/page_005.html}"
TARGET_QPS="${TARGET_QPS:-586215.40}"

log() { echo "[$(date '+%H:%M:%S')] $*"; }

stop_all() {
    log "停止旧进程..."
    nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
    rm -f "$DEPLOY/nginx-3.pid"
    pkill -f "${BIN} 808" 2>/dev/null || true
    sleep 0.5
}

# ── 阶段1：系统调优 ──
apply_sysctl() {
    log "阶段1: 系统内核/句柄调优"
    echo 1024 65535 > /proc/sys/net/ipv4/ip_local_port_range
    echo 1 > /proc/sys/net/ipv4/tcp_tw_reuse
    echo 0 > /proc/sys/net/ipv4/tcp_timestamps
    ulimit -n 1048576 2>/dev/null || ulimit -n 65535
    echo "  ip_local_port_range: $(sysctl -n net.ipv4.ip_local_port_range)"
    echo "  tcp_tw_reuse:        $(cat /proc/sys/net/ipv4/tcp_tw_reuse)"
    echo "  tcp_timestamps:      $(cat /proc/sys/net/ipv4/tcp_timestamps)"
    echo "  ulimit -n:           $(ulimit -n)"
}

# ── 阶段2：启动 3 backend + Nginx ──
start_stack() {
    log "阶段2: 启动 3 backend + Nginx"
    for p in "${BACKEND_PORTS[@]}"; do
        (cd "$BUILD" && "$BIN" "$p" "$WWW") &
        echo "  backend pid $! port $p"
    done
    sleep 1
    nginx -t -c "$NGINX_CONF"
    nginx -c "$NGINX_CONF"
    sleep 0.5
    curl -sf -o /dev/null "$WRK_URL" || { echo "URL 不可用: $WRK_URL"; exit 1; }
    echo "  nginx workers: $(pgrep -c 'nginx: worker' || echo 0)"
}

count_estab() {
    ss -H -tan "sport = :$1" 2>/dev/null | awk '$1=="ESTAB"{n++} END{print n+0}'
}

sample_load() {
    local tag=$1
    {
        echo "=== $tag $(date '+%H:%M:%S') ==="
        echo "--- backend CPU ---"
        for p in "${BACKEND_PORTS[@]}"; do
            pid=$(pgrep -f "${BIN} ${p} " | head -1)
            [[ -n "$pid" ]] && ps -p "$pid" -o pid,pcpu,cmd --no-headers 2>/dev/null
        done
        echo "--- nginx worker CPU ---"
        ps -C nginx -o pid,pcpu,cmd --no-headers 2>/dev/null | grep "worker process" || true
        echo "--- ESTAB ---"
        for p in "${BACKEND_PORTS[@]}"; do
            echo "  backend $p: $(count_estab "$p")"
        done
        echo "  nginx $LB_PORT: $(count_estab "$LB_PORT")"
    } >> "$DEPLOY/cpu-sample.txt"
}

parse_wrk_log() {
    local f=$1
    local qps lat_avg lat_max req_sec
    qps=$(grep 'Requests/sec' "$f" | awk '{print $2}')
    read -r lat_avg lat_max <<< "$(grep 'Latency' "$f" | head -1 | awk '{print $2, $4}')"
    req_sec=$(grep 'Req/Sec' "$f" | head -1 | awk '{print $2}')
    echo "$qps ${lat_avg%ms} ${lat_max%ms} $req_sec"
}

# ── 阶段3：3 路 wrk 并行 ──
run_3wrk() {
    log "阶段3: 3 路 wrk -t8 -c500 -d${DURATION}s → $WRK_URL"
    rm -f "$DEPLOY/cpu-sample.txt"
    : > "$DEPLOY/cpu-sample.txt"

    wrk -t8 -c500 -d"${DURATION}s" "$WRK_URL" > "$DEPLOY/wrk_1.log" 2>&1 &
    P1=$!
    wrk -t8 -c500 -d"${DURATION}s" "$WRK_URL" > "$DEPLOY/wrk_2.log" 2>&1 &
    P2=$!
    wrk -t8 -c500 -d"${DURATION}s" "$WRK_URL" > "$DEPLOY/wrk_3.log" 2>&1 &
    P3=$!

    for i in $(seq 1 "$DURATION"); do
        sleep 1
        sample_load "t=${i}s"
    done
    wait "$P1" "$P2" "$P3"
}

run_single_wrk() {
    log "对照: 单 wrk -t8 -c500 -d${DURATION}s"
    wrk -t8 -c500 -d"${DURATION}s" "$WRK_URL" > "$DEPLOY/wrk_single.log" 2>&1
}

# ── main ──
[[ -x "$BIN" ]] || { echo "请先编译: cd build && cmake .. && make"; exit 1; }

stop_all
apply_sysctl
start_stack

echo ""
echo "=============================================="
echo " nginx -t"
echo "=============================================="
nginx -t -c "$NGINX_CONF" 2>&1
ps aux | grep "nginx: worker" | grep -v grep | head -8

run_single_wrk
run_3wrk

# ── 阶段4：汇总 ──
QPS1=$(grep 'Requests/sec' "$DEPLOY/wrk_1.log" | awk '{print $2}')
QPS2=$(grep 'Requests/sec' "$DEPLOY/wrk_2.log" | awk '{print $2}')
QPS3=$(grep 'Requests/sec' "$DEPLOY/wrk_3.log" | awk '{print $2}')
TOTAL_QPS=$(awk -v a="$QPS1" -v b="$QPS2" -v c="$QPS3" 'BEGIN{printf "%.2f", a+b+c}')
SINGLE_THREAD_K=$(awk -v q="$TOTAL_QPS" 'BEGIN{printf "%.2f", q/24/1000}')
SINGLE_QPS=$(grep 'Requests/sec' "$DEPLOY/wrk_single.log" | awk '{print $2}')

total_lat=0 global_max=0
for i in 1 2 3; do
    read -r _ lat_avg lat_max _ <<< "$(parse_wrk_log "$DEPLOY/wrk_${i}.log")"
    total_lat=$(awk -v a="$total_lat" -v b="$lat_avg" 'BEGIN{print a+b}')
    awk -v m="$lat_max" -v g="$global_max" 'BEGIN{if(m>g) exit 1; exit 0}' || global_max=$lat_max
done
AVG_LAT=$(awk -v t="$total_lat" 'BEGIN{printf "%.2f", t/3}')

echo ""
echo "=============================================="
echo " 阶段4: 数据汇总"
echo "=============================================="
echo "  单路 wrk1 QPS:        $QPS1"
echo "  单路 wrk2 QPS:        $QPS2"
echo "  单路 wrk3 QPS:        $QPS3"
echo "  3实例集群总 QPS:      $TOTAL_QPS"
echo "  集群等效单线程吞吐:   ${SINGLE_THREAD_K}k"
echo "  三路平均延迟:         ${AVG_LAT}ms"
echo "  全局最大延迟:         ${global_max}ms"
echo "  单 wrk 对照 QPS:      $SINGLE_QPS"

echo ""
echo "=============================================="
echo " 表1: 三路 wrk 合并后集群总性能"
echo "=============================================="
printf "| %-28s | %-4s | %-6s | %-10s | %-10s | %-10s | %-12s |\n" \
    "测试场景" "线程" "并发" "集群总QPS" "平均延迟" "最大延迟" "单线程Req/Sec"
printf "| %-28s | %-4s | %-6s | %-10s | %-10sms | %-8sms | %-10sk |\n" \
    "Nginx+3实例(3wrk并行)" "24" "1500" "$TOTAL_QPS" "$AVG_LAT" "$global_max" "$SINGLE_THREAD_K"

echo ""
echo "=============================================="
echo " 表2: 横向对比"
echo "=============================================="
printf "| %-28s | %-4s | %-4s | %-12s | %-8s | %-12s |\n" \
    "部署方案" "线程" "并发" "QPS" "平均延迟" "单线程Req/Sec"
single_lat=$(grep 'Latency' "$DEPLOY/wrk_single.log" | head -1 | awk '{print $2}')
printf "| %-28s | %-4s | %-4s | %-12s | %-8s | %-12s |\n" \
    "无LB SO_REUSEPORT 8worker" "8" "500" "586215.40" "1.58ms" "73.28k"
printf "| %-28s | %-4s | %-4s | %-12.0f | %-8s | %-12s |\n" \
    "Nginx+3实例(单wrk)" "8" "500" "$SINGLE_QPS" "$single_lat" "$(awk -v q="$SINGLE_QPS" 'BEGIN{printf "%.2fk", q/8/1000}')"
printf "| %-28s | %-4s | %-4s | %-12s | %-8sms | %-10sk |\n" \
    "Nginx+3实例(3wrk并行)" "24" "1500" "$TOTAL_QPS" "$AVG_LAT" "$SINGLE_THREAD_K"

echo ""
echo "=============================================="
echo " 达标判定"
echo "=============================================="
# backend CPU 均衡
cpu_vals=()
for p in "${BACKEND_PORTS[@]}"; do
    pid=$(pgrep -f "${BIN} ${p} " | head -1)
    cpu=$(ps -p "$pid" -o pcpu= 2>/dev/null | tr -d ' ' || echo 0)
    cpu_vals+=("$cpu")
done
max_cpu=${cpu_vals[0]}; min_cpu=${cpu_vals[0]}
for c in "${cpu_vals[@]}"; do
    awk -v v="$c" -v m="$max_cpu" 'BEGIN{if(v>m) exit 1; exit 0}' || max_cpu=$c
    awk -v v="$c" -v m="$min_cpu" 'BEGIN{if(v<m) exit 1; exit 0}' || min_cpu=$c
done
cpu_diff=$(awk -v a="$max_cpu" -v b="$min_cpu" 'BEGIN{printf "%.1f", a-b}')
echo "  backend CPU: ${cpu_vals[*]} (差值 ${cpu_diff}%)"

if awk -v q="$TOTAL_QPS" -v t="$TARGET_QPS" 'BEGIN{exit (q>=t)?0:1}'; then
    echo "  ✅ QPS 达标: $TOTAL_QPS >= $TARGET_QPS"
else
    pct=$(awk -v q="$TOTAL_QPS" -v t="$TARGET_QPS" 'BEGIN{printf "%.1f", q/t*100}')
    echo "  ❌ QPS 未达标: $TOTAL_QPS (${pct}% of $TARGET_QPS)"
fi
if awk -v l="$AVG_LAT" 'BEGIN{exit (l<=3)?0:1}'; then
    echo "  ✅ 平均延迟 <= 3ms"
else
    echo "  ❌ 平均延迟 ${AVG_LAT}ms > 3ms"
fi
if awk -v d="$cpu_diff" 'BEGIN{exit (d<=10)?0:1}'; then
    echo "  ✅ backend CPU 差值 <= 10%"
else
    echo "  ❌ backend CPU 差值 ${cpu_diff}% > 10%"
fi

echo ""
echo "=============================================="
echo " wrk 原始日志"
echo "=============================================="
for i in 1 2 3; do echo ""; echo "--- wrk_${i}.log ---"; cat "$DEPLOY/wrk_${i}.log"; done

echo ""
echo "=============================================="
echo " 压测期间 CPU 采样 (末 3 次)"
echo "=============================================="
tail -45 "$DEPLOY/cpu-sample.txt"

echo ""
echo " nginx error.log:"
tail -10 "$DEPLOY/nginx-3-error.log" 2>/dev/null || echo "(空)"
