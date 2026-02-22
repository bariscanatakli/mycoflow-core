#!/usr/bin/env bash
# MycoFlow Docker Bufferbloat Benchmark
#
# Waveform-style A-F grading: latency increase under load vs idle.
# Mimarisi: tek "mycoflow-lab" container, ip netns topolojisi içinde.
#
# Kullanım (host'tan):
#   docker compose -f docker-compose.bench.yml up --build -d
#   ./scripts/docker-bench.sh
#
# Ortam değişkenleri:
#   WAN_BW_KBIT    Simüle edilen WAN bant genişliği (varsayılan: 100000)
#   BENCH_DURATION Her senaryo süresi saniye (varsayılan: 20)
#   RESULTS_DIR    Çıktı dizini (varsayılan: ./results)

set -euo pipefail

# ── Konfigürasyon ──────────────────────────────────────────────────────────
LAB="mycoflow-lab"
SERVER_IP="10.0.1.2"
ROUTER_WAN_IP="10.0.1.1"
DURATION="${BENCH_DURATION:-20}"
WAN_BW_KBIT="${WAN_BW_KBIT:-100000}"
OUTDIR="${RESULTS_DIR:-$(dirname "$0")/../results}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTDIR=$(realpath "$OUTDIR")
mkdir -p "$OUTDIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()  { echo -e "${CYAN}[BENCH]${NC} $*"; }
pass()  { echo -e "${GREEN}[OK]${NC}   $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail()  { echo -e "${RED}[FAIL]${NC} $*"; }
title() { echo -e "\n${BOLD}══════════════════════════════════════════${NC}";
          echo -e "${BOLD}  $*${NC}";
          echo -e "${BOLD}══════════════════════════════════════════${NC}"; }

# ── Helpers ────────────────────────────────────────────────────────────────
# Container içinde client netns'de komut çalıştır
client() { docker exec "$LAB" ip netns exec client "$@"; }
# Container içinde router netns'de komut çalıştır
router() { docker exec "$LAB" ip netns exec router "$@"; }

# ── Grading (Waveform-style) ───────────────────────────────────────────────
grade_bufferbloat() {
    local inc="$1"
    local i
    i=$(python3 -c "import sys; print(int(float('$inc')))" 2>/dev/null || echo 999)
    if   [ "$i" -lt 5   ]; then echo "A"
    elif [ "$i" -lt 30  ]; then echo "B"
    elif [ "$i" -lt 60  ]; then echo "C"
    elif [ "$i" -lt 200 ]; then echo "D"
    else                         echo "F"
    fi
}

grade_label() {
    case "$1" in
        A|B) echo -e "${GREEN}$1${NC}" ;;
        C)   echo -e "${YELLOW}$1${NC}" ;;
        *)   echo -e "${RED}$1${NC}" ;;
    esac
}

# ── Prerequisite check ─────────────────────────────────────────────────────
check_prereqs() {
    info "Checking prerequisites..."
    if ! command -v docker &>/dev/null; then
        fail "docker not found"; exit 1
    fi
    if ! docker inspect "$LAB" --format '{{.State.Running}}' 2>/dev/null | grep -q true; then
        fail "Container $LAB is not running"
        fail "  docker compose -f docker-compose.bench.yml up --build -d"
        exit 1
    fi
    pass "Container $LAB running"

    # iperf3 server hazır mı?
    if ! client iperf3 -c "$SERVER_IP" -t 1 --connect-timeout 2000 \
            > /dev/null 2>&1; then
        warn "iperf3 server not responding — restarting..."
        docker exec "$LAB" sh -c \
            "ip netns exec server iperf3 -s --forceflush --daemon \
             --logfile /tmp/iperf3-server.log 2>/dev/null; sleep 2"
    fi

    if client iperf3 -c "$SERVER_IP" -t 1 --connect-timeout 2000 \
            > /dev/null 2>&1; then
        pass "iperf3 server at $SERVER_IP ready"
    else
        fail "iperf3 server unreachable"
        exit 1
    fi
}

# ── Scenario runner ────────────────────────────────────────────────────────
declare -A RES_GRADE RES_UP RES_DN RES_IDLE RES_LOADED RES_INC

