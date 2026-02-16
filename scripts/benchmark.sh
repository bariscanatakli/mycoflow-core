#!/usr/bin/env bash
# MycoFlow Benchmark Suite
# Compares FIFO, static CAKE, and MycoFlow adaptive QoS.
#
# Usage: ./scripts/benchmark.sh [OPTIONS]
#   --iface IFACE       Network interface (default: eth0)
#   --server IP         iperf3 server address (default: 192.168.1.1)
#   --duration SEC      Per-test duration (default: 60)
#   --runs N            Number of runs per config (default: 3)
#   --output DIR        Output directory (default: results/)
#   --skip-iperf        Skip iperf3 tests
#   --skip-flent        Skip flent tests

set -euo pipefail

IFACE="eth0"
SERVER="192.168.1.1"
DURATION=60
RUNS=3
OUTDIR="results"
DO_IPERF=true
DO_FLENT=true

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${CYAN}[BENCH]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
pass() { echo -e "${GREEN}[OK]${NC} $1"; }

# ── Parse args ──────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case $1 in
        --iface)    IFACE="$2";    shift 2 ;;
        --server)   SERVER="$2";   shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --runs)     RUNS="$2";     shift 2 ;;
        --output)   OUTDIR="$2";   shift 2 ;;
        --skip-iperf) DO_IPERF=false; shift ;;
        --skip-flent) DO_FLENT=false; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$OUTDIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$OUTDIR/summary_${TIMESTAMP}.json"

info "MycoFlow Benchmark Suite"
info "Interface: $IFACE | Server: $SERVER | Duration: ${DURATION}s | Runs: $RUNS"

# ── Prerequisite check ──────────────────────────────────────
check_tool() {
    if ! command -v "$1" &>/dev/null; then
        warn "$1 not found. Install it or skip with --skip-$2"
        return 1
    fi
    return 0
}

# ── iperf3 Benchmark ────────────────────────────────────────
run_iperf3() {
    local label="$1"
    local run="$2"
    local outfile="$OUTDIR/iperf3_${label}_run${run}_${TIMESTAMP}.json"

    info "iperf3: $label (run $run/$RUNS)"
    iperf3 -c "$SERVER" -t "$DURATION" -J --bind-dev "$IFACE" > "$outfile" 2>/dev/null || {
        warn "iperf3 failed for $label run $run"
        return 1
    }

    # Extract key metrics from JSON
    local bps=$(python3 -c "
import json, sys
d = json.load(open('$outfile'))
end = d.get('end', {})
s = end.get('sum_sent', end.get('sum', {}))
print(f\"{s.get('bits_per_second', 0):.0f}\")
" 2>/dev/null || echo "0")
    
    pass "  throughput: $(echo "$bps" | awk '{printf "%.2f Mbps", $1/1e6}') → $outfile"
}

# ── flent Benchmark ─────────────────────────────────────────
run_flent() {
    local label="$1"
    local run="$2"
    local outfile="$OUTDIR/flent_${label}_run${run}_${TIMESTAMP}"

    info "flent RRUL: $label (run $run/$RUNS)"
    flent rrul -p all_scaled -l "$DURATION" -H "$SERVER" \
        -o "${outfile}.png" \
        --data-dir "$OUTDIR" \
        --note "$label run $run" 2>/dev/null || {
        warn "flent failed for $label run $run"
        return 1
    }
    pass "  flent output: ${outfile}.png"
}

# ── QoS Configurations ─────────────────────────────────────
setup_fifo() {
    info "Configuring: FIFO (no QoS)"
    tc qdisc replace dev "$IFACE" root pfifo_fast 2>/dev/null || true
}

setup_cake_static() {
    info "Configuring: Static CAKE (20Mbit)"
    tc qdisc replace dev "$IFACE" root cake bandwidth 20mbit 2>/dev/null || true
}

setup_mycoflow() {
    info "Configuring: MycoFlow Adaptive"
    # MycoFlow manages tc on its own, just make sure daemon is running
    if ! pgrep -x mycoflowd &>/dev/null; then
        warn "mycoflowd not running — start it first"
        return 1
    fi
    pass "mycoflowd is active"
}

# ── Main benchmark loop ────────────────────────────────────
CONFIGS=("fifo" "cake_static" "mycoflow")
echo '{"benchmark": "mycoflow", "timestamp": "'"$TIMESTAMP"'", "results": [' > "$SUMMARY"
FIRST=true

for config in "${CONFIGS[@]}"; do
    info "═══ Configuration: $config ═══"
    
    case $config in
        fifo)        setup_fifo ;;
        cake_static) setup_cake_static ;;
        mycoflow)    setup_mycoflow || continue ;;
    esac
    
    sleep 2  # Let qdisc settle

    for run in $(seq 1 "$RUNS"); do
        if $FIRST; then FIRST=false; else echo "," >> "$SUMMARY"; fi
        echo '  {"config": "'"$config"'", "run": '"$run"'' >> "$SUMMARY"
        
        if $DO_IPERF && check_tool iperf3 iperf; then
            run_iperf3 "$config" "$run"
            echo ', "iperf3_file": "iperf3_'"${config}_run${run}_${TIMESTAMP}"'.json"' >> "$SUMMARY"
        fi
        
        if $DO_FLENT && check_tool flent flent; then
            run_flent "$config" "$run"
            echo ', "flent_file": "flent_'"${config}_run${run}_${TIMESTAMP}"'"' >> "$SUMMARY"
        fi
        
        echo '}' >> "$SUMMARY"
    done
done

echo ']}' >> "$SUMMARY"

info "═══════════════════════════════════════════"
info "Benchmark complete! Summary: $SUMMARY"
info "Results directory: $OUTDIR/"
info "═══════════════════════════════════════════"
