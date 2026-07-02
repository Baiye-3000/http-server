#!/usr/bin/env bash
# 负载均衡 CPU/连接分布检测脚本
# 在 wrk 压测期间采样：各 backend 的 CPU 占用、运行核、ESTABLISHED 连接数
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
DEPLOY="$ROOT/deploy"
BIN="$BUILD/simple_http_server"
WWW="$ROOT/www"
LB_PORT=8090

BACKENDS="${BACKENDS:-4}"
CONCURRENCY="${CONCURRENCY:-500}"
WRK_THREADS="${WRK_THREADS:-8}"
DURATION="${DURATION:-10}"
SAMPLE_INTERVAL="${SAMPLE_INTERVAL:-1}"

usage() {
    cat <<EOF
用法: $0 [选项]

环境变量:
  BACKENDS=4        backend 进程数（端口 8081 起）
  CONCURRENCY=500   wrk 并发连接数
  WRK_THREADS=8     wrk 线程数
  DURATION=10       压测时长（秒）
  SAMPLE_INTERVAL=1 采样间隔（秒）

示例:
  BACKENDS=8 CONCURRENCY=500 $0
EOF
}

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
    exit 0
fi

if [[ ! -x "$BIN" ]]; then
    echo "请先编译: cd build && cmake .. && make"
    exit 1
fi

PORTS=()
for ((i = 0; i < BACKENDS; i++)); do
    PORTS+=($((8081 + i)))
done

NGINX_CONF="$DEPLOY/nginx-lb-check-${BACKENDS}.conf"
PID_FILE="$DEPLOY/nginx-lb-check-${BACKENDS}.pid"
ERROR_LOG="$DEPLOY/nginx-lb-check-${BACKENDS}.error.log"

# ---------- CPU 拓扑 ----------
NCPU=$(nproc)
CPU_MODEL=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | xargs)
HYPERVISOR=$(lscpu 2>/dev/null | awk -F: '/Hypervisor/ {gsub(/^ +/,"",$2); print $2; exit}')
[[ -z "$HYPERVISOR" && "$(grep -c ^processor /proc/cpuinfo)" -eq 1 ]] && \
    grep -q hypervisor /proc/cpuinfo 2>/dev/null && HYPERVISOR="VM (hypervisor bit set)"

echo "=============================================="
echo " CPU 拓扑"
echo "=============================================="
echo "  逻辑 CPU 数: $NCPU"
echo "  型号:        $CPU_MODEL"
if [[ -n "$HYPERVISOR" ]]; then
    echo "  虚拟化:      $HYPERVISOR (VM 可见 vCPU 数由虚拟机配置决定，非物理核数)"
fi
if [[ "$NCPU" -eq 1 ]]; then
    echo ""
    echo "  ⚠ 当前 VM 仅 1 个 vCPU：进程只能跑在同一颗核上，"
    echo "    无法验证「多核分摊」。请在 VMware 里增加 vCPU 后重测。"
fi
echo ""

# ---------- 工具函数 ----------
proc_jiffies() {
    local pid=$1
    awk '{print $14 + $15}' "/proc/$pid/stat" 2>/dev/null || echo 0
}

proc_last_cpu() {
    local pid=$1
    awk '{print $39}' "/proc/$pid/stat" 2>/dev/null || echo "?"
}

read_cpu_line() {
    awk -v id="$1" '
        $1 == "cpu" && id == "all" { for (i=2; i<=NF; i++) s+=$i; print s; exit }
        $1 == ("cpu" id) { for (i=2; i<=NF; i++) s+=$i; print s; exit }
    ' /proc/stat
}

stop_all() {
    if [[ -f "$PID_FILE" ]]; then
        nginx -c "$NGINX_CONF" -s stop 2>/dev/null || true
        rm -f "$PID_FILE"
    fi
    for p in "${PORTS[@]}"; do
        pkill -f "${BIN} ${p} " 2>/dev/null || true
    done
    sleep 0.5
}

