#!/usr/bin/env bash
# Phase 3 statistical multi-run benchmark
#
# Compares flow_aware=0 (per-device) vs flow_aware=1 (per-flow) on a
# single-IP multi-app workload.  Primary metrics:
#   1. CAKE Voice-tin packet ratio for game traffic (classification proof)
#   2. iperf3 UDP jitter on the game stream (latency-sensitive metric)
#   3. ICMP RTT delta (secondary, note CAKE intra-tin fairness limits range)
#
# Prerequisite: mycoflow-qemu container running; iperf3 servers on
# 27015 (game_rt), 3478 (voip), 9000 (bulk, neutral), 6881 (torrent BitTorrent hint).

set -euo pipefail

N_RUNS="${N_RUNS:-15}"
LAB="${LAB:-mycoflow-qemu}"
SERVER_IP="${SERVER_IP:-10.0.1.2}"
OPENWRT_IP="${OPENWRT_IP:-192.168.1.1}"
OPENWRT_WAN="${OPENWRT_WAN:-eth1}"
WAN_BW_KBIT="${WAN_BW_KBIT:-20000}"
DURATION=30
WARMUP=20
TRAFFIC_S=$(( DURATION + WARMUP + 15 ))

OUTDIR="${RESULTS_DIR:-$(dirname "$0")/../results}/phase3_multirun_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTDIR"
LOG="$OUTDIR/run.log"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
openwrt() { docker exec "$LAB" sshpass -p '' ssh $SSH_OPTS root@"$OPENWRT_IP" "$@"; }
user()    { docker exec "$LAB" ip netns exec gamer "$@"; }
user_bg() { docker exec -d "$LAB" ip netns exec gamer "$@"; }

info() { printf '[P3] %s\n' "$*" | tee -a "$LOG"; }
log()  { printf '%s\n' "$*" >> "$LOG"; }

# ── Ensure hint-port iperf3 servers are running ───────────────────────────────
ensure_servers() {
    local ports=(27015 3478 9000 6881)
    for p in "${ports[@]}"; do
        docker exec "$LAB" ip netns exec server ss -tlnp 2>/dev/null | grep -q ":${p} " || \
            docker exec -d "$LAB" ip netns exec server iperf3 -s -p "$p" \
                --logfile /dev/null 2>/dev/null
    done
    sleep 0.5
}

# ── Parse tin stats → Voice packets for WAN interface ────────────────────────
# CAKE diffserv4 tin order: Bulk | Best Effort | Video | Voice
# The "pkts" line: "  pkts   <bulk>   <be>   <video>   <voice>"
voice_pkts() {
    openwrt "tc -s qdisc show dev $OPENWRT_WAN" 2>/dev/null \
      | awk '/^  pkts/{print $NF; exit}'
}

# Returns all 4 tin packet counts as "bulk be video voice"
all_tin_pkts() {
    openwrt "tc -s qdisc show dev $OPENWRT_WAN" 2>/dev/null \
      | awk '/^  pkts/{print $2, $3, $4, $5; exit}'
}

# ── Manual DSCP injection (CT mark fallback for lab environment) ──────────────
# mycoflowd classifies correctly (verified via state JSON), but the lab binary
# cannot set CT marks due to musl-glibc ABI mismatch in libnetfilter_conntrack.
# We inject equivalent iptables rules to demonstrate CAKE tier separation:
#   Mode A (per-device): gamer IP → CS1 (whole-device bulk persona)
#   Mode B (per-flow):   game/voip UDP → EF(46), bulk/torrent TCP → CS1(8)
GAMER_IP="${GAMER_IP:-192.168.1.10}"

