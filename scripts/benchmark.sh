#!/usr/bin/env bash
# MycoFlow Benchmark Suite
# Compares FIFO, static CAKE, and MycoFlow adaptive QoS — egress and ingress.
#
# Usage: ./scripts/benchmark.sh [OPTIONS]
#   --iface IFACE         WAN interface for egress/ingress shaping (default: eth0)
#   --ifb IFACE           IFB interface for ingress shaping (default: ifb0)
#   --server IP           iperf3 server address (default: 192.168.1.1)
#   --duration SEC        Per-test duration (default: 60)
#   --runs N              Number of runs per config (default: 3)
#   --output DIR          Output directory (default: results/)
#   --ingress-bw KBIT     Ingress bandwidth kbit for static CAKE ingress test (default: 20000)
#   --skip-iperf          Skip iperf3 tests
#   --skip-flent          Skip flent tests
#   --skip-ingress        Skip ingress (download) tests

set -euo pipefail

IFACE="eth0"
IFB_IFACE="ifb0"
SERVER="192.168.1.1"
DURATION=60
RUNS=3
OUTDIR="results"
INGRESS_BW=20000
DO_IPERF=true
DO_FLENT=true
DO_INGRESS=true

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
        --iface)         IFACE="$2";        shift 2 ;;
        --ifb)           IFB_IFACE="$2";    shift 2 ;;
        --server)        SERVER="$2";       shift 2 ;;
        --duration)      DURATION="$2";     shift 2 ;;
        --runs)          RUNS="$2";         shift 2 ;;
        --output)        OUTDIR="$2";       shift 2 ;;
        --ingress-bw)    INGRESS_BW="$2";   shift 2 ;;
        --skip-iperf)    DO_IPERF=false;    shift ;;
        --skip-flent)    DO_FLENT=false;    shift ;;
        --skip-ingress)  DO_INGRESS=false;  shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

mkdir -p "$OUTDIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
SUMMARY="$OUTDIR/summary_${TIMESTAMP}.json"

info "MycoFlow Benchmark Suite"
info "Interface: $IFACE | IFB: $IFB_IFACE | Server: $SERVER | Duration: ${DURATION}s | Runs: $RUNS"

# ── Prerequisite check ──────────────────────────────────────
check_tool() {
    if ! command -v "$1" &>/dev/null; then
        warn "$1 not found. Install it or skip with --skip-$2"
        return 1
    fi
    return 0
}

