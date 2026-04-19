#!/usr/bin/env bash
# MycoFlow Real-World Bufferbloat Benchmark
#
# Runs CAKE-only vs CAKE+MycoFlow bufferbloat tests on a live OpenWrt router
# over real WAN. Measures idle and loaded RTT, computes Δ and grade.
#
# All probing and loading happens ON the router (so WAN path is isolated
# from WSL/Linux host variability). OpenWrt busybox ping reports only
# min/avg/max — mdev is not available.
#
# Usage: ./scripts/realworld-bench.sh [OPTIONS]
#   --router IP        Router SSH host (default: 10.10.1.1)
#   --user USER        SSH user (default: root)
#   --pass PASS        SSH password (or set ROUTER_PASS env var)
#   --wan-iface IF     WAN egress iface on router (default: pppoe-wan)
#   --probe HOST       Ping target (default: 8.8.8.8)
#   --load-url URL     HTTP download URL for saturating downlink
#                      (default: http://speedtest.ftp.otenet.gr/files/test100Mb.db)
#   --idle-secs N      Idle ping duration (default: 5)
#   --load-secs N      Loaded ping duration (default: 20)
#   --warmup-secs N    Wait after starting mycoflowd (default: 15)
#   --output DIR       Output dir (default: results)
#   --skip-cake-only   Skip CAKE-only scenario
#   --skip-mycoflow    Skip MycoFlow scenario
#
# Exit 0 on success, non-zero on setup failure.

set -euo pipefail

ROUTER="10.10.1.1"
USER="root"
PASS="${ROUTER_PASS:-}"
WAN_IFACE="pppoe-wan"
PROBE="8.8.8.8"
LOAD_URL="http://speedtest.ftp.otenet.gr/files/test100Mb.db"
IDLE_SECS=5
LOAD_SECS=20
WARMUP_SECS=15
OUTDIR="results"
DO_CAKE=true
DO_MYCO=true

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info() { echo -e "${CYAN}[BENCH]${NC} $1" >&2; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1" >&2; }
pass() { echo -e "${GREEN}[OK]${NC} $1" >&2; }
fail() { echo -e "${RED}[FAIL]${NC} $1" >&2; }

while [[ $# -gt 0 ]]; do
    case $1 in
        --router)         ROUTER="$2";      shift 2 ;;
        --user)           USER="$2";        shift 2 ;;
        --pass)           PASS="$2";        shift 2 ;;
        --wan-iface)      WAN_IFACE="$2";   shift 2 ;;
        --probe)          PROBE="$2";       shift 2 ;;
        --load-url)       LOAD_URL="$2";    shift 2 ;;
        --idle-secs)      IDLE_SECS="$2";   shift 2 ;;
        --load-secs)      LOAD_SECS="$2";   shift 2 ;;
        --warmup-secs)    WARMUP_SECS="$2"; shift 2 ;;
        --output)         OUTDIR="$2";      shift 2 ;;
        --skip-cake-only) DO_CAKE=false;    shift ;;
        --skip-mycoflow)  DO_MYCO=false;    shift ;;
        -h|--help)        sed -n '3,30p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [[ -z "$PASS" ]]; then
    warn "No password given. Set ROUTER_PASS or pass --pass. Falling back to interactive SSH."
fi

mkdir -p "$OUTDIR"
TS=$(date +%Y%m%d_%H%M%S)
JSON="$OUTDIR/realworld_${TS}.json"

# ── SSH helper using SSH_ASKPASS (non-interactive password auth) ─────
SSH_CONTROL="/tmp/mycoflow-ssh-${TS}.sock"
setup_ssh_askpass() {
    if [[ -z "$PASS" ]]; then return 0; fi
    ASKPASS_BIN="$(mktemp)"
    cat > "$ASKPASS_BIN" <<EOF
#!/usr/bin/env bash
echo "$PASS"
EOF
    chmod +x "$ASKPASS_BIN"
    export SSH_ASKPASS="$ASKPASS_BIN"
    export SSH_ASKPASS_REQUIRE="force"
    export DISPLAY=:0
    # Pre-seed ControlMaster so subsequent ssh calls reuse the same connection
    setsid -w ssh -o StrictHostKeyChecking=no \
                  -o UserKnownHostsFile=/dev/null \
                  -o ControlMaster=yes \
                  -o ControlPath="$SSH_CONTROL" \
                  -o ControlPersist=10m \
                  -N -f "$USER@$ROUTER" 2>/dev/null
}

cleanup() {
    if [[ -n "${ASKPASS_BIN:-}" && -f "$ASKPASS_BIN" ]]; then rm -f "$ASKPASS_BIN"; fi
    if [[ -S "$SSH_CONTROL" ]]; then
        ssh -o ControlPath="$SSH_CONTROL" -O exit "$USER@$ROUTER" 2>/dev/null || true
    fi
}
trap cleanup EXIT