gen_nginx_conf() {
    local workers=$1
    [[ "$workers" -gt "$NCPU" && "$NCPU" -gt 0 ]] && workers=$NCPU
    [[ "$workers" -lt 1 ]] && workers=1

    cat >"$NGINX_CONF" <<EOF
pid        $PID_FILE;
error_log  $ERROR_LOG;

worker_processes $workers;
worker_rlimit_nofile 65535;

events {
    worker_connections 65535;
    use epoll;
    multi_accept on;
}

http {
    access_log off;

    upstream simple_http_backend {
        least_conn;
EOF
    for p in "${PORTS[@]}"; do
        echo "        server 127.0.0.1:${p};" >>"$NGINX_CONF"
    done
    cat >>"$NGINX_CONF" <<EOF
        keepalive 1024;
    }

    server {
        listen ${LB_PORT} reuseport;
        location / {
            proxy_pass http://simple_http_backend;
            proxy_http_version 1.1;
            proxy_set_header Host \$host;
            proxy_set_header Connection "";
            proxy_buffering off;
        }
    }
}
EOF
}

count_connections() {
    local port=$1
    ss -H -tan "sport = :${port}" 2>/dev/null | awk '$1 == "ESTAB" { c++ } END { print c + 0 }' || echo 0
}

# ---------- 启动服务 ----------
stop_all
gen_nginx_conf "$BACKENDS"

echo "==> 启动 ${BACKENDS} 个 backend (${PORTS[0]}-${PORTS[-1]})..."
declare -A BACKEND_PID
for p in "${PORTS[@]}"; do
    (cd "$BUILD" && "$BIN" "$p" "$WWW") &
    BACKEND_PID[$p]=$!
done
sleep 1

echo "==> 启动 Nginx LB (:${LB_PORT})..."
nginx -c "$NGINX_CONF"
sleep 0.5

# ---------- 压测 + 采样 ----------
WRK_URL="http://127.0.0.1:${LB_PORT}/"
echo ""
echo "=============================================="
echo " 压测: wrk -t${WRK_THREADS} -c${CONCURRENCY} -d${DURATION}s ${WRK_URL}"
echo " 采样: 每 ${SAMPLE_INTERVAL}s 记录进程 CPU / 运行核 / 连接数"
echo "=============================================="
echo ""

declare -A SUM_CPU SUM_CONN SAMPLE_COUNT CPU_AFFINITY
for p in "${PORTS[@]}"; do
    SUM_CPU[$p]=0
    SUM_CONN[$p]=0
    SAMPLE_COUNT[$p]=0
done

declare -A CORE_SUM
for ((c = 0; c < NCPU; c++)); do
    CORE_SUM[$c]=0
done

wrk -t"$WRK_THREADS" -c"$CONCURRENCY" -d"${DURATION}s" "$WRK_URL" >"$DEPLOY/wrk-lb-check.out" 2>&1 &
WRK_PID=$!

PREV_SYS=$(read_cpu_line all)
declare -A PREV_JIFFIES
for p in "${PORTS[@]}"; do
    PREV_JIFFIES[$p]=$(proc_jiffies "${BACKEND_PID[$p]}")
done

PREV_CORE=()
for ((c = 0; c < NCPU; c++)); do
    PREV_CORE[$c]=$(read_cpu_line "$c")
done