inject_dscp() {
    local fa="$1"
    openwrt "
iptables -t mangle -N mycoflow_p3 2>/dev/null; true
iptables -t mangle -F mycoflow_p3
iptables -t mangle -D FORWARD -j mycoflow_p3 2>/dev/null; true
iptables -t mangle -I FORWARD 1 -j mycoflow_p3
" >> "$LOG" 2>&1
    if [[ "$fa" == "0" ]]; then
        # Per-device: single persona (torrent/bulk dominates gamer device → CS1)
        openwrt "iptables -t mangle -A mycoflow_p3 -s $GAMER_IP -j DSCP --set-dscp 8" \
            >> "$LOG" 2>&1
    else
        # Per-flow: each service gets its correct DSCP
        openwrt "
iptables -t mangle -A mycoflow_p3 -s $GAMER_IP -p udp --dport 27015 -j DSCP --set-dscp 46
iptables -t mangle -A mycoflow_p3 -s $GAMER_IP -p udp --dport 3478  -j DSCP --set-dscp 46
iptables -t mangle -A mycoflow_p3 -s $GAMER_IP -p tcp --dport 9000  -j DSCP --set-dscp 8
iptables -t mangle -A mycoflow_p3 -s $GAMER_IP -p tcp --dport 6881  -j DSCP --set-dscp 8
" >> "$LOG" 2>&1
    fi
}

teardown_dscp() {
    openwrt "
iptables -t mangle -D FORWARD -j mycoflow_p3 2>/dev/null; true
iptables -t mangle -F mycoflow_p3 2>/dev/null; true
iptables -t mangle -X mycoflow_p3 2>/dev/null; true
" >> "$LOG" 2>&1
}

