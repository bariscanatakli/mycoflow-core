#!/usr/bin/env bash
# MycoFlow QEMU OpenWrt Per-Device QoS Benchmark
#
# Gerçek OpenWrt ortamında 3 senaryo karşılaştırması:
#   1. FIFO (baseline) — büyük buffer, bufferbloat patlar
#   2. CAKE only       — diffserv4 ama DSCP marking yok → hepsi Best Effort
#   3. CAKE + MycoFlow — per-device DSCP: gamer=CS4 (Video tin), bulk=CS1 (Bulk tin)
#
# Ölçüm: bulk client WAN'ı doldururken, gamer client'ın ping latency'si.
# Fark: Senaryo 2 vs 3 → MycoFlow gerçekten gaming latency'yi iyileştiriyor mu?
#
# Kullanım:
#   docker compose -f docker-compose.qemu.yml up --build -d
#   ./scripts/qemu-bench.sh

set -euo pipefail

# ── Konfigürasyon ──────────────────────────────────────────────────────────
LAB="mycoflow-qemu"
SERVER_IP="10.0.1.2"
OPENWRT_IP="192.168.1.1"  # SSH via LAN (OpenWrt default br-lan)
OPENWRT_WAN="eth1"        # OpenWrt WAN interface (second NIC)
DURATION="${BENCH_DURATION:-30}"
WAN_BW_KBIT="${WAN_BW_KBIT:-20000}"
OUTDIR="${RESULTS_DIR:-$(dirname "$0")/../results}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${CYAN}[BENCH]${NC} $*"; }
pass()  { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }
title() { echo -e "\n${BOLD}══════════════════════════════════════════════════════${NC}";
          echo -e "${BOLD}  $*${NC}";
          echo -e "${BOLD}══════════════════════════════════════════════════════${NC}"; }

# ── Helpers ────────────────────────────────────────────────────────────────
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# OpenWrt VM'de komut çalıştır
openwrt() { docker exec "$LAB" sshpass -p '' ssh $SSH_OPTS root@"$OPENWRT_IP" "$@"; }

# Netns'lerde komut çalıştır
gamer()  { docker exec "$LAB" ip netns exec gamer "$@"; }
bulk()   { docker exec "$LAB" ip netns exec bulk "$@"; }
server() { docker exec "$LAB" ip netns exec server "$@"; }

# Arka planda komut çalıştır (docker exec -d)
gamer_bg()  { docker exec -d "$LAB" ip netns exec gamer "$@"; }
bulk_bg()   { docker exec -d "$LAB" ip netns exec bulk "$@"; }

# ── Grading (Waveform-style) ──────────────────────────────────────────────
grade_bufferbloat() {
    local inc="$1"
    local i
    i=$(python3 -c "import sys; print(int(float('$inc')))" 2>/dev/null || echo 999)
    if   [ "$i" -lt 5   ]; then echo "A+"
    elif [ "$i" -lt 15  ]; then echo "A"
    elif [ "$i" -lt 30  ]; then echo "B"
    elif [ "$i" -lt 60  ]; then echo "C"
    elif [ "$i" -lt 200 ]; then echo "D"
    else                         echo "F"
    fi
}

grade_label() {
    case "$1" in
        A+|A|B) echo -e "${GREEN}$1${NC}" ;;
        C)      echo -e "${YELLOW}$1${NC}" ;;
        *)      echo -e "${RED}$1${NC}" ;;
    esac
}

