#!/usr/bin/env bash
# MycoFlow Cross-Traffic Bufferbloat Benchmark
#
# Proves CAKE diffserv4 tin separation by running a saturating upload
# (iperf3 TCP) while measuring RTT of two probes to 8.8.8.8:
#   1. default tin (no DSCP) — competes with bulk upload in Best-Effort
#   2. Voice tin (DSCP EF)   — should be protected even under saturation
#
# The differential between the two probes under load is the diffserv4
# benefit. MycoFlow's role is to auto-classify LAN devices into these
# tins via iptables mangle — this benchmark uses an explicit OUTPUT
# mangle rule on the router itself, since busybox ping lacks -Q support.
#
# Usage: ./scripts/cross-bench.sh [OPTIONS]
#   --router IP        default 10.10.1.1
#   --pass PASS        or set ROUTER_PASS
#   --iperf-server H   default ping.online.net
#   --probe IP         default 8.8.8.8
#   --load-secs N      iperf3 duration (default 30)
#   --probe-secs N     ping duration (default 20)
#   --output DIR       default results

set -euo pipefail

ROUTER="10.10.1.1"
USER="root"
PASS="${ROUTER_PASS:-}"
IPERF_SERVER="ping.online.net"
IPERF_PORT="5202"
PROBE="8.8.8.8"
LOAD_SECS=30
PROBE_SECS=20
OUTDIR="results"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYAN}[XBENCH]${NC} $1" >&2; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
pass() { echo -e "${GREEN}[OK]${NC} $1" >&2; }
fail() { echo -e "${RED}[FAIL]${NC} $1" >&2; }

while [[ $# -gt 0 ]]; do
    case $1 in
        --router)        ROUTER="$2";        shift 2 ;;
        --pass)          PASS="$2";          shift 2 ;;
        --iperf-server)  IPERF_SERVER="$2";  shift 2 ;;
        --probe)         PROBE="$2";         shift 2 ;;
        --load-secs)     LOAD_SECS="$2";     shift 2 ;;
        --probe-secs)    PROBE_SECS="$2";    shift 2 ;;
        --output)        OUTDIR="$2";        shift 2 ;;
        -h|--help)       sed -n '3,22p' "$0"; exit 0 ;;
        *) echo "Unknown: $1"; exit 1 ;;
    esac
done

[[ -z "$PASS" ]] && { fail "No password. Set ROUTER_PASS or --pass"; exit 1; }
mkdir -p "$OUTDIR"
TS=$(date +%Y%m%d_%H%M%S)
JSON="$OUTDIR/cross_${TS}.json"

# ── SSH with ControlMaster ────────────────────────────
SSH_CTRL="/tmp/mycoflow-xbench-${TS}.sock"
ASKPASS_BIN="$(mktemp)"
cat > "$ASKPASS_BIN" <<EOF
#!/usr/bin/env bash
echo "$PASS"
EOF
chmod +x "$ASKPASS_BIN"
export SSH_ASKPASS="$ASKPASS_BIN" SSH_ASKPASS_REQUIRE=force DISPLAY=:0
setsid -w ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
              -o ControlMaster=yes -o ControlPath="$SSH_CTRL" \
              -o ControlPersist=10m -N -f "$USER@$ROUTER" 2>/dev/null
cleanup() {
    rm -f "$ASKPASS_BIN" 2>/dev/null
    ssh -o ControlPath="$SSH_CTRL" -O exit "$USER@$ROUTER" 2>/dev/null || true
    # Ensure any OUTPUT mangle rule we added gets removed
    rssh "iptables -t mangle -D OUTPUT -d $PROBE -p icmp -j DSCP --set-dscp 0x2e 2>/dev/null; true" >/dev/null 2>&1 || true
}
trap cleanup EXIT
rssh() {
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o ControlPath="$SSH_CTRL" "$USER@$ROUTER" "$@"
}

# ── Preflight ──────────────────────────────────────────
info "Preflight: SSH to $ROUTER"
rssh 'uname -a' >/dev/null || { fail "SSH fail"; exit 2; }
info "Preflight: iperf3 to $IPERF_SERVER:$IPERF_PORT (retry up to 4 ports)"
IPERF_OK=""
for p in $IPERF_PORT 5201 5203 5204 5205 5209; do
    if rssh "iperf3 -c $IPERF_SERVER -p $p -t 3 2>&1 | grep -q 'Mbits/sec'"; then
        IPERF_PORT="$p"; IPERF_OK=1
        pass "iperf3 $IPERF_SERVER:$p reachable"
        break
    fi
    warn "  port $p busy, trying next"
done
[[ -z "$IPERF_OK" ]] && { fail "no iperf3 port reachable on $IPERF_SERVER"; exit 3; }
info "Preflight: CAKE on pppoe-wan"
rssh 'tc qdisc show dev pppoe-wan | head -1' >&2

# ── Parse busybox ping output ─────────────────────────
parse_ping() {
    awk -F'[ =/]+' '
        /round-trip|rtt/ { print $(NF-3), $(NF-2), $(NF-1); found=1 }
        END { if (!found) print "0 0 0" }
    '
}

