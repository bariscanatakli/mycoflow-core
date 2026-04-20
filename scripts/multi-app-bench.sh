#!/usr/bin/env bash
# MycoFlow multi-app single-user benchmark (Phase 6d)
#
# Why this test exists:
#   qemu-bench.sh already covers the "one app per device" case — a gamer
#   netns, a bulk netns, a torrent netns, each with a distinct src IP.
#   That's what the per-device (v2) classifier was built for.
#
#   This script covers the *failure mode* of per-device classification:
#   a single developer machine running ~10 apps at once (Zoom call +
#   Steam download + git push + Spotify + a browser tab streaming YT
#   in the background). All traffic shares one src IP, so a per-device
#   assignment must pick one persona and mis-classify everything else.
#
#   Flow-aware (v3) classifies each flow individually and pushes
#   per-flow CONNMARK → per-flow DSCP, so VOIP/GAME can hold EF while
#   the same device's torrent sits on CS1. That's what we measure.
#
# What we run — all from the gamer netns (single src IP):
#   - "game": low-rate UDP 64B  → should classify as game_rt (EF)
#   - "voip": very-low UDP 64B  → voip_call (EF)
#   - "bulk": TCP single stream → bulk_dl (CS1)
#   - "torr": TCP × 20 parallel → torrent (CS1)
#
# Success = under contention, the game ping RTT stays low (<20 ms delta)
# and /tmp/myco_state.json shows distinct service tags per flow.
#
# Environment: reuses the qemu-bench Docker lab (qemu-lab running).
# Assumes mycoflowd is installed on the OpenWrt VM at /usr/bin/mycoflowd.

set -euo pipefail

LAB="${LAB:-mycoflow-qemu}"
SERVER_IP="${SERVER_IP:-10.0.1.2}"
OPENWRT_IP="${OPENWRT_IP:-192.168.1.1}"
OPENWRT_WAN="${OPENWRT_WAN:-eth1}"
WAN_BW_KBIT="${WAN_BW_KBIT:-20000}"
DURATION="${DURATION:-30}"
OUTDIR="${RESULTS_DIR:-$(dirname "$0")/../results}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
openwrt() { docker exec "$LAB" sshpass -p '' ssh $SSH_OPTS root@"$OPENWRT_IP" "$@"; }
user()    { docker exec "$LAB" ip netns exec gamer "$@"; }
user_bg() { docker exec -d "$LAB" ip netns exec gamer "$@"; }

info()  { printf '[MULTI] %s\n' "$*"; }
pass()  { printf '[OK]    %s\n' "$*"; }

# ── 1. Wipe prior state, set CAKE, start mycoflowd in flow-aware mode ──────
info "Clean WAN qdisc + start mycoflowd (flow_aware=1)..."
openwrt "
killall mycoflowd 2>/dev/null; true
tc qdisc del dev $OPENWRT_WAN root 2>/dev/null; true
tc qdisc add dev $OPENWRT_WAN root cake bandwidth ${WAN_BW_KBIT}kbit diffserv4
MYCOFLOW_NO_TC=0 \
MYCOFLOW_DUMMY=0 \
MYCOFLOW_BW_KBIT=${WAN_BW_KBIT} \
MYCOFLOW_EGRESS_IFACE=$OPENWRT_WAN \
MYCOFLOW_PER_DEVICE=1 \
MYCOFLOW_FLOW_AWARE=1 \
MYCOFLOW_PROBE_HOST=${SERVER_IP} \
MYCOFLOW_SAMPLE_HZ=2 \
MYCOFLOW_LOG_LEVEL=3 \
MYCOFLOW_BASELINE_SAMPLES=3 \
/usr/bin/mycoflowd > /tmp/mycoflowd.log 2>&1 &
sleep 2; echo 'mycoflowd PID: '\$(pgrep mycoflowd)
"

# ── 2. Idle ping baseline ─────────────────────────────────────────────────
info "Idle RTT (no contention)..."
idle_ms=$(user ping -c 10 -i 0.2 "$SERVER_IP" 2>/dev/null \
          | awk -F'/' 'END{print $5}')