rssh() {
    ssh -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ControlPath="$SSH_CONTROL" \
        "$USER@$ROUTER" "$@"
}

setup_ssh_askpass

# ── Preflight ────────────────────────────────────────────────────────
info "Preflight: checking router connectivity"
if ! rssh 'uname -a' >/dev/null 2>&1; then
    fail "Cannot SSH to $USER@$ROUTER"; exit 2
fi
pass "SSH OK"

info "Preflight: checking CAKE on $WAN_IFACE"
QDISC=$(rssh "tc qdisc show dev $WAN_IFACE" 2>/dev/null || echo "")
if ! grep -q 'cake' <<<"$QDISC"; then
    fail "CAKE not active on $WAN_IFACE. Current qdisc:"; echo "$QDISC"
    warn "Enable with: tc qdisc replace dev $WAN_IFACE root cake bandwidth 50mbit diffserv4"
    exit 3
fi
pass "CAKE active: $(echo "$QDISC" | head -1)"

# ── Measurement primitives ───────────────────────────────────────────
# Parse OpenWrt busybox ping: 'round-trip min/avg/max = 7.841/8.320/9.234 ms'
parse_ping() {
    # emit "min avg max"; fall back to "0 0 0" on parse failure
    awk -F'[ =/]+' '
        /round-trip|rtt/ { print $(NF-3), $(NF-2), $(NF-1); found=1 }
        END { if (!found) print "0 0 0" }
    '
}

run_ping_idle() {
    local label="$1"
    local out="$OUTDIR/rw_ping_idle_${label}_${TS}.txt"
    info "  idle ping ${IDLE_SECS}s → $PROBE"
    rssh "ping -c $IDLE_SECS -W 2 $PROBE" > "$out" 2>&1 || true
    local line; line=$(parse_ping < "$out")
    echo "$line $out"
}

run_ping_loaded() {
    local label="$1"
    local out="$OUTDIR/rw_ping_load_${label}_${TS}.txt"
    local load_log="/tmp/rw_load_${label}_${TS}.log"
    info "  starting WAN saturator: curl $LOAD_URL"
    # kick off curl in background on router (log in /tmp/, router has no 'results/' cwd)
    rssh "(curl -sS -o /dev/null --max-time $((LOAD_SECS + 5)) '$LOAD_URL' > $load_log 2>&1 &); echo ok" >/dev/null
    sleep 2  # let TCP ramp
    info "  loaded ping ${LOAD_SECS}s → $PROBE"
    rssh "ping -c $LOAD_SECS -W 2 $PROBE" > "$out" 2>&1 || true
    # ensure curl is done
    rssh "killall curl 2>/dev/null; wait 2>/dev/null; true" >/dev/null 2>&1 || true
    local line; line=$(parse_ping < "$out")
    echo "$line $out"
}

grade() {
    local delta="$1"
    awk -v d="$delta" 'BEGIN{
        if (d<5)   print "A+";
        else if (d<30)  print "A";
        else if (d<60)  print "B";
        else if (d<200) print "C";
        else if (d<500) print "D";
        else print "F";
    }'
}

# ── Scenario runners ─────────────────────────────────────────────────
scenario_cake_only() {
    info "═══ Scenario 1: CAKE-only (MycoFlow OFF) ═══"
    # stop via init.d first so procd doesn't respawn it
    rssh "/etc/init.d/mycoflowd stop 2>/dev/null; killall -9 mycoflowd 2>/dev/null; true" >/dev/null 2>&1
    # Flush the DSCP chain so old per-device marks don't skew results
    rssh "iptables -t mangle -F mycoflow_dscp 2>/dev/null; true" >/dev/null 2>&1
    sleep 3
    local idle_line load_line idle_avg load_avg delta g
    read -r idle_min idle_avg idle_max idle_file < <(run_ping_idle "cake")
    read -r load_min load_avg load_max load_file < <(run_ping_loaded "cake")
    delta=$(awk -v a="$load_avg" -v b="$idle_avg" 'BEGIN{printf "%.2f", a-b}')
    g=$(grade "$delta")
    pass "CAKE-only: idle=${idle_avg}ms loaded=${load_avg}ms Δ=${delta}ms grade=$g"
    cat <<EOF
{
  "scenario": "cake_only",
  "idle_min_ms": $idle_min, "idle_avg_ms": $idle_avg, "idle_max_ms": $idle_max,
  "load_min_ms": $load_min, "load_avg_ms": $load_avg, "load_max_ms": $load_max,
  "delta_ms": $delta, "grade": "$g",
  "idle_log": "$(basename "$idle_file")", "load_log": "$(basename "$load_file")"
}
EOF
}