samples=0
for ((tick = 1; tick <= DURATION; tick++)); do
    sleep "$SAMPLE_INTERVAL"

    CURR_SYS=$(read_cpu_line all)
    SYS_DELTA=$((CURR_SYS - PREV_SYS))
    if [[ "$SYS_DELTA" -le 0 ]]; then
        PREV_SYS=$CURR_SYS
        continue
    fi

    wrk_alive="running"
    kill -0 "$WRK_PID" 2>/dev/null || wrk_alive="done"

    printf "── 采样 #%d (t=%ds, wrk %s) ──\n" "$((++samples))" "$tick" "$wrk_alive"

    for p in "${PORTS[@]}"; do
        pid="${BACKEND_PID[$p]}"
        curr_j=$(proc_jiffies "$pid")
        delta=$((curr_j - PREV_JIFFIES[$p]))
        PREV_JIFFIES[$p]=$curr_j

        cpu_pct=$(awk -v d="$delta" -v s="$SYS_DELTA" -v n="$NCPU" \
            'BEGIN { if (s > 0) printf "%.1f", d / s * 100 * n; else print "0.0" }')
        ps_cpu=$(ps -p "$pid" -o %cpu= 2>/dev/null | tr -d ' ' || echo "0")
        SUM_CPU[$p]=$(awk -v a="${SUM_CPU[$p]}" -v b="$cpu_pct" 'BEGIN { print a + b }')
        SAMPLE_COUNT[$p]=$((SAMPLE_COUNT[$p] + 1))

        last_cpu=$(proc_last_cpu "$pid")
        aff_key="${p}_${last_cpu}"
        CPU_AFFINITY[$aff_key]=$((${CPU_AFFINITY[$aff_key]:-0} + 1))

        conn=$(count_connections "$p")
        SUM_CONN[$p]=$((${SUM_CONN[$p]} + conn))

        printf "  port %-5s  PID %-6s  CPU %5s%% (ps %5s%%)  核 %s  ESTAB %3d\n" \
            "$p" "$pid" "$cpu_pct" "$ps_cpu" "$last_cpu" "$conn"
    done

    if [[ "$NCPU" -gt 1 ]]; then
        printf "  各核利用率:"
        for ((c = 0; c < NCPU; c++)); do
            curr_core=$(read_cpu_line "$c")
            core_delta=$((curr_core - PREV_CORE[$c]))
            PREV_CORE[$c]=$curr_core
            core_pct=$(awk -v d="$core_delta" -v s="$SYS_DELTA" \
                'BEGIN { if (s > 0) printf "%.0f", d / s * 100; else print "0" }')
            CORE_SUM[$c]=$((${CORE_SUM[$c]} + core_pct))
            printf " cpu%d=%s%%" "$c" "$core_pct"
        done
        echo ""
    fi

    PREV_SYS=$CURR_SYS
    echo ""
done

wait "$WRK_PID" 2>/dev/null || true

# ---------- 汇总 ----------
echo "=============================================="
echo " wrk 结果"
echo "=============================================="
grep -E 'Requests/sec|Latency|Socket errors' "$DEPLOY/wrk-lb-check.out" || cat "$DEPLOY/wrk-lb-check.out"

echo ""
echo "=============================================="
echo " 负载分布汇总（wrk 期间平均）"
echo "=============================================="

total_cpu=0
total_conn=0
for p in "${PORTS[@]}"; do
    avg_cpu=$(awk -v s="${SUM_CPU[$p]}" -v n="${SAMPLE_COUNT[$p]}" \
        'BEGIN { if (n > 0) printf "%.1f", s / n; else print "0.0" }')
    avg_conn=$(awk -v s="${SUM_CONN[$p]}" -v n="${SAMPLE_COUNT[$p]}" \
        'BEGIN { if (n > 0) printf "%.0f", s / n; else print "0" }')
    total_cpu=$(awk -v a="$total_cpu" -v b="$avg_cpu" 'BEGIN { print a + b }')
    total_conn=$((total_conn + avg_conn))
    printf "  port %-5s  平均 CPU %5s%%  平均 ESTAB %3s\n" "$p" "$avg_cpu" "$avg_conn"
done

echo ""
echo "  理想情况: 各 port 的 CPU/连接应接近 (${BACKENDS} 等分)"
if [[ "$BACKENDS" -gt 0 ]]; then
    expect_cpu=$(awk -v t="$total_cpu" -v n="$BACKENDS" 'BEGIN { printf "%.1f", t / n }')
    expect_conn=$(awk -v t="$total_conn" -v n="$BACKENDS" 'BEGIN { printf "%.0f", t / n }')
    echo "  期望均值:  CPU ~${expect_cpu}%   ESTAB ~${expect_conn}"