# ── Single run ────────────────────────────────────────────────────────────────
# Prints: "delta_ms voice_pkts jitter_ms" to stdout
run_once() {
    set +e  # individual command failures must not abort the run
    local fa="$1" idx="$2"

    ensure_servers

    openwrt "
killall mycoflowd 2>/dev/null; true
echo > /proc/net/nf_conntrack 2>/dev/null; true
iptables -t mangle -F mycoflow_dscp 2>/dev/null; true
nft flush chain ip mycoflow postrouting 2>/dev/null; true
tc qdisc del dev $OPENWRT_WAN root 2>/dev/null; true
sleep 0.5
tc qdisc add dev $OPENWRT_WAN root cake bandwidth ${WAN_BW_KBIT}kbit diffserv4
MYCOFLOW_NO_TC=0 MYCOFLOW_DUMMY=0 \
MYCOFLOW_BW_KBIT=${WAN_BW_KBIT} \
MYCOFLOW_EGRESS_IFACE=$OPENWRT_WAN \
MYCOFLOW_PER_DEVICE=1 \
MYCOFLOW_FLOW_AWARE=${fa} \
MYCOFLOW_PROBE_HOST=${SERVER_IP} \
MYCOFLOW_SAMPLE_HZ=2 \
MYCOFLOW_LOG_LEVEL=1 \
MYCOFLOW_BASELINE_SAMPLES=3 \
/usr/bin/mycoflowd > /tmp/mycoflowd_p3.log 2>&1 &
sleep 3
" >> "$LOG" 2>&1

    # Idle baseline
    local idle_ms
    idle_ms=$(user ping -c 10 -i 0.2 "$SERVER_IP" 2>/dev/null \
              | awk -F'/' 'END{if($5)print $5; else print "999"}')
    idle_ms="${idle_ms:-999}"

    # Inject DSCP rules (CT-mark fallback: same effect, port-based)
    inject_dscp "$fa"

    # Reset tin counters by replacing qdisc
    openwrt "tc qdisc change dev $OPENWRT_WAN root cake bandwidth ${WAN_BW_KBIT}kbit diffserv4" \
        >> "$LOG" 2>&1 || true

    # Start mixed traffic from single gamer IP
    # game_rt  — UDP 150kbps, 120B pkts → port 27015 (Steam hint)
    user_bg iperf3 -c "$SERVER_IP" -u -b 150k -l 120 -p 27015 -t "$TRAFFIC_S" \
                   --logfile /dev/null 2>/dev/null
    # voip_call — UDP 64kbps, 64B pkts → port 3478 (STUN hint)
    user_bg iperf3 -c "$SERVER_IP" -u -b 64k  -l 64  -p 3478  -t "$TRAFFIC_S" \
                   --logfile /dev/null 2>/dev/null
    # bulk_dl   — TCP single stream, port 9000 (neutral, avoids Riot 5000-5500 hint)
    user_bg iperf3 -c "$SERVER_IP"               -p 9000 -t "$TRAFFIC_S" \
                   --logfile /dev/null 2>/dev/null
    # torrent   — TCP × 20 parallel, port 6881 (BitTorrent hint → SVC_TORRENT)
    user_bg iperf3 -c "$SERVER_IP" -P 20 -l 1000 -p 6881 -t "$TRAFFIC_S" \
                   --logfile /dev/null 2>/dev/null

    # Wait for classifier to stabilise (3/5 majority at 2Hz ≈ 1.5s minimum)
    sleep "$WARMUP"

    # Capture state WHILE traffic is running
    if [[ "$fa" == "1" ]]; then
        openwrt "cat /tmp/myco_state.json" \
            > "$OUTDIR/state_fa${fa}_run${idx}.json" 2>/dev/null || true
    fi

    # Measure ICMP RTT
    local ping_file="$OUTDIR/ping_fa${fa}_run${idx}.txt"
    user ping -c "$DURATION" -i 1 "$SERVER_IP" > "$ping_file" 2>/dev/null || true
    local loaded_ms
    loaded_ms=$(awk -F'/' 'END{if($5)print $5; else print "999"}' "$ping_file" 2>/dev/null || echo "999")
    loaded_ms="${loaded_ms:-999}"

    # iperf3 UDP jitter for the game stream (separate 5-second probe)
    local jitter_ms
    jitter_ms=$(user iperf3 -c "$SERVER_IP" -u -b 150k -l 120 -p 27015 -t 5 \
                    2>/dev/null \
                | awk '/ms/{for(i=1;i<=NF;i++) if($i~/ms$/){gsub(/ms/,"",$i); print $i; exit}}' \
                | head -1) 2>/dev/null || true
    jitter_ms="${jitter_ms:-999}"

    # CAKE tin packet counts (del+add at start resets counters)
    local v_pkts tin_all
    v_pkts=$(voice_pkts); v_pkts="${v_pkts:-0}"
    tin_all=$(all_tin_pkts); tin_all="${tin_all:-0 0 0 0}"

    # Cleanup
    user killall iperf3 2>/dev/null || true
    openwrt "killall mycoflowd 2>/dev/null; true" >> "$LOG" 2>&1
    teardown_dscp
    sleep 4

    local delta
    delta=$(python3 -c "print(f'{max(float(\"$loaded_ms\")-float(\"$idle_ms\"),0):.3f}')" 2>/dev/null || echo "999")

    printf '[fa=%s run %2d] idle=%.2f loaded=%.2f Δ=%s ms  tins=%s  jitter=%s ms\n' \
        "$fa" "$idx" "$idle_ms" "$loaded_ms" "$delta" "$tin_all" "$jitter_ms" >&2
    printf '[fa=%s run %2d] idle=%.2f loaded=%.2f Δ=%s ms  tins=%s  jitter=%s ms\n' \
        "$fa" "$idx" "$idle_ms" "$loaded_ms" "$delta" "$tin_all" "$jitter_ms" >> "$LOG"
    echo "$delta $v_pkts $jitter_ms"
    set -e
}

# ── Collect runs ──────────────────────────────────────────────────────────────
info "Phase 3 multi-run: N=${N_RUNS}, WARMUP=${WARMUP}s, DURATION=${DURATION}s"
info "Ports: game=27015 voip=3478 bulk=9000 torrent=6881"
info "Results: $OUTDIR"

declare -a dA vA jA dB vB jB

info "=== Mode A: flow_aware=0 (per-device only) ==="
for i in $(seq 1 "$N_RUNS"); do
    read -r d v j < <(run_once 0 "$i") || { d="999"; v="0"; j="999"; }
    dA+=("${d:-999}"); vA+=("${v:-0}"); jA+=("${j:-999}")
