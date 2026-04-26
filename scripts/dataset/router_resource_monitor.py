#!/usr/bin/env python3
"""
router_resource_monitor.py — Track 3 (hardware feasibility) collector
=====================================================================

Samples router vitals at 1 Hz while a blind test runs in parallel.
Captures: CPU%, MemAvailable, daemon liveness, conntrack count, state JSON
freshness, daemon uptime, flash write proxy.

Usage:
  # Foreground (interactive)
  python3 router_resource_monitor.py --output results/blind_test/resources_modeA.jsonl

  # Background (orchestrator launches it)
  python3 router_resource_monitor.py --output ... &
  MON_PID=$!
  ... run blind test ...
  kill $MON_PID
"""
import argparse, json, os, signal, subprocess, sys, time
from pathlib import Path

ROUTER_IP   = os.environ.get("MYCOFLOW_ROUTER_IP",   "10.10.1.1")
ROUTER_USER = os.environ.get("MYCOFLOW_ROUTER_USER", "root")
ROUTER_PASS = os.environ.get("MYCOFLOW_ROUTER_PASS", "sukranflat7")
USE_KEYAUTH = bool(os.environ.get("MYCOFLOW_SSH_KEYAUTH"))
SSH_OPTS    = ["-o", "StrictHostKeyChecking=no",
               "-o", "ConnectTimeout=3",
               "-o", "LogLevel=ERROR",
               "-o", "ServerAliveInterval=10"]

# Single one-shot probe — must be fast (<1s round trip).
# Combined into one ssh call to avoid overhead.
PROBE_CMD = r"""
TS=$(date +%s.%N)
PIDOF=$(pidof mycoflowd 2>/dev/null)
UPTIME=$(awk '{print $1}' /proc/uptime)
CPU_LINE=$(grep -E '^cpu ' /proc/stat)
MEM_AVAIL=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)
MEM_TOTAL=$(awk '/^MemTotal:/{print $2}' /proc/meminfo)
LOAD=$(awk '{print $1,$2,$3}' /proc/loadavg)
CT_COUNT=$(cat /proc/sys/net/netfilter/nf_conntrack_count 2>/dev/null || echo 0)
ST_AGE=$(if [ -f /tmp/myco_state.json ]; then echo $((`date +%s` - `stat -c %Y /tmp/myco_state.json`)); else echo -1; fi)
ST_PERSONA=$(grep -o '"persona":"[^"]*"' /tmp/myco_state.json 2>/dev/null | head -1 | cut -d: -f2 | tr -d '"')
ST_REASON=$(grep -o '"reason":"[^"]*"' /tmp/myco_state.json 2>/dev/null | head -1 | cut -d: -f2 | tr -d '"')
ST_BW=$(grep -o '"bandwidth_kbit":[0-9]*' /tmp/myco_state.json 2>/dev/null | head -1 | cut -d: -f2)
SAFE=$(grep -o '"safe_mode":[a-z]*' /tmp/myco_state.json 2>/dev/null | head -1 | cut -d: -f2)
PROC_CPU=""
if [ -n "$PIDOF" ] && [ -f /proc/$PIDOF/stat ]; then
  PROC_CPU=$(awk '{print $14+$15}' /proc/$PIDOF/stat)
  PROC_RSS=$(awk '/^VmRSS:/{print $2}' /proc/$PIDOF/status 2>/dev/null)
fi
echo "TS=$TS"
echo "PIDOF=$PIDOF"
echo "UPTIME=$UPTIME"
echo "CPU_LINE=$CPU_LINE"
echo "MEM_AVAIL=$MEM_AVAIL"
echo "MEM_TOTAL=$MEM_TOTAL"
echo "LOAD=$LOAD"
echo "CT_COUNT=$CT_COUNT"
echo "ST_AGE=$ST_AGE"
echo "ST_PERSONA=$ST_PERSONA"
echo "ST_REASON=$ST_REASON"
echo "ST_BW=$ST_BW"
echo "SAFE=$SAFE"
echo "PROC_CPU_TICKS=$PROC_CPU"
echo "PROC_RSS_KB=$PROC_RSS"
"""

def probe_router():
    """Send single SSH command, parse KEY=VAL output."""
    if USE_KEYAUTH:
        argv = ["ssh"] + SSH_OPTS + [f"{ROUTER_USER}@{ROUTER_IP}", PROBE_CMD]
    else:
        argv = ["sshpass", "-p", ROUTER_PASS, "ssh"] + SSH_OPTS + \
               [f"{ROUTER_USER}@{ROUTER_IP}", PROBE_CMD]
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=8)
    except subprocess.TimeoutExpired:
        return {"ts": time.time(), "error": "ssh-timeout"}

    out = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    out["_local_ts"] = time.time()
    out["_ssh_rc"]   = result.returncode
    return out