run_scenario() {
    local name="$1"
    local label="$2"
    local setup_cmd="$3"    # ip netns exec router <tc komutları>

    title "Scenario: $label"

    # 1. tc konfigürasyonu uygula
    info "Configuring router tc..."
    docker exec "$LAB" bash -c "$setup_cmd" 2>&1 | sed "s/^/  [router] /"
    sleep 2   # qdisc yerleşmesini bekle

    # 2. Idle latency (yük altında değil)
    info "Measuring idle latency..."
    local idle_ms
    idle_ms=$(client ping -c 10 -i 0.2 "$SERVER_IP" 2>/dev/null \
        | awk -F'/' 'END{print $5}')
    idle_ms="${idle_ms:-999}"
    pass "  Idle: ${idle_ms} ms"

    # 3. Yük altında latency ölçümü
    # UDP blast ile link'i doldur (TCP congestion control bufferı gizleyebilir)
    local out_up="$OUTDIR/up_${name}_${TIMESTAMP}.json"
    local ping_out="$OUTDIR/ping_${name}_${TIMESTAMP}.txt"
    local udp_bw=$(( WAN_BW_KBIT * 3 ))  # 3x WAN BW → kesinlikle link'i satur

    info "Starting UDP saturation (${DURATION}s, ${udp_bw}kbit)..."
    docker exec -d "$LAB" sh -c \
        "ip netns exec client iperf3 -c $SERVER_IP -t $DURATION -u -b ${udp_bw}k -J \
         > /results/up_${name}_${TIMESTAMP}.json 2>/dev/null"

    sleep 2   # traffic ramp-up
    info "Measuring latency under load (${DURATION}s)..."
    local ping_count=$(( DURATION - 3 ))
    client ping -c "$ping_count" -i 0.2 "$SERVER_IP" \
        > "$ping_out" 2>/dev/null || true

    sleep $(( DURATION - 1 ))   # iperf3 bitişini bekle

    # 4. Sonuçları parse et
    local upload_mbps download_mbps loaded_ms increase_ms grade

    upload_mbps=$(python3 -c "
import json
try:
    d = json.load(open('$out_up'))
    bps = d.get('end',{}).get('sum',{}).get('bits_per_second', 0)
    print(f'{bps/1e6:.1f}')
except: print('0')
" 2>/dev/null || echo "0")
    download_mbps="$(python3 -c "print(f'{${WAN_BW_KBIT}/1000:.0f}')" 2>/dev/null || echo "N/A")M(cap)"

    loaded_ms=$(awk -F'/' 'END{print $5}' "$ping_out" 2>/dev/null || echo "999")
    loaded_ms="${loaded_ms:-999}"

    increase_ms=$(python3 -c "
try:
    v = float('$loaded_ms') - float('$idle_ms')
    print(f'{max(v,0):.1f}')
except: print('999')
" 2>/dev/null || echo "999")

    grade=$(grade_bufferbloat "$increase_ms")

    RES_GRADE["$name"]="$grade"
    RES_UP["$name"]="$upload_mbps"
    RES_DN["$name"]="$download_mbps"
    RES_IDLE["$name"]="$idle_ms"
    RES_LOADED["$name"]="$loaded_ms"
    RES_INC["$name"]="$increase_ms"

    pass "  Upload:   ${upload_mbps} Mbps"
    pass "  Download: ${download_mbps} Mbps"
    pass "  Idle lat: ${idle_ms} ms"
    pass "  Load lat: ${loaded_ms} ms"
    pass "  Increase: +${increase_ms} ms"
    echo -e "  ${BOLD}Grade: $(grade_label "$grade")${NC}"
}

# ── Scenario tc commands ────────────────────────────────────────────────────
# "del + add" yaklaşımı: mevcut qdisc hiyerarşisini sil, temizden kur.
# WAN iface: veth-wan (router netns'de sabit isim)

# 1. FIFO: büyük buffer → yük altında latency patlar
CMD_FIFO="
ip netns exec router bash -c '
pkill mycoflowd 2>/dev/null; sleep 1
tc qdisc del dev veth-wan root 2>/dev/null; true
tc qdisc add dev veth-wan root handle 1: htb default 10
tc class add dev veth-wan parent 1: classid 1:10 htb rate ${WAN_BW_KBIT}kbit burst 4mbit
tc qdisc add dev veth-wan parent 1:10 handle 10: pfifo limit 10000
echo FIFO/pfifo set: ${WAN_BW_KBIT}kbit pfifo-limit=10000
'
"

# 2. AQM: CAKE (OpenWrt) veya HTB+fq_codel (Docker/WSL2) → latency düşük
CMD_AQM="
ip netns exec router bash -c '
pkill mycoflowd 2>/dev/null; sleep 1
tc qdisc del dev veth-wan root 2>/dev/null; true
if tc qdisc add dev veth-wan root cake bandwidth ${WAN_BW_KBIT}kbit diffserv4 2>/dev/null; then
    echo AQM: CAKE ${WAN_BW_KBIT}kbit
else
    tc qdisc add dev veth-wan root handle 1: htb default 10
    tc class add dev veth-wan parent 1: classid 1:10 htb rate ${WAN_BW_KBIT}kbit burst 32kbit
    tc qdisc add dev veth-wan parent 1:10 handle 10: fq_codel target 5ms interval 100ms
    echo AQM: HTB+fq_codel ${WAN_BW_KBIT}kbit
fi
'
"

# 3. MycoFlow: AQM + mycoflowd adaptif kontrol
CMD_MYCOFLOW="
ip netns exec router bash -c '
tc qdisc del dev veth-wan root 2>/dev/null; true
tc qdisc add dev veth-wan root handle 1: htb default 10
tc class add dev veth-wan parent 1: classid 1:10 htb rate ${WAN_BW_KBIT}kbit burst 32kbit
tc qdisc add dev veth-wan parent 1:10 handle 10: fq_codel target 5ms interval 100ms
echo MycoFlow: HTB+fq_codel ready
MYCOFLOW_NO_TC=0 MYCOFLOW_WAN_IFACE=veth-wan MYCOFLOW_BW_KBIT=${WAN_BW_KBIT} \
MYCOFLOW_SAMPLE_HZ=2 mycoflowd &
sleep 6
echo mycoflowd adapting...
'
"

# ── Main ───────────────────────────────────────────────────────────────────
main() {
    title "MycoFlow Docker Bufferbloat Benchmark"
    info "Lab container:  $LAB"
    info "Server IP:      $SERVER_IP"
    info "WAN bandwidth:  ${WAN_BW_KBIT} kbit/s (simulated)"
    info "Duration:       ${DURATION}s per scenario"
    info "Output dir:     $OUTDIR"

    check_prereqs

    run_scenario "fifo"     "1. FIFO (baseline)"    "$CMD_FIFO"
    run_scenario "aqm"      "2. Static AQM"         "$CMD_AQM"
    run_scenario "mycoflow" "3. MycoFlow Adaptive"  "$CMD_MYCOFLOW"

    # ── Summary ────────────────────────────────────────────────────────────
    title "RESULTS SUMMARY"
    printf "%-22s %8s %8s %9s %9s %8s %7s\n" \
        "Scenario" "Upload" "Download" "Idle(ms)" "Load(ms)" "+Δ(ms)" "Grade"
    printf '%.0s─' {1..75}; echo

    for key in fifo aqm mycoflow; do
        local lbl
        case "$key" in
            fifo)     lbl="FIFO (baseline)" ;;
            aqm)      lbl="Static AQM" ;;
            mycoflow) lbl="MycoFlow" ;;
        esac
        printf "%-22s %7sMbps %7sMbps %9s %9s %8s %7s\n" \
            "$lbl" \
            "${RES_UP[$key]:-?}" \
            "${RES_DN[$key]:-?}" \
            "${RES_IDLE[$key]:-?}" \
            "${RES_LOADED[$key]:-?}" \
            "+${RES_INC[$key]:-?}" \
            "${RES_GRADE[$key]:-?}"
    done

    # JSON özet
    local jf="$OUTDIR/summary_${TIMESTAMP}.json"
    python3 - << PYEOF > "$jf"