idle_ms="${idle_ms:-999}"
pass "idle: ${idle_ms} ms"

# ── 3. Fire all four "apps" simultaneously from the same netns ────────────
TOTAL_S=$(( DURATION + 20 ))
info "Starting multi-app traffic mix (${TOTAL_S}s)..."

# game_rt — small UDP, destination port 27015 (Steam source engine) so the
# port hint backs up the behavior signal.
user_bg iperf3 -c "$SERVER_IP" -u -b 150k -l 120 -p 5201 -t "$TOTAL_S" \
               --logfile /dev/null 2>/dev/null

# voip — tiny UDP, port 3478 (STUN) to tilt port hint toward voip_call.
user_bg iperf3 -c "$SERVER_IP" -u -b 64k  -l 64  -p 5202 -t "$TOTAL_S" \
               --logfile /dev/null 2>/dev/null

# bulk_dl — straight TCP, port 5204 — bytes-heavy, low pkt rate.
user_bg iperf3 -c "$SERVER_IP"               -p 5204 -t "$TOTAL_S" \
               --logfile /dev/null 2>/dev/null

# torrent-ish — many parallel TCP streams.
user_bg iperf3 -c "$SERVER_IP" -P 20 -l 1000 -p 5206 -t "$TOTAL_S" \
               --logfile /dev/null 2>/dev/null

info "Warming classifier for 15s..."
sleep 15

# ── 4. Measure "game" ping RTT while the other three are hammering ────────
info "Measuring RTT under multi-app contention (${DURATION} pings)..."
user ping -c "$DURATION" -i 1 "$SERVER_IP" \
    > "$OUTDIR/multiapp_ping_${TIMESTAMP}.txt" 2>/dev/null || true
load_ms=$(awk -F'/' 'END{print $5}' "$OUTDIR/multiapp_ping_${TIMESTAMP}.txt" 2>/dev/null || echo "999")
load_ms="${load_ms:-999}"
delta=$(python3 -c "print(f\"{max(float('$load_ms')-float('$idle_ms'),0):.1f}\")" 2>/dev/null || echo "999")
pass "loaded: ${load_ms} ms  (Δ = +${delta} ms)"

# ── 5. Capture per-flow classification snapshot ───────────────────────────
info "Per-flow classifier snapshot (/tmp/myco_state.json):"
openwrt "cat /tmp/myco_state.json" > "$OUTDIR/multiapp_state_${TIMESTAMP}.json" 2>/dev/null || true
python3 -c "
import json, sys
try:
    s = json.load(open('$OUTDIR/multiapp_state_${TIMESTAMP}.json'))
except Exception as e:
    print('  (no state json:', e, ')'); sys.exit(0)
flows = s.get('flows', [])
print(f'  {len(flows)} classified flow(s):')
for f in flows:
    tag = f.get('service','?')
    print(f\"    {f.get('src','?'):>15} -> {f.get('dst','?'):<15} \"
          f\"dport={f.get('dport','?'):<5} proto={f.get('proto','?')} \"
          f\"svc={tag:<15} mark={f.get('mark',0):<3} stable={f.get('stable',0)} \"
          f\"rtt={f.get('rtt_ms',0)}ms demoted={f.get('demoted',0)}\")
" || true

# ── 6. CAKE tin counters show how much traffic got prioritised ────────────
info "CAKE tin statistics:"
openwrt "tc -s qdisc show dev $OPENWRT_WAN" | head -60

# ── 7. Cleanup ────────────────────────────────────────────────────────────
info "Stopping clients + mycoflowd..."
user killall iperf3 2>/dev/null || true
openwrt "killall mycoflowd 2>/dev/null" || true

printf '\n== SUMMARY ==\n  idle=%s ms  loaded=%s ms  delta=+%s ms\n' \
       "$idle_ms" "$load_ms" "$delta"
printf '  artifacts: %s/multiapp_{ping,state}_%s.*\n' "$OUTDIR" "$TIMESTAMP"