fi

if [[ "$NCPU" -gt 1 ]]; then
    echo ""
    echo "  各核平均利用率:"
    for ((c = 0; c < NCPU; c++)); do
        avg_core=$(awk -v s="${CORE_SUM[$c]}" -v n="$samples" \
            'BEGIN { if (n > 0) printf "%.0f", s / n; else print "0" }')
        printf "    cpu%d: %s%%\n" "$c" "$avg_core"
    done
else
    echo ""
    echo "  进程运行核分布（1 vCPU 时应全在 cpu0）:"
    for p in "${PORTS[@]}"; do
        aff=""
        for ((c = 0; c < NCPU; c++)); do
            aff_key="${p}_${c}"
            cnt=${CPU_AFFINITY[$aff_key]:-0}
            [[ "$cnt" -gt 0 ]] && aff+=" cpu${c}×${cnt}"
        done
        printf "    port %s:%s\n" "$p" "${aff:- (无采样)}"
    done
fi

# 简单判定
echo ""
echo "=============================================="
echo " 判定"
echo "=============================================="
if [[ "$NCPU" -eq 1 ]]; then
    echo "  [环境] 仅 1 vCPU → 无法验证多核分摊，只能看 Nginx 是否均匀分到各 backend 进程"
fi

if [[ "$BACKENDS" -gt 0 && "$samples" -gt 0 ]]; then
    max_conn=0
    min_conn=999999
    for p in "${PORTS[@]}"; do
        avg=$(awk -v s="${SUM_CONN[$p]}" -v n="${SAMPLE_COUNT[$p]}" 'BEGIN { print (n > 0) ? s / n : 0 }')
        awk -v v="$avg" -v max="$max_conn" 'BEGIN { if (v > max) exit 1; exit 0 }' || max_conn=$avg
        awk -v v="$avg" -v min="$min_conn" 'BEGIN { if (v < min) exit 1; exit 0 }' || min_conn=$avg
    done
    if awk -v max="$max_conn" -v min="$min_conn" 'BEGIN { exit (min > 0 && max / min < 1.5) ? 0 : 1 }'; then
        echo "  [backend 连接分布] 较均匀 (max/min ESTAB < 1.5×)"
    else
        echo "  [backend 连接分布] 不均匀 (max/min ESTAB ≥ 1.5×)"
    fi

    max_cpu=0
    min_cpu=999999
    for p in "${PORTS[@]}"; do
        avg=$(awk -v s="${SUM_CPU[$p]}" -v n="${SAMPLE_COUNT[$p]}" 'BEGIN { print (n > 0) ? s / n : 0 }')
        if awk -v v="$avg" 'BEGIN { exit (v > 0) ? 0 : 1 }'; then
            awk -v v="$avg" -v max="$max_cpu" 'BEGIN { if (v > max) exit 1; exit 0 }' || max_cpu=$avg
            awk -v v="$avg" -v min="$min_cpu" 'BEGIN { if (v < min) exit 1; exit 0 }' || min_cpu=$avg
        fi
    done
    if awk -v max="$max_cpu" -v min="$min_cpu" 'BEGIN { exit (min > 0 && max / min < 3) ? 0 : 1 }'; then
        echo "  [backend CPU 分布] 较均匀 (max/min CPU < 3×)"
    elif awk -v max="$max_cpu" 'BEGIN { exit (max > 0) ? 0 : 1 }'; then
        echo "  [backend CPU 分布] 不均匀 (max/min CPU ≥ 3×)"
    fi
fi

echo ""
echo "停止服务: pkill -f nginx-lb-check; 或重新运行前脚本会自动清理"
echo "wrk 完整输出: $DEPLOY/wrk-lb-check.out"