import json
results = {}
for key, label in [("fifo","FIFO"),("aqm","Static AQM"),("mycoflow","MycoFlow")]:
    results[label] = {
        "upload_mbps":   "${RES_UP[fifo]:-0}",
        "download_mbps": "${RES_DN[fifo]:-0}",
        "idle_ms":       "${RES_IDLE[fifo]:-0}",
        "loaded_ms":     "${RES_LOADED[fifo]:-0}",
        "increase_ms":   "${RES_INC[fifo]:-0}",
        "grade":         "${RES_GRADE[fifo]:-?}",
    }
results["FIFO"] = {
    "upload_mbps":   "${RES_UP[fifo]:-0}",
    "download_mbps": "${RES_DN[fifo]:-0}",
    "idle_ms":       "${RES_IDLE[fifo]:-0}",
    "loaded_ms":     "${RES_LOADED[fifo]:-0}",
    "increase_ms":   "${RES_INC[fifo]:-0}",
    "grade":         "${RES_GRADE[fifo]:-?}",
}
results["Static AQM"] = {
    "upload_mbps":   "${RES_UP[aqm]:-0}",
    "download_mbps": "${RES_DN[aqm]:-0}",
    "idle_ms":       "${RES_IDLE[aqm]:-0}",
    "loaded_ms":     "${RES_LOADED[aqm]:-0}",
    "increase_ms":   "${RES_INC[aqm]:-0}",
    "grade":         "${RES_GRADE[aqm]:-?}",
}
results["MycoFlow"] = {
    "upload_mbps":   "${RES_UP[mycoflow]:-0}",
    "download_mbps": "${RES_DN[mycoflow]:-0}",
    "idle_ms":       "${RES_IDLE[mycoflow]:-0}",
    "loaded_ms":     "${RES_LOADED[mycoflow]:-0}",
    "increase_ms":   "${RES_INC[mycoflow]:-0}",
    "grade":         "${RES_GRADE[mycoflow]:-?}",
}
print(json.dumps({
    "benchmark":   "mycoflow-bufferbloat",
    "timestamp":   "${TIMESTAMP}",
    "wan_bw_kbit": ${WAN_BW_KBIT},
    "duration_s":  ${DURATION},
    "results":     results
}, indent=2))
PYEOF

    pass "Summary: $jf"
}

main "$@"