scenario_mycoflow() {
    info "═══ Scenario 2: CAKE + MycoFlow ═══"
    rssh "/etc/init.d/mycoflowd stop 2>/dev/null; killall -9 mycoflowd 2>/dev/null; true" >/dev/null 2>&1
    sleep 1
    info "  starting mycoflowd via procd"
    rssh "/etc/init.d/mycoflowd start" >/dev/null 2>&1
    sleep 3
    if ! rssh 'pidof mycoflowd' >/dev/null 2>&1; then
        fail "mycoflowd did not start — check 'logread | grep mycoflowd'"
        rssh "logread | grep mycoflowd | tail -20" >&2 || true
        return 1
    fi
    info "  warmup ${WARMUP_SECS}s for persona convergence"
    sleep "$WARMUP_SECS"
    local idle_min idle_avg idle_max idle_file load_min load_avg load_max load_file delta g
    read -r idle_min idle_avg idle_max idle_file < <(run_ping_idle "myco")
    read -r load_min load_avg load_max load_file < <(run_ping_loaded "myco")
    delta=$(awk -v a="$load_avg" -v b="$idle_avg" 'BEGIN{printf "%.2f", a-b}')
    g=$(grade "$delta")
    pass "MycoFlow: idle=${idle_avg}ms loaded=${load_avg}ms Δ=${delta}ms grade=$g"

    # Snapshot daemon state for the record
    local state_snap="$OUTDIR/rw_state_${TS}.json"
    rssh "cat /tmp/myco_state.json 2>/dev/null || true" > "$state_snap"
    local dscp_snap="$OUTDIR/rw_dscp_${TS}.txt"
    rssh "iptables -t mangle -S mycoflow_dscp 2>/dev/null || true" > "$dscp_snap"

    cat <<EOF
{
  "scenario": "mycoflow",
  "idle_min_ms": $idle_min, "idle_avg_ms": $idle_avg, "idle_max_ms": $idle_max,
  "load_min_ms": $load_min, "load_avg_ms": $load_avg, "load_max_ms": $load_max,
  "delta_ms": $delta, "grade": "$g",
  "idle_log": "$(basename "$idle_file")", "load_log": "$(basename "$load_file")",
  "state_snapshot": "$(basename "$state_snap")",
  "dscp_snapshot": "$(basename "$dscp_snap")"
}
EOF
}

# ── Main ─────────────────────────────────────────────────────────────
RESULTS=()

if $DO_CAKE; then
    R=$(scenario_cake_only) || { warn "CAKE-only scenario failed"; R=""; }
    [[ -n "$R" ]] && RESULTS+=("$R")
fi
if $DO_MYCO; then
    R=$(scenario_mycoflow) || { warn "MycoFlow scenario failed"; R=""; }
    [[ -n "$R" ]] && RESULTS+=("$R")
fi

# Assemble final JSON
{
    echo "{"
    echo "  \"benchmark\": \"realworld\","
    echo "  \"timestamp\": \"$TS\","
    echo "  \"router\": \"$ROUTER\","
    echo "  \"wan_iface\": \"$WAN_IFACE\","
    echo "  \"probe\": \"$PROBE\","
    echo "  \"load_url\": \"$LOAD_URL\","
    echo "  \"idle_secs\": $IDLE_SECS,"
    echo "  \"load_secs\": $LOAD_SECS,"
    echo "  \"results\": ["
    for i in "${!RESULTS[@]}"; do
        echo -n "    ${RESULTS[$i]}"
        [[ $i -lt $((${#RESULTS[@]} - 1)) ]] && echo "," || echo ""
    done
    echo "  ]"
    echo "}"
} > "$JSON"

info "═══════════════════════════════════════════"
info "Results written to $JSON"
info "Summary:"
printf "%-14s %10s %10s %10s %8s\n" "scenario" "idle_avg" "load_avg" "delta_ms" "grade"
printf "%-14s %10s %10s %10s %8s\n" "--------------" "--------" "--------" "--------" "-----"
extract_str() { echo "$1" | sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1; }
extract_num() { echo "$1" | sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\([0-9.-]*\).*/\1/p' | head -1; }
for R in "${RESULTS[@]}"; do
    scen=$(extract_str "$R" scenario)
    ia=$(extract_num "$R" idle_avg_ms)
    la=$(extract_num "$R" load_avg_ms)
    de=$(extract_num "$R" delta_ms)
    gr=$(extract_str "$R" grade)
    printf "%-14s %10s %10s %10s %8s\n" "$scen" "$ia" "$la" "$de" "$gr"
done
info "═══════════════════════════════════════════"