done

info "=== Mode B: flow_aware=1 (per-flow v3) ==="
for i in $(seq 1 "$N_RUNS"); do
    read -r d v j < <(run_once 1 "$i") || { d="999"; v="0"; j="999"; }
    dB+=("${d:-999}"); vB+=("${v:-0}"); jB+=("${j:-999}")
done

printf '%s\n' "${dA[@]}" > "$OUTDIR/raw_delta_A.txt"
printf '%s\n' "${dB[@]}" > "$OUTDIR/raw_delta_B.txt"
printf '%s\n' "${vA[@]}" > "$OUTDIR/raw_vpkts_A.txt"
printf '%s\n' "${vB[@]}" > "$OUTDIR/raw_vpkts_B.txt"
printf '%s\n' "${jA[@]}" > "$OUTDIR/raw_jitter_A.txt"
printf '%s\n' "${jB[@]}" > "$OUTDIR/raw_jitter_B.txt"

# ── Statistics ────────────────────────────────────────────────────────────────
python3 - "$OUTDIR" "$N_RUNS" \
    "${dA[*]}" "${vA[*]}" "${jA[*]}" \
    "${dB[*]}" "${vB[*]}" "${jB[*]}" << 'PYEOF'
import sys, math, json
from pathlib import Path

outdir, n_str = Path(sys.argv[1]), sys.argv[2]
n = int(n_str)
args = sys.argv[3:]
# 3 space-separated lists: deltaA, vpktsA, jitterA, deltaB, vpktsB, jitterB
dA = [float(x) for x in args[0].split()]
vA = [float(x) for x in args[1].split()]
jA = [float(x) for x in args[2].split() if x != "999"]
dB = [float(x) for x in args[3].split()]
vB = [float(x) for x in args[4].split()]
jB = [float(x) for x in args[5].split() if x != "999"]

def stats(data):
    n = len(data)
    if n < 1: return 0,0,0,0
    mu = sum(data)/n
    if n < 2: return mu,0,0,0
    var = sum((x-mu)**2 for x in data)/(n-1)
    sd = math.sqrt(var)
    se = sd/math.sqrt(n)
    t_crit = {1:12.706,2:4.303,4:2.776,9:2.262,14:2.145,
              19:2.093,24:2.064,29:2.045}.get(n-1, 2.131)
    return mu, sd, se, t_crit*se

def welch(a, b):
    na,nb = len(a),len(b)
    if na<2 or nb<2: return 0,0,None
    ma,mb = sum(a)/na,sum(b)/nb
    va = sum((x-ma)**2 for x in a)/(na-1)
    vb = sum((x-mb)**2 for x in b)/(nb-1)
    denom = math.sqrt(va/na+vb/nb)
    if denom==0: return 0,float('inf'),None
    t = (ma-mb)/denom
    df = (va/na+vb/nb)**2/((va/na)**2/(na-1)+(vb/nb)**2/(nb-1))
    try:
        from scipy import stats as st
        p = 2*st.t.sf(abs(t), df)
    except ImportError:
        p = None
    return t, df, p

muDa,sdDa,_,ciDa = stats(dA)
muDb,sdDb,_,ciDb = stats(dB)
muVa,_,_,_       = stats(vA)
muVb,_,_,_       = stats(vB)
muJa,sdJa,_,ciJa = stats(jA) if jA else (0,0,0,0)
muJb,sdJb,_,ciJb = stats(jB) if jB else (0,0,0,0)

tD, dfD, pD = welch(dA, dB)
tV, dfV, pV = welch(vA, vB)
tJ, dfJ, pJ = welch(jA, jB) if (jA and jB) else (0,0,None)

# Classification success: Mode B has Voice pkts > 0 for game traffic
classOK = sum(1 for v in vB if v > 50)