# ── One measurement pass ──────────────────────────────
# args: label, dscp_class ("" for none, "EF" for Voice)
run_pass() {
    local label="$1"
    local dscp="$2"
    local ping_out="$OUTDIR/xb_ping_${label}_${TS}.txt"
    local iperf_out="$OUTDIR/xb_iperf_${label}_${TS}.json"

    info "── pass: $label (dscp=${dscp:-none}) ──"

    # Inject OUTPUT mangle rule if EF
    if [[ -n "$dscp" ]]; then
        info "  add OUTPUT mangle: -d $PROBE -p icmp -j DSCP --set-dscp-class $dscp"
        rssh "iptables -t mangle -I OUTPUT -d $PROBE -p icmp -j DSCP --set-dscp-class $dscp" >/dev/null
    fi

    info "  starting iperf3 upload (${LOAD_SECS}s, detached)"
    rssh "(iperf3 -c $IPERF_SERVER -p $IPERF_PORT -t $LOAD_SECS -J > /tmp/xb_iperf.json 2>&1 &) ; echo ok" >/dev/null
    sleep 3  # let TCP ramp into saturation

    info "  probing ${PROBE_SECS}s ping → $PROBE"
    rssh "ping -c $PROBE_SECS -W 2 $PROBE" > "$ping_out" 2>&1 || true

    # Wait for iperf3 to finish (should be done by now since load_secs > probe_secs+3)
    sleep $((LOAD_SECS - PROBE_SECS - 3 + 2))
    rssh "cat /tmp/xb_iperf.json" > "$iperf_out" 2>&1 || true

    # Remove the mangle rule
    if [[ -n "$dscp" ]]; then
        rssh "iptables -t mangle -D OUTPUT -d $PROBE -p icmp -j DSCP --set-dscp-class $dscp" >/dev/null 2>&1 || true
    fi

    local stats; stats=$(parse_ping < "$ping_out")
    local min avg max; read -r min avg max <<<"$stats"
    local upload_bps; upload_bps=$(awk -F'[:,]' '/bits_per_second.*sender/{gsub(/[^0-9.]/,"",$2); print $2; exit}' "$iperf_out" 2>/dev/null || echo "0")
    [[ -z "$upload_bps" ]] && upload_bps=0
    pass "$label: min=${min} avg=${avg} max=${max} ms | upload=$(awk -v b=$upload_bps 'BEGIN{printf "%.1f",b/1e6}')Mbps"

    cat <<EOF
{
  "label": "$label",
  "dscp": "${dscp:-none}",
  "ping_min_ms": $min, "ping_avg_ms": $avg, "ping_max_ms": $max,
  "upload_bps": $upload_bps,
  "ping_log": "$(basename "$ping_out")",
  "iperf_log": "$(basename "$iperf_out")"
}
EOF
}

# ── Idle baseline ─────────────────────────────────────
info "═══ Idle baseline (no load) ═══"
rssh "ping -c 5 -W 2 $PROBE" > "$OUTDIR/xb_idle_${TS}.txt" 2>&1 || true
IDLE=$(parse_ping < "$OUTDIR/xb_idle_${TS}.txt")
read -r idle_min idle_avg idle_max <<<"$IDLE"
pass "Idle: min=$idle_min avg=$idle_avg max=$idle_max ms"

# ── Two loaded passes ─────────────────────────────────
R1=$(run_pass "loaded_default" "")
sleep 5
R2=$(run_pass "loaded_ef" "EF")

# ── Differential ──────────────────────────────────────
d1=$(echo "$R1" | sed -n 's/.*"ping_avg_ms"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1)
d2=$(echo "$R2" | sed -n 's/.*"ping_avg_ms"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1)
delta_default=$(awk -v a="$d1" -v b="$idle_avg" 'BEGIN{printf "%.2f", a-b}')
delta_ef=$(awk -v a="$d2" -v b="$idle_avg" 'BEGIN{printf "%.2f", a-b}')
diffserv_benefit=$(awk -v a="$d1" -v b="$d2" 'BEGIN{printf "%.2f", a-b}')

# ── Assemble JSON ─────────────────────────────────────
cat > "$JSON" <<EOF
{
  "benchmark": "cross_traffic",
  "timestamp": "$TS",
  "router": "$ROUTER",
  "iperf_server": "$IPERF_SERVER",
  "probe": "$PROBE",
  "load_secs": $LOAD_SECS,
  "probe_secs": $PROBE_SECS,
  "idle": {
    "min_ms": $idle_min, "avg_ms": $idle_avg, "max_ms": $idle_max
  },
  "loaded_default_tin": $R1,
  "loaded_voice_tin_EF": $R2,
  "diffserv_benefit_ms": $diffserv_benefit,
  "delta_default_vs_idle_ms": $delta_default,
  "delta_ef_vs_idle_ms": $delta_ef
}
EOF

info "═══════════════════════════════════════════"
info "Results: $JSON"
info ""
info "Summary:"
printf "  %-18s %8s %8s %8s  %s\n" "scenario" "min" "avg" "max" "Δ vs idle"
printf "  %-18s %8s %8s %8s  %s\n" "------------------" "----" "----" "----" "----------"
printf "  %-18s %8s %8s %8s  %s\n" "idle"          "$idle_min" "$idle_avg" "$idle_max" "—"
printf "  %-18s %8s %8s %8s  %s\n" "loaded default"  $(echo "$R1" | sed -n 's/.*"ping_min_ms"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1) "$d1" $(echo "$R1" | sed -n 's/.*"ping_max_ms"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1) "+${delta_default} ms"
printf "  %-18s %8s %8s %8s  %s\n" "loaded EF(Voice)" $(echo "$R2" | sed -n 's/.*"ping_min_ms"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1) "$d2" $(echo "$R2" | sed -n 's/.*"ping_max_ms"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1) "+${delta_ef} ms"
info ""
info "Diffserv4 benefit (default - EF): ${diffserv_benefit} ms"
info "═══════════════════════════════════════════"