# ── Prerequisite check ───────────────────────────────────────────────────
check_prereqs() {
    info "Checking prerequisites..."

    if ! docker inspect "$LAB" --format '{{.State.Running}}' 2>/dev/null | grep -q true; then
        fail "Container $LAB is not running"
        fail "  docker compose -f docker-compose.qemu.yml up --build -d"
        exit 1
    fi
    pass "Container $LAB running"

    # OpenWrt SSH erişimi
    if ! openwrt "echo ok" > /dev/null 2>&1; then
        fail "Cannot SSH into OpenWrt VM"
        exit 1
    fi
    pass "OpenWrt VM reachable via SSH"

    # CAKE kontrolü
    local cake_check
    cake_check=$(openwrt "tc qdisc add dev lo root cake bandwidth 1000kbit 2>/dev/null && tc qdisc del dev lo root 2>/dev/null && echo OK || echo FAIL")
    if echo "$cake_check" | grep -q "OK"; then
        pass "CAKE qdisc available"
    else
        fail "CAKE qdisc NOT available — per-device test impossible"
        exit 1
    fi

    # Cleanup stale iperf3 from previous runs, then ensure servers are running
    info "Cleaning up stale processes..."
    docker exec "$LAB" sh -c "ip netns exec gamer killall iperf3 2>/dev/null; ip netns exec bulk killall iperf3 2>/dev/null; true"
    docker exec "$LAB" sh -c "\
        ip netns exec server killall iperf3 2>/dev/null; sleep 1; \
        ip netns exec server iperf3 -s --forceflush --daemon --logfile /tmp/iperf3-server.log; \
        ip netns exec server iperf3 -s -p 5202 --forceflush --daemon --logfile /tmp/iperf3-5202.log; \
        ip netns exec server iperf3 -s -p 5203 --forceflush --daemon --logfile /tmp/iperf3-5203.log; \
        ip netns exec server iperf3 -s -p 5204 --forceflush --daemon --logfile /tmp/iperf3-5204.log; \
        sleep 1"
    # Also kill any leftover mycoflowd
    openwrt "killall mycoflowd 2>/dev/null; true" 2>/dev/null || true

    if gamer iperf3 -c "$SERVER_IP" -t 1 --connect-timeout 3000 > /dev/null 2>&1; then
        pass "iperf3 server at $SERVER_IP reachable from gamer"
    else
        fail "iperf3 server unreachable from gamer"
        exit 1
    fi

    if bulk iperf3 -c "$SERVER_IP" -t 1 --connect-timeout 3000 > /dev/null 2>&1; then
        pass "iperf3 server at $SERVER_IP reachable from bulk"
    else
        fail "iperf3 server unreachable from bulk"
        exit 1
    fi
}

# ── Scenario runner ──────────────────────────────────────────────────────
declare -A RES_GRADE RES_IDLE RES_LOADED RES_INC

