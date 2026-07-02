#!/usr/bin/env bash
# 模拟 3 台 backend（8081/8082/8083）+ Nginx 入口 8090
# 演示：均分 → kill 一台 → 流量切到其余 → 恢复
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
NGINX_CONF="$DEPLOY/nginx-lb-3.conf"
LB="http://127.0.0.1:8090/"
PORTS=(8081 8082 8083)
LABELS=("机器A" "机器B" "机器C")

log() { echo ""; echo ">>> $*"; }

stop_all() {
    nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
    rm -f "$DEPLOY/nginx-3.pid"
    pkill -f "${BIN} 808" 2>/dev/null || true
    sleep 0.5
}

count_estab() {
    ss -H -tan "sport = :$1" 2>/dev/null | awk '$1=="ESTAB"{n++} END{print n+0}'
}

kill_backend() {
    local port=$1
    local pid
    pid=$(pgrep -f "${BIN} ${port} " | head -1 || true)
    if [[ -n "$pid" ]]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
        echo "    已 kill :${port} PID=$pid"
    else
        echo "    :${port} 已无进程"
    fi
}

is_backend_up() {
    pgrep -f "${BIN} ${1} " >/dev/null 2>&1
}

sample_backends() {
    local title=$1
    echo "  [$title]"
    local i=0
    for p in "${PORTS[@]}"; do
        if is_backend_up "$p"; then
            pid=$(pgrep -f "${BIN} ${p} " | head -1)
            cpu=$(ps -p "$pid" -o pcpu= 2>/dev/null | tr -d ' ' || echo "?")
            echo "    ${LABELS[$i]} :${p}  PID=$pid  CPU=${cpu}%  ESTAB=$(count_estab "$p")"
        else
            echo "    ${LABELS[$i]} :${p}  进程=DOWN  ESTAB=$(count_estab "$p")"
        fi
        i=$((i + 1))
    done
    echo "    Nginx 入口 :8090  ESTAB=$(count_estab 8090)"
}

sample_routing() {
    local title=$1
    echo "  [$title] 100 次请求路由分布:"
    local a=0 b=0 c=0 err=0
    for _ in $(seq 1 100); do
        addr=$(curl -sf -D - -o /dev/null "$LB" 2>/dev/null | awk -F: '/^[Xx]-Backend-[Aa]ddr/ {print $NF}' | tr -d '\r ' || echo "")
        case "$addr" in
            *8081*) a=$((a + 1)) ;;
            *8082*) b=$((b + 1)) ;;
            *8083*) c=$((c + 1)) ;;
            *) err=$((err + 1)) ;;
        esac
    done
    echo "    → 机器A(8081): $a   机器B(8082): $b   机器C(8083): $c   失败: $err"
}

[[ -x "$BIN" ]] || { echo "请先编译: cd build && cmake .. && make"; exit 1; }

stop_all

log "1. 启动 3 台 backend（模拟 3 台机器）"
for i in 0 1 2; do
    p=${PORTS[$i]}
    (cd "$BUILD" && "$BIN" "$p" "$WWW") &
    sleep 0.2
    pid=$(pgrep -f "${BIN} ${p} " | head -1)
    echo "    ${LABELS[$i]}  127.0.0.1:${p}  PID=${pid}"
done
sleep 1

log "2. 启动 Nginx 入口 :8090（least_conn + max_fails=2 fail_timeout=10s）"
nginx -t -c "$NGINX_CONF"
nginx -c "$NGINX_CONF"
sleep 0.5
curl -sf -o /dev/null "$LB" || { echo "入口不可用"; exit 1; }
echo "    客户端只需访问: $LB"

log "3. 常态：3 台均在线"
sample_routing "路由"
sample_backends "连接"

log "4. 背景压测 3 路 wrk（各 8 线程 200 连接，共 600，20s）"
for i in 1 2 3; do
    wrk -t8 -c200 -d20s "$LB" > "$DEPLOY/failover_wrk_${i}.log" 2>&1 &
done
sleep 3

log "5. 压测中（3 台全活）"
sample_backends "连接"
sample_routing "路由"

log "6. 💥 模拟机器B(:8082) 宕机"
kill_backend 8082
sleep 4

log "7. 故障后（机器A + 机器C 应承接全部流量）"
sample_backends "连接"
sample_routing "路由"

log "8. 再等 5s（wrk 仍在跑，观察是否自动恢复服务）"
sleep 5
sample_backends "连接"

log "9. 🔧 恢复机器B(:8082)"
(cd "$BUILD" && "$BIN" 8082 "$WWW") &
sleep 0.5
pid=$(pgrep -f "${BIN} 8082 " | head -1)
echo "    机器B 重启 PID=${pid}"
sleep 5

log "10. 恢复后（3 台重新参与）"
sample_backends "连接"
sample_routing "路由"

log "11. 等待 wrk 结束..."
wait 2>/dev/null || true

echo ""
echo "=============================================="
echo " wrk 结果"
echo "=============================================="
total_qps=0
for i in 1 2 3; do
    q=$(grep 'Requests/sec' "$DEPLOY/failover_wrk_${i}.log" | awk '{print $2}')
    err=$(grep 'Socket errors' "$DEPLOY/failover_wrk_${i}.log" 2>/dev/null | tail -1 || echo "无")
    echo "  客户端$i: ${q} req/s  ${err}"
    total_qps=$(awk -v a="$total_qps" -v b="${q:-0}" 'BEGIN{print a+b}')
done
echo "  合计 QPS: $total_qps"

echo ""
echo "=============================================="
echo " 结论"
echo "=============================================="
cat <<'EOF'
  • 8081/8082/8083 = 三台机器，客户端只认 :8090
  • kill 8082 后，X-Backend-Addr 应只剩 8081/8083
  • wrk 压测不中断（可能有短暂 502 后 proxy_next_upstream 重试）
  • 8082 重启后，least_conn 逐步拉回三台
  • 这就是 Nginx 的价值：统一入口 + 故障转移（不是单机 QPS）
EOF

echo ""
echo "停止: nginx -c $NGINX_CONF -s stop; pkill -f '${BIN} 808'"