# ── iperf3 Upload Benchmark (egress) ────────────────────────
run_iperf3_upload() {
    local label="$1"
    local run="$2"
    local outfile="$OUTDIR/iperf3_up_${label}_run${run}_${TIMESTAMP}.json"

    info "iperf3 upload: $label (run $run/$RUNS)"
    iperf3 -c "$SERVER" -t "$DURATION" -J --bind-dev "$IFACE" > "$outfile" 2>/dev/null || {
        warn "iperf3 upload failed for $label run $run"
        return 1
    }

    local bps
    bps=$(python3 -c "
import json
d = json.load(open('$outfile'))
s = d.get('end', {}).get('sum_sent', d.get('end', {}).get('sum', {}))
print(f\"{s.get('bits_per_second', 0):.0f}\")
" 2>/dev/null || echo "0")

    pass "  upload: $(echo "$bps" | awk '{printf "%.2f Mbps", $1/1e6}') → $outfile"
    echo "$outfile"
}

# ── iperf3 Download Benchmark (ingress via -R reverse mode) ─
run_iperf3_download() {
    local label="$1"
    local run="$2"
    local outfile="$OUTDIR/iperf3_dl_${label}_run${run}_${TIMESTAMP}.json"

    info "iperf3 download: $label (run $run/$RUNS)"
    iperf3 -c "$SERVER" -t "$DURATION" -R -J --bind-dev "$IFACE" > "$outfile" 2>/dev/null || {
        warn "iperf3 download failed for $label run $run"
        return 1
    }

    local bps
    bps=$(python3 -c "
import json
d = json.load(open('$outfile'))
s = d.get('end', {}).get('sum_received', d.get('end', {}).get('sum', {}))
print(f\"{s.get('bits_per_second', 0):.0f}\")
" 2>/dev/null || echo "0")

    pass "  download: $(echo "$bps" | awk '{printf "%.2f Mbps", $1/1e6}') → $outfile"
    echo "$outfile"
}

# ── iperf3 Latency-under-load (simultaneous up+down) ────────
run_iperf3_bidir() {
    local label="$1"
    local run="$2"
    local outfile="$OUTDIR/iperf3_bidir_${label}_run${run}_${TIMESTAMP}.json"

    info "iperf3 bidir: $label (run $run/$RUNS)"
    # --bidir requires iperf3 >= 3.7; fall back gracefully
    iperf3 -c "$SERVER" -t "$DURATION" --bidir -J --bind-dev "$IFACE" > "$outfile" 2>/dev/null || {
        warn "iperf3 bidir failed for $label run $run (iperf3 < 3.7?), skipping"
        return 1
    }
    pass "  bidir output → $outfile"
    echo "$outfile"
}

# ── flent RRUL (upload + download + latency) ─────────────────
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
    echo "${outfile}.png"
}

# ── QoS Configuration helpers ───────────────────────────────

setup_fifo() {
    info "Configuring: FIFO (no QoS, egress only)"
    tc qdisc replace dev "$IFACE" root pfifo_fast 2>/dev/null || true
}

setup_cake_static_egress() {
    info "Configuring: Static CAKE egress (${INGRESS_BW}kbit)"
    tc qdisc replace dev "$IFACE" root cake bandwidth "${INGRESS_BW}kbit" 2>/dev/null || true
}

setup_cake_static_ingress() {
    info "Configuring: Static CAKE ingress via IFB (${INGRESS_BW}kbit)"
    # IFB plumbing
    ip link add "$IFB_IFACE" type ifb 2>/dev/null || true
    ip link set "$IFB_IFACE" up
    tc qdisc add dev "$IFACE" handle ffff: ingress 2>/dev/null || true
    tc filter add dev "$IFACE" parent ffff: protocol all u32 match u32 0 0 \
        action mirred egress redirect dev "$IFB_IFACE" 2>/dev/null || true
    tc qdisc replace dev "$IFB_IFACE" root cake bandwidth "${INGRESS_BW}kbit" 2>/dev/null || true
}

teardown_ingress() {
    info "Tearing down ingress IFB"
    tc filter del dev "$IFACE" parent ffff: 2>/dev/null || true
    tc qdisc del dev "$IFACE" ingress 2>/dev/null || true
    ip link set "$IFB_IFACE" down 2>/dev/null || true
    ip link del "$IFB_IFACE" 2>/dev/null || true
}

setup_mycoflow() {
    info "Configuring: MycoFlow Adaptive (egress)"
    if ! pgrep -x mycoflowd &>/dev/null; then
        warn "mycoflowd not running — start it first"
        return 1
    fi
    pass "mycoflowd is active"
}

setup_mycoflow_ingress() {
    info "Configuring: MycoFlow Adaptive (egress + ingress IFB)"
    if ! pgrep -x mycoflowd &>/dev/null; then
        warn "mycoflowd not running — start it first"
        return 1
    fi
    # Verify IFB device is up (mycoflowd sets it up if ingress_enabled=1)
    if ! ip link show "$IFB_IFACE" &>/dev/null; then
        warn "IFB device $IFB_IFACE not found — ensure MYCOFLOW_INGRESS=1 in daemon config"
        return 1
    fi
    pass "mycoflowd active with IFB ingress on $IFB_IFACE"
}

# ── Main benchmark loop ────────────────────────────────────
EGRESS_CONFIGS=("fifo" "cake_static_egress" "mycoflow_egress")
INGRESS_CONFIGS=("cake_static_ingress" "mycoflow_ingress")

echo '{"benchmark": "mycoflow", "timestamp": "'"$TIMESTAMP"'", "ingress_bw_kbit": '"$INGRESS_BW"', "results": [' > "$SUMMARY"
FIRST=true

# ── Egress (upload) tests ─────────────────────────────────
info "════════════ EGRESS (UPLOAD) TESTS ════════════"
for config in "${EGRESS_CONFIGS[@]}"; do
    info "═══ Configuration: $config ═══"

    case $config in
        fifo)              setup_fifo ;;
        cake_static_egress) setup_cake_static_egress ;;
        mycoflow_egress)   setup_mycoflow || continue ;;
    esac

    sleep 2  # Let qdisc settle

    for run in $(seq 1 "$RUNS"); do
        if $FIRST; then FIRST=false; else echo "," >> "$SUMMARY"; fi
        printf '  {"config": "%s", "direction": "egress", "run": %d' "$config" "$run" >> "$SUMMARY"

        if $DO_IPERF && check_tool iperf3 iperf; then
            up_file=$(run_iperf3_upload "$config" "$run" || true)
            [[ -n "$up_file" ]] && printf ', "iperf3_upload": "%s"' "$(basename "$up_file")" >> "$SUMMARY"
            bidir_file=$(run_iperf3_bidir "$config" "$run" || true)
            [[ -n "$bidir_file" ]] && printf ', "iperf3_bidir": "%s"' "$(basename "$bidir_file")" >> "$SUMMARY"
        fi

        if $DO_FLENT && check_tool flent flent; then
            fl_file=$(run_flent "${config}_egress" "$run" || true)
            [[ -n "$fl_file" ]] && printf ', "flent": "%s"' "$(basename "$fl_file")" >> "$SUMMARY"
        fi

        echo '}' >> "$SUMMARY"
    done