def parse_cpu_line(line):
    """Parse '/proc/stat cpu' line into idle/total ticks."""
    if not line or not line.startswith("cpu "):
        return None, None
    parts = line.split()
    if len(parts) < 8:
        return None, None
    # cpu user nice system idle iowait irq softirq steal ...
    nums = [int(x) for x in parts[1:8]]
    user, nice, system, idle, iowait, irq, softirq = nums
    idle_total = idle + iowait
    non_idle   = user + nice + system + irq + softirq
    return idle_total, idle_total + non_idle

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, help="JSONL output path")
    parser.add_argument("--interval", type=float, default=1.0,
                        help="Sampling interval seconds (default 1.0)")
    parser.add_argument("--duration", type=float, default=None,
                        help="Stop after this many seconds (default: run until SIGTERM)")
    args = parser.parse_args()

    Path(args.output).parent.mkdir(parents=True, exist_ok=True)
    fp = open(args.output, "a", buffering=1)

    print(f"[mon] writing to {args.output}", flush=True)
    print(f"[mon] sampling every {args.interval}s, ctrl-C to stop", flush=True)

    prev_idle = prev_total = None
    prev_proc_ticks = None
    prev_uptime     = None
    samples = 0
    t_start = time.time()

    # Catch SIGTERM gracefully (orchestrator kill)
    stop = {"flag": False}
    def handle(sig, frame):
        stop["flag"] = True
    signal.signal(signal.SIGTERM, handle)
    signal.signal(signal.SIGINT,  handle)

    try:
        while not stop["flag"]:
            if args.duration and (time.time() - t_start) > args.duration:
                break
            t0 = time.time()
            d = probe_router()
            samples += 1
            rec = {"ts": float(d.get("TS", d.get("_local_ts", t0))),
                   "local_ts": d.get("_local_ts"),
                   "samples": samples}

            # CPU%
            idle, total = parse_cpu_line(d.get("CPU_LINE", ""))
            if idle is not None and prev_idle is not None and total > prev_total:
                rec["cpu_pct"] = round(100.0 * (1.0 - (idle - prev_idle) / (total - prev_total)), 2)
            prev_idle, prev_total = idle, total

            # Per-process CPU%
            try:
                up = float(d.get("UPTIME", 0))
                proc_ticks = int(d.get("PROC_CPU_TICKS", 0)) if d.get("PROC_CPU_TICKS") else None
                if proc_ticks is not None and prev_proc_ticks is not None and up > prev_uptime:
                    delta_ticks = proc_ticks - prev_proc_ticks
                    delta_up    = up - prev_uptime
                    # 100 ticks/sec on most kernels; CPU% = ticks/sec / clk_tck
                    rec["mycoflowd_cpu_pct"] = round(delta_ticks / 100.0 / delta_up * 100.0, 2)
                prev_proc_ticks = proc_ticks
                prev_uptime     = up
            except Exception:
                pass

            # Memory
            try:
                rec["mem_avail_kb"] = int(d.get("MEM_AVAIL", 0))
                rec["mem_total_kb"] = int(d.get("MEM_TOTAL", 0))
                if rec["mem_total_kb"] > 0:
                    rec["mem_used_pct"] = round(100.0 * (1 - rec["mem_avail_kb"]/rec["mem_total_kb"]), 2)
            except Exception:
                pass

            # Process RSS
            try:
                rec["mycoflowd_rss_kb"] = int(d.get("PROC_RSS_KB", 0))
            except Exception:
                pass

            # Liveness
            rec["daemon_alive"]  = bool(d.get("PIDOF"))
            rec["daemon_pid"]    = d.get("PIDOF") or None
            rec["uptime"]        = float(d.get("UPTIME", 0)) if d.get("UPTIME") else None
            rec["loadavg"]       = d.get("LOAD")

            # State JSON freshness
            try:
                rec["state_age_s"] = int(d.get("ST_AGE", -1))
            except Exception:
                rec["state_age_s"] = -1
            rec["state_persona"]   = d.get("ST_PERSONA") or None
            rec["state_reason"]    = d.get("ST_REASON")  or None
            rec["state_bw_kbit"]   = int(d.get("ST_BW", 0)) if d.get("ST_BW","").isdigit() else None
            rec["state_safe_mode"] = (d.get("SAFE") == "true")

            # Conntrack
            try:
                rec["ct_count"] = int(d.get("CT_COUNT", 0))
            except Exception:
                rec["ct_count"] = -1

            # SSH errors
            if d.get("error") or d.get("_ssh_rc") not in (0, None):
                rec["ssh_error"] = d.get("error") or f"rc={d.get('_ssh_rc')}"

            fp.write(json.dumps(rec) + "\n")

            # Sleep remainder
            elapsed = time.time() - t0
            if elapsed < args.interval:
                time.sleep(args.interval - elapsed)
    finally:
        fp.close()
        print(f"\n[mon] {samples} samples written → {args.output}")

if __name__ == "__main__":
    main()