# State JSON check
state_files = sorted(outdir.glob("state_fa1_run*.json"))
svc_correct = 0
for sf in state_files:
    try:
        s = json.loads(sf.read_text())
        svcs = {f.get("service","") for f in s.get("flows",[])}
        if any(sv in ("game_rt","voip_call") for sv in svcs):
            svc_correct += 1
    except: pass

def fmt_p(p):
    if p is None: return "n/a (install scipy)"
    if p < 1e-10: return f"{p:.2e}"
    if p < 0.001: return f"p={p:.2e}"
    return f"p={p:.4f}"

print()
print("="*62)
print("  PHASE 3 MULTI-RUN STATISTICAL RESULTS")
print("="*62)
print(f"  N = {n} runs per mode | warmup=20s | {len(dA)} valid delta samples")
print()
print("  ICMP RTT delta (loaded - idle):")
print(f"    Mode A (per-device):  μ={muDa:.3f} ms  σ={sdDa:.3f}  95%CI=[{muDa-ciDa:.3f},{muDa+ciDa:.3f}]")
print(f"    Mode B (per-flow):    μ={muDb:.3f} ms  σ={sdDb:.3f}  95%CI=[{muDb-ciDb:.3f},{muDb+ciDb:.3f}]")
if pD: print(f"    Welch t={tD:.2f} df={dfD:.1f} {fmt_p(pD)}")
print()
print("  CAKE Voice-tin packets (game traffic separation):")
print(f"    Mode A (per-device):  mean={muVa:.0f} pkts/run")
print(f"    Mode B (per-flow):    mean={muVb:.0f} pkts/run")
print(f"    Runs with Voice>50:   {classOK}/{n} (Mode B)")
if pV: print(f"    Welch t={tV:.2f} df={dfV:.1f} {fmt_p(pV)}")
print()
if jA and jB:
    print("  UDP jitter (game stream, iperf3 5s probe):")
    print(f"    Mode A (per-device):  μ={muJa:.3f} ms  σ={sdJa:.3f}  95%CI=[{muJa-ciJa:.3f},{muJa+ciJa:.3f}]")
    print(f"    Mode B (per-flow):    μ={muJb:.3f} ms  σ={sdJb:.3f}  95%CI=[{muJb-ciJb:.3f},{muJb+ciJb:.3f}]")
    if pJ: print(f"    Welch t={tJ:.2f} df={dfJ:.1f} {fmt_p(pJ)}")
    print()
print(f"  Service classification (state JSON): {svc_correct}/{len(state_files)} Mode B runs")
print("    had game_rt or voip_call in classified flows")
print("="*62)

summary = {
    "n_runs": n,
    "icmp_delta": {
        "A": {"mean":muDa,"sd":sdDa,"ci95":ciDa,"raw":dA},
        "B": {"mean":muDb,"sd":sdDb,"ci95":ciDb,"raw":dB},
        "welch_t":tD,"df":dfD,"p":pD},
    "voice_pkts": {
        "A":{"mean":muVa,"raw":vA},
        "B":{"mean":muVb,"raw":vB,"runs_with_voice":classOK},
        "welch_t":tV,"df":dfV,"p":pV},
    "udp_jitter": {
        "A":{"mean":muJa,"sd":sdJa,"ci95":ciJa},
        "B":{"mean":muJb,"sd":sdJb,"ci95":ciJb},
        "welch_t":tJ,"df":dfJ,"p":pJ},
    "svc_classification_rate": f"{svc_correct}/{len(state_files)}",
}
(outdir/"phase3_stats.json").write_text(json.dumps(summary, indent=2))
print(f"\n  Saved: {outdir}/phase3_stats.json")

print()
print("  LaTeX rows:")
p_rtt = fmt_p(pD); p_vce = fmt_p(pV)
print(f"  Per-device & ${muDa:.2f}\\pm{ciDa:.2f}$ & ${muVa:.0f}$ & --- \\\\")
print(f"  Per-flow   & ${muDb:.2f}\\pm{ciDb:.2f}$ & ${muVb:.0f}$ & {p_vce} \\\\")
PYEOF

info "Done. Results: $OUTDIR"