done

# ── Ingress (download) tests ──────────────────────────────
if $DO_INGRESS; then
    info "════════════ INGRESS (DOWNLOAD) TESTS ════════════"
    for config in "${INGRESS_CONFIGS[@]}"; do
        info "═══ Configuration: $config ═══"

        case $config in
            cake_static_ingress) setup_cake_static_ingress ;;
            mycoflow_ingress)    setup_mycoflow_ingress || continue ;;
        esac

        sleep 2  # Let qdisc settle

        for run in $(seq 1 "$RUNS"); do
            echo "," >> "$SUMMARY"
            printf '  {"config": "%s", "direction": "ingress", "run": %d' "$config" "$run" >> "$SUMMARY"

            if $DO_IPERF && check_tool iperf3 iperf; then
                dl_file=$(run_iperf3_download "$config" "$run" || true)
                [[ -n "$dl_file" ]] && printf ', "iperf3_download": "%s"' "$(basename "$dl_file")" >> "$SUMMARY"
                bidir_file=$(run_iperf3_bidir "$config" "$run" || true)
                [[ -n "$bidir_file" ]] && printf ', "iperf3_bidir": "%s"' "$(basename "$bidir_file")" >> "$SUMMARY"
            fi

            if $DO_FLENT && check_tool flent flent; then
                fl_file=$(run_flent "${config}_ingress" "$run" || true)
                [[ -n "$fl_file" ]] && printf ', "flent": "%s"' "$(basename "$fl_file")" >> "$SUMMARY"
            fi

            echo '}' >> "$SUMMARY"
        done

        # Clean up ingress plumbing between static and mycoflow configs
        if [[ "$config" == "cake_static_ingress" ]]; then
            teardown_ingress
        fi
    done
fi

echo ']}' >> "$SUMMARY"

info "═══════════════════════════════════════════"
info "Benchmark complete! Summary: $SUMMARY"
info "Results directory: $OUTDIR/"
info "═══════════════════════════════════════════"