run_scenario() {
    local name="$1"
    local label="$2"
    local setup_fn="$3"

    title "Scenario: $label"

    # 1. mycoflowd'yi durdur (varsa)
    openwrt "killall mycoflowd 2>/dev/null; sleep 1" 2>/dev/null || true

    # 2. Senaryo setup'ını çalıştır
    $setup_fn

    # 3. Idle latency ölç (yük yok)
    info "Measuring idle latency (gamer → server)..."
    local idle_ms
    idle_ms=$(gamer ping -c 20 -i 0.2 "$SERVER_IP" 2>/dev/null \
        | awk -F'/' 'END{print $5}')
    idle_ms="${idle_ms:-999}"
    pass "  Idle: ${idle_ms} ms"

    # 4. Bulk iperf3 başlat (UDP saturation)
    local udp_bw=$(( WAN_BW_KBIT * 3 ))
    info "Starting bulk UDP saturation (${DURATION}s, ${udp_bw}kbit)..."
    bulk_bg iperf3 -c "$SERVER_IP" -t "$DURATION" -u -b "${udp_bw}k" \
        --logfile /dev/null 2>/dev/null

    # Traffic ramp-up bekle
    sleep 4

    # 5. Gamer latency ölç (yük altında)
    local ping_count=$(( DURATION - 6 ))
    info "Measuring gamer latency under load (${ping_count} pings)..."
    local ping_out="/tmp/ping_${name}_${TIMESTAMP}.txt"
    docker exec "$LAB" sh -c \
        "ip netns exec gamer ping -c $ping_count -i 0.5 $SERVER_IP" \
        > "$OUTDIR/ping_${name}_${TIMESTAMP}.txt" 2>/dev/null || true

    # iperf3 bitişini bekle
    sleep 3

    # 6. Sonuçları parse et
    local loaded_ms increase_ms grade
    loaded_ms=$(awk -F'/' 'END{print $5}' "$OUTDIR/ping_${name}_${TIMESTAMP}.txt" 2>/dev/null || echo "999")
    loaded_ms="${loaded_ms:-999}"

    increase_ms=$(python3 -c "
try:
    v = float('$loaded_ms') - float('$idle_ms')
    print(f'{max(v,0):.1f}')
except: print('999')
" 2>/dev/null || echo "999")

    grade=$(grade_bufferbloat "$increase_ms")

    RES_GRADE["$name"]="$grade"
    RES_IDLE["$name"]="$idle_ms"
    RES_LOADED["$name"]="$loaded_ms"
    RES_INC["$name"]="$increase_ms"

    pass "  Idle lat:    ${idle_ms} ms"
    pass "  Loaded lat:  ${loaded_ms} ms"
    pass "  Increase:    +${increase_ms} ms"
    echo -e "  ${BOLD}Grade: $(grade_label "$grade")${NC}"
}

# ── Scenario setup functions ──────────────────────────────────────────────

setup_fifo() {
    info "Setting FIFO qdisc on OpenWrt WAN (pfifo limit=1000)..."
    openwrt "
tc qdisc del dev $OPENWRT_WAN root 2>/dev/null; true
tc qdisc add dev $OPENWRT_WAN root handle 1: htb default 10
tc class add dev $OPENWRT_WAN parent 1: classid 1:10 htb rate ${WAN_BW_KBIT}kbit burst 4mbit
tc qdisc add dev $OPENWRT_WAN parent 1:10 handle 10: pfifo limit 1000
echo 'FIFO set: ${WAN_BW_KBIT}kbit pfifo limit=1000'
"
    sleep 2
}

setup_cake() {
    info "Setting CAKE diffserv4 on OpenWrt WAN (no DSCP marking)..."
    openwrt "
tc qdisc del dev $OPENWRT_WAN root 2>/dev/null; true
tc qdisc add dev $OPENWRT_WAN root cake bandwidth ${WAN_BW_KBIT}kbit diffserv4
echo 'CAKE diffserv4 set: ${WAN_BW_KBIT}kbit'
tc qdisc show dev $OPENWRT_WAN
"
    sleep 2
}

setup_mycoflow() {
    info "Setting CAKE diffserv4 + MycoFlow per-device on OpenWrt WAN..."
    openwrt "
tc qdisc del dev $OPENWRT_WAN root 2>/dev/null; true
tc qdisc add dev $OPENWRT_WAN root cake bandwidth ${WAN_BW_KBIT}kbit diffserv4
echo 'CAKE diffserv4 set: ${WAN_BW_KBIT}kbit'
"

    # mycoflowd'yi başlat (per-device DSCP marking etkin)
    info "Starting mycoflowd with per-device DSCP..."
    openwrt "
MYCOFLOW_NO_TC=0 \
MYCOFLOW_DUMMY=0 \
MYCOFLOW_BW_KBIT=${WAN_BW_KBIT} \
MYCOFLOW_EGRESS_IFACE=$OPENWRT_WAN \
MYCOFLOW_PER_DEVICE=1 \
MYCOFLOW_PROBE_HOST=${SERVER_IP} \
MYCOFLOW_SAMPLE_HZ=2 \
MYCOFLOW_LOG_LEVEL=3 \
MYCOFLOW_BASELINE_SAMPLES=3 \
/usr/bin/mycoflowd > /tmp/mycoflowd.log 2>&1 &
echo 'mycoflowd started (PID: '\$(pgrep mycoflowd)')'
"

    # Persona detection'ın stabilize olması için trafik üret
    # 3+ vote gerek, 2 Hz sample rate → ~2s minimum; 20s güvenli margin
    # Trafik MEASUREMENT sırasında da devam etmeli — conntrack entries canlı kalmalı
    local WARMUP_S=60  # warm-up + measurement süresince devam edecek

    info "Warming up: generating traffic patterns for persona detection..."

    # Gamer: küçük paketlerle gaming-like traffic (birden fazla flow)
    # UDP, 64 byte paketler, düşük rate — interactive pattern
    gamer_bg iperf3 -c "$SERVER_IP" -t "$WARMUP_S" -u -b 100k -l 64 --logfile /dev/null 2>/dev/null
    gamer_bg iperf3 -c "$SERVER_IP" -t "$WARMUP_S" -u -b 50k -l 64 -p 5202 --logfile /dev/null 2>/dev/null
    gamer_bg iperf3 -c "$SERVER_IP" -t "$WARMUP_S" -u -b 50k -l 64 -p 5203 --logfile /dev/null 2>/dev/null

    # Bulk: büyük paketlerle download traffic (elephant flow)
    # TCP, varsayılan MSS=1448 → large packets → BULK persona
    # Port 5204 kullan — 5201 measurement bulk için ayrılmalı
    bulk_bg iperf3 -c "$SERVER_IP" -t "$WARMUP_S" -p 5204 --logfile /dev/null 2>/dev/null

    info "Waiting 20s for persona stabilization..."
    sleep 20

    # Persona durumunu kontrol et
    info "Checking persona detection results..."
    openwrt "cat /tmp/myco_state.json 2>/dev/null" | python3 -m json.tool 2>/dev/null || true
    openwrt "iptables -t mangle -L mycoflow_dscp -n -v 2>/dev/null" || true
}

# ── CAKE tin statistics ──────────────────────────────────────────────────
dump_cake_stats() {
    local label="$1"
    info "CAKE tin statistics ($label):"
    openwrt "tc -s qdisc show dev $OPENWRT_WAN" 2>/dev/null | head -40
}

# ── Main ──────────────────────────────────────────────────────────────────
main() {
    title "MycoFlow QEMU OpenWrt Per-Device QoS Benchmark"
    info "Lab container:  $LAB"
    info "OpenWrt VM:     $OPENWRT_IP"
    info "Server:         $SERVER_IP"
    info "WAN bandwidth:  ${WAN_BW_KBIT} kbit/s"
    info "Duration:       ${DURATION}s per scenario"
    info "Output dir:     $OUTDIR"

    check_prereqs

    # ── Scenario 1: FIFO (baseline) ──
    run_scenario "fifo" "1. FIFO — No QoS (baseline)" setup_fifo

    # ── Scenario 2: CAKE only ──
    run_scenario "cake" "2. CAKE diffserv4 — Static AQM (no DSCP)" setup_cake
    dump_cake_stats "after CAKE-only test"

    # ── Scenario 3: CAKE + MycoFlow ──
    run_scenario "mycoflow" "3. CAKE + MycoFlow Per-Device DSCP" setup_mycoflow
    dump_cake_stats "after MycoFlow test"

    # mycoflowd durumunu kaydet
    info "Final MycoFlow state:"
    openwrt "cat /tmp/myco_state.json 2>/dev/null" > "$OUTDIR/myco_state_${TIMESTAMP}.json" 2>/dev/null || true
    openwrt "iptables -t mangle -L mycoflow_dscp -n -v 2>/dev/null" > "$OUTDIR/dscp_rules_${TIMESTAMP}.txt" 2>/dev/null || true
    openwrt "killall mycoflowd 2>/dev/null" || true

    # ── Summary ──────────────────────────────────────────────────────────
    title "RESULTS SUMMARY"
    echo ""
    printf "  %-35s %9s %9s %8s %7s\n" \
        "Scenario" "Idle(ms)" "Load(ms)" "+Δ(ms)" "Grade"
    printf '  %.0s─' {1..72}; echo

    for key in fifo cake mycoflow; do
        local lbl
        case "$key" in
            fifo)     lbl="1. FIFO (no QoS)" ;;
            cake)     lbl="2. CAKE diffserv4 (no DSCP)" ;;
            mycoflow) lbl="3. CAKE + MycoFlow per-device" ;;
        esac
        printf "  %-35s %9s %9s %8s %7s\n" \
            "$lbl" \
            "${RES_IDLE[$key]:-?}" \
            "${RES_LOADED[$key]:-?}" \
            "+${RES_INC[$key]:-?}" \
            "${RES_GRADE[$key]:-?}"
    done

    echo ""

    # MycoFlow vs CAKE-only farkı
    local cake_inc="${RES_INC[cake]:-999}"
    local myco_inc="${RES_INC[mycoflow]:-999}"
    local improvement
    improvement=$(python3 -c "
ci = float('$cake_inc')
mi = float('$myco_inc')
if ci > 0:
    pct = ((ci - mi) / ci) * 100
    print(f'{pct:.0f}')
else:
    print('0')
" 2>/dev/null || echo "?")
    echo -e "  ${BOLD}MycoFlow vs CAKE-only: ${improvement}% latency improvement${NC}"
    echo ""

    # JSON summary
    local jf="$OUTDIR/qemu_summary_${TIMESTAMP}.json"
    python3 - << PYEOF > "$jf"
import json
print(json.dumps({
    "benchmark": "mycoflow-qemu-per-device",
    "timestamp": "${TIMESTAMP}",
    "wan_bw_kbit": ${WAN_BW_KBIT},
    "duration_s": ${DURATION},
    "improvement_pct": "$improvement",
    "results": {
        "FIFO": {
            "idle_ms": "${RES_IDLE[fifo]:-0}",
            "loaded_ms": "${RES_LOADED[fifo]:-0}",
            "increase_ms": "${RES_INC[fifo]:-0}",
            "grade": "${RES_GRADE[fifo]:-?}"
        },
        "CAKE only": {
            "idle_ms": "${RES_IDLE[cake]:-0}",
            "loaded_ms": "${RES_LOADED[cake]:-0}",
            "increase_ms": "${RES_INC[cake]:-0}",
            "grade": "${RES_GRADE[cake]:-?}"
        },
        "CAKE + MycoFlow": {
            "idle_ms": "${RES_IDLE[mycoflow]:-0}",
            "loaded_ms": "${RES_LOADED[mycoflow]:-0}",
            "increase_ms": "${RES_INC[mycoflow]:-0}",
            "grade": "${RES_GRADE[mycoflow]:-?}"
        }
    }
}, indent=2))
PYEOF

    pass "JSON summary: $jf"
    pass "Ping logs: $OUTDIR/ping_*_${TIMESTAMP}.txt"
    pass "DSCP rules: $OUTDIR/dscp_rules_${TIMESTAMP}.txt"
    pass "MycoFlow state: $OUTDIR/myco_state_${TIMESTAMP}.json"
}

main "$@"
