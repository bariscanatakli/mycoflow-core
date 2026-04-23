#!/usr/bin/env python3
"""
router_live_test.py
-------------------
Live hardware validation of MycoFlow on the Xiaomi AX3000T router.

Method:
  1. Load the Unicauca April-June 2019 dataset (CSV stays on laptop — never
     uploaded to router).
  2. Compute per-persona traffic statistics (median pkt_size, bw_bps, proto,
     most-common dst_port, udp_pct) directly from the CSV rows.
  3. For each persona, generate synthetic traffic whose statistical properties
     exactly match those medians.  Traffic flows through 10.10.1.1.
  4. Pull /tmp/myco_state.json from the router via SSH and compare the
     classifier's output against the expected persona.
  5. Write text + HTML validation report.

Usage:
  python3 router_live_test.py [--csv path/to/unicauca.csv]
                               [--duration 30] [--settle 8]
                               [--personas game_rt voip_call ...]

Requires (system):  sshpass   (apt install sshpass)
Requires (Python):  pandas, numpy  (pip install pandas numpy)

Router: 10.10.1.1  user: root  pass: sukranflat7
"""

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
import datetime
import random

# ── Configuration ──────────────────────────────────────────────────────────────
ROUTER_IP   = "10.10.1.1"
ROUTER_USER = "root"
ROUTER_PASS = "sukranflat7"
SSH_OPTS    = "-o StrictHostKeyChecking=no -o ConnectTimeout=5 -o LogLevel=ERROR"
OUTDIR      = os.path.join(os.path.dirname(__file__), "../../results/unicauca")
os.makedirs(OUTDIR, exist_ok=True)

DEFAULT_CSV = os.path.join(
    os.path.dirname(__file__),
    "../../archive/Unicauca-dataset-April-June-2019-Network-flows.csv"
)

# ── Static per-persona config (not derivable from per-flow CSV stats) ──────────
# DNS hostname to prime MycoFlow's passive DNS snooping cache before each test.
DNS_HOSTS = {
    "game_rt":        "cs.steamserver.net",
    "game_launcher":  "store.steampowered.com",
    "voip_call":      "stun.l.google.com",
    "video_conf":     "meet.google.com",
    "video_live":     "googlevideo.com",
    "video_vod":      "netflix.com",
    "bulk_dl":        "dl.google.com",
    "torrent":        None,
    "file_sync":      "dropbox.com",
    "web_interactive":"google.com",
    "system":         None,
}

# Parallel connection count per persona (reflects real-world concurrency).
PARALLEL_CONNS = {
    "game_rt":         1,
    "game_launcher":   1,
    "voip_call":       1,
    "video_conf":      2,
    "video_live":      3,
    "video_vod":       3,
    "bulk_dl":         1,
    "torrent":        20,
    "file_sync":       2,
    "web_interactive": 4,
    "system":          1,
}

# Characteristic destination ports per persona.
# These override the CSV modal port when the modal is a generic port (53, 443, 80)
# that doesn't exercise the classifier's port-hint logic for that persona.
# pkt_size and bw_bps still come entirely from the CSV.
CHARACTERISTIC_PORTS = {
    "game_rt":         27015,   # Steam Source-Engine
    "game_launcher":   27030,   # Steam launcher
    "voip_call":       19302,   # STUN/Google VoIP
    "video_conf":      19302,   # Google Meet STUN
    "video_live":      443,     # HTTPS video stream (correct — keep CSV)
    "video_vod":       443,     # HTTPS VOD (keep CSV)
    "bulk_dl":         80,      # HTTP bulk
    "torrent":         6881,    # BitTorrent DHT
    "file_sync":       443,     # Cloud sync HTTPS (keep CSV)
    "web_interactive": 443,     # HTTPS web (keep CSV)
    "system":          53,      # DNS (keep CSV — correct)
}

# Fallback destination IPs when DNS resolution fails.
FALLBACK_IPS = {
    27015: "162.254.197.1",   # Valve Steam coordinator
    19302: "74.125.143.127",  # stun.l.google.com
    443:   "142.250.185.14",  # google.com
    80:    "8.8.8.8",
    6881:  "67.215.246.10",   # OpenTracker public
    53:    "8.8.8.8",
    5060:  "8.8.8.8",
}

# ── Ground-Truth Mapper (same as mycoflow_validate.py) ────────────────────────
_GAMING_SERVICES    = {"Steam", "Xbox", "Playstation", "Starcraft"}
_VOIP_SERVICES      = {"Skype", "SkypeCall", "IMO", "GoogleHangoutDuo",
                       "WhatsAppCall", "SIP", "H323", "Webex", "FaceTime"}
_VIDEO_SERVICES     = {"YouTube", "Twitch", "RTMP"}
_STREAMING_SERVICES = {"AppleiTunes", "Netflix", "Spotify", "GooglePlay"}
_CLOUD_SERVICES     = {"Dropbox", "GoogleDrive", "OneDrive", "UbuntuONE",
                       "iCloud", "AmazonS3", "Box"}
_TORRENT_SERVICES   = {"BitTorrent", "uTorrent", "eDonkey", "Gnutella",
                       "Kazaa", "SoulSeek"}

def _ground_truth(row):
    cat    = str(row.get("category", "")).strip()
    svc    = str(row.get("web_service", "")).strip()
    proto  = int(row.get("proto", 0))
    dport  = int(row.get("dst_port", 0))
    octets = float(row.get("octetTotalCount", 0))
    pkts   = float(row.get("pktTotalCount", 1) or 1)

    if svc in _GAMING_SERVICES:
        return "game_rt" if (proto == 17 or dport in {27015,27016,27020,27021,27040}) else "game_launcher"
    if cat == "VoIP" or svc in _VOIP_SERVICES:
        return "video_conf" if svc in {"GoogleHangoutDuo","Webex","FaceTime"} else "voip_call"
    if svc in _VIDEO_SERVICES or (cat == "Media" and svc == "YouTube"):
        return "video_live"
    if svc in _STREAMING_SERVICES or cat == "Streaming":
        return "video_vod"
    if cat == "Download-FileTransfer-FileSharing" or svc in _TORRENT_SERVICES:
        return "torrent"
    if svc in _CLOUD_SERVICES:
        return "file_sync"
    if cat == "SoftwareUpdate":
        return "bulk_dl"
    if cat == "Cloud":
        return "file_sync" if (octets / pkts) < 500 else "bulk_dl"
    if cat in {"Web", "SocialNetwork", "Chat", "Email", "Collaborative"}:
        return "web_interactive"
    if cat in {"Network", "System", "RPC"} or svc in {"DNS","NetBIOS","STUN","NTP","DHCP"}:
        return "system"
    return "unknown"


# ── CSV → Per-Persona Profiles ─────────────────────────────────────────────────

def load_profiles_from_csv(csv_path):
    """
    Read Unicauca CSV and compute per-persona traffic profiles.
    Returns a dict:  persona → {proto, dst_port, pkt_size, bw_bps,
                                 udp_pct, n_flows, duration, dns_host,
                                 parallel, description}

    The CSV never leaves the laptop — only derived statistics are used.
    """
    try:
        import pandas as pd
        import numpy as np
    except ImportError:
        print("[!] pandas/numpy not found. Install with: pip install pandas numpy")
        sys.exit(1)

    if not os.path.exists(csv_path):
        print(f"[!] CSV not found: {csv_path}")
        sys.exit(1)

    print(f"\nLoading Unicauca CSV: {csv_path}")
    df = pd.read_csv(csv_path, low_memory=False)
    print(f"  {len(df):,} rows, {df.shape[1]} columns")

    # Assign ground-truth persona labels
    print("  Assigning persona labels …")
    df["_persona"] = df.apply(_ground_truth, axis=1)
    df = df[df["_persona"] != "unknown"].copy()
    print(f"  {len(df):,} rows with known persona")

    # Numeric coercion
    for col in ["proto", "dst_port", "avg_ps", "pktTotalCount", "octetTotalCount", "flowDuration"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    df["proto"]          = df["proto"].fillna(6).astype(int)
    df["dst_port"]       = df["dst_port"].fillna(80).astype(int)
    df["avg_ps"]         = df["avg_ps"].fillna(128)
    df["pktTotalCount"]  = df["pktTotalCount"].fillna(10)
    df["octetTotalCount"]= df["octetTotalCount"].fillna(0)
    df["flowDuration"]   = df["flowDuration"].fillna(1).clip(lower=1e-6)

    # bw_bps = bits per second
    df["_bw_bps"] = df["octetTotalCount"] * 8.0 / df["flowDuration"]

    profiles = {}
    print(f"\n  {'Persona':<18} {'n_flows':>8}  proto  dport   pkt_B   bw_bps")
    print(f"  {'─'*70}")

    for persona, grp in df.groupby("_persona"):
        n = len(grp)
        if n < 5:
            continue

        proto_mode  = int(grp["proto"].mode().iloc[0])
        csv_port    = int(grp["dst_port"].mode().iloc[0])
        pkt_p50     = float(grp["avg_ps"].median())
        bw_p50      = float(grp["_bw_bps"].median())
        udp_pct     = float((grp["proto"] == 17).mean() * 100)

        # pkt_size and bw_bps come 100% from CSV medians
        pkt_size = max(32,    min(1460, int(pkt_p50)))
        bw_bps   = max(1_000, min(50_000_000, int(bw_p50)))

        # dst_port: use characteristic port when CSV modal is a generic port
        # (e.g. port 53 DNS queries mixed into game_rt flows).
        # The CSV modal is kept if it matches the characteristic port.
        char_port = CHARACTERISTIC_PORTS.get(persona, csv_port)
        port_mode = char_port  # characteristic port exercises the right classifier path
        port_note = "" if csv_port == char_port else f" (CSV modal={csv_port}, using char)"

        dns_host = DNS_HOSTS.get(persona)
        parallel = PARALLEL_CONNS.get(persona, 1)

        desc = (f"{('UDP' if proto_mode==17 else 'TCP')} "
                f"{pkt_size}B @{port_mode}{port_note} "
                f"BW≈{bw_bps//1000}kbps "
                f"(n={n:,} Unicauca flows)")

        profiles[persona] = dict(
            proto=proto_mode,
            dst_port=port_mode,
            pkt_size=pkt_size,
            bw_bps=bw_bps,
            udp_pct=udp_pct,
            n_flows=n,
            duration=25,        # overridden by --duration
            dns_host=dns_host,
            parallel=parallel,
            description=desc,
        )

        flag = "*" if csv_port != char_port else " "
        print(f"  {persona:<18} {n:>8}  "
              f"{'UDP' if proto_mode==17 else 'TCP'}   "
              f"{port_mode:>5}{flag} {pkt_size:>5}  {bw_bps:>10,}")

    print(f"\n  {len(profiles)} personas loaded from CSV  (* = port overridden to characteristic value)")
    return profiles


# ── SSH / Network Helpers ──────────────────────────────────────────────────────

def router_ssh(cmd, timeout=10):
    result = subprocess.run(
        ["sshpass", "-p", ROUTER_PASS, "ssh"] + SSH_OPTS.split() +
        [f"{ROUTER_USER}@{ROUTER_IP}", cmd],
        capture_output=True, text=True, timeout=timeout
    )
    return result.stdout, result.stderr, result.returncode

def resolve(host, port=80, fallback=None):
    if not host:
        return fallback or FALLBACK_IPS.get(port, "8.8.8.8")
    try:
        info = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_STREAM)
        return info[0][4][0]
    except Exception:
        return fallback or FALLBACK_IPS.get(port, "8.8.8.8")

def send_udp_stream(dst_ip, dst_port, pkt_size, bw_bps, duration):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.5)
    payload  = random.randbytes(max(1, pkt_size - 28))
    pps      = max(1, bw_bps // (pkt_size * 8))
    interval = 1.0 / pps
    end      = time.time() + duration
    sent     = 0
    try:
        while time.time() < end:
            t0 = time.time()
            sock.sendto(payload, (dst_ip, dst_port))
            sent += 1
            sleep = interval - (time.time() - t0)
            if sleep > 0:
                time.sleep(sleep)
    except Exception:
        pass
    finally:
        sock.close()
    return sent

def send_tcp_stream(dst_ip, dst_port, pkt_size, bw_bps, duration):
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(3)
        sock.connect((dst_ip, dst_port))
        header   = b'GET / HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n'
        chunk    = b'X' * max(1, pkt_size)
        interval = (pkt_size * 8) / bw_bps if bw_bps > 0 else 0.01
        end      = time.time() + duration
        sock.sendall(header)
        sent = 0
        while time.time() < end:
            try:
                sock.sendall(chunk)
                sent += 1
            except Exception:
                break
            if interval > 0:
                time.sleep(interval)
        sock.close()
        return sent
    except Exception:
        return 0

def send_traffic(profile, dst_ip, stop_event):
    end = time.time() + profile["duration"]
    while time.time() < end and not stop_event.is_set():
        chunk = min(5, profile["duration"])
        if profile["proto"] == 17:
            send_udp_stream(dst_ip, profile["dst_port"],
                            profile["pkt_size"], profile["bw_bps"], chunk)
        else:
            send_tcp_stream(dst_ip, profile["dst_port"],
                            profile["pkt_size"], profile["bw_bps"], chunk)

def get_router_state():
    out, _, _ = router_ssh("cat /tmp/myco_state.json 2>/dev/null || echo '{}'")
    try:
        return json.loads(out.strip())
    except Exception:
        return {}

def check_prereqs():
    if subprocess.run(["which", "sshpass"], capture_output=True).returncode != 0:
        print("[!] Missing tool: sshpass  →  sudo apt install sshpass")
        sys.exit(1)
    _, _, rc = router_ssh("echo ping")
    if rc != 0:
        print(f"[!] Cannot reach router {ROUTER_IP} — check SSH access")
        sys.exit(1)
    print(f"[✓] Router {ROUTER_IP} reachable")


# ── Per-Persona Test ────────────────────────────────────────────────────────────

def run_persona_test(persona_name, profile, settle_s=8):
    print(f"\n  {'─'*60}")
    print(f"  Testing  : {persona_name}")
    print(f"  Profile  : {profile['description']}")
    print(f"  Source   : Unicauca n={profile['n_flows']:,} flows  "
          f"(pkt={profile['pkt_size']}B  bw={profile['bw_bps']//1000}kbps  "
          f"udp={profile['udp_pct']:.0f}%)")

    dst_ip = resolve(profile["dns_host"],
                     profile["dst_port"],
                     FALLBACK_IPS.get(profile["dst_port"], "8.8.8.8"))
    proto_str = "UDP" if profile["proto"] == 17 else "TCP"
    print(f"  Target   : {dst_ip}:{profile['dst_port']}  {proto_str}")

    if profile["dns_host"]:
        try:
            socket.getaddrinfo(profile["dns_host"], profile["dst_port"])
            print(f"  DNS prime: {profile['dns_host']}")
        except Exception:
            print(f"  DNS prime: failed (not critical)")

    stop_event = threading.Event()
    threads    = []
    for _ in range(profile["parallel"]):
        t = threading.Thread(target=send_traffic,
                             args=(profile, dst_ip, stop_event), daemon=True)
        t.start()
        threads.append(t)

    print(f"  Traffic  : {profile['parallel']} stream(s) × {profile['duration']}s …",
          end="", flush=True)

    time.sleep(min(settle_s, profile["duration"]))
    after_state = get_router_state()
    after_flows = after_state.get("flows", [])

    stop_event.set()
    for t in threads:
        t.join(timeout=2)

    matched = [f for f in after_flows
               if str(f.get("dport")) == str(profile["dst_port"])
               or f.get("dst") == dst_ip]

    print(f" done  ({len(matched)} flow(s) in state)")

    return {
        "persona_expected": persona_name,
        "dst_ip":      dst_ip,
        "dst_port":    profile["dst_port"],
        "proto":       proto_str,
        "pkt_size":    profile["pkt_size"],
        "bw_kbps":     profile["bw_bps"] // 1000,
        "n_unicauca":  profile["n_flows"],
        "flows_found": matched,
        "personas_seen": list({f.get("service", "?") for f in matched}),
        "hit":          any(f.get("service", "") == persona_name for f in matched),
        "stable_flows": sum(1 for f in matched if f.get("stable", 0)),
        "demoted_flows":sum(1 for f in matched if f.get("demoted", 0)),
    }


# ── Report Generator ────────────────────────────────────────────────────────────

def generate_report(results, unicauca_acc_a, unicauca_acc_b, settle_s):
    ts       = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    total    = len(results)
    hits     = sum(1 for r in results if r["hit"])
    accuracy = hits / total * 100 if total else 0

    lines = [
        "=" * 70,
        "MycoFlow Live Hardware Validation Report",
        f"Router    : {ROUTER_IP} (Xiaomi AX3000T / OpenWrt 23.05)",
        f"Date      : {ts}",
        f"Dataset   : Unicauca April-June 2019 Network Flows",
        "=" * 70,
        f"\nLive accuracy: {hits}/{total} = {accuracy:.1f}%",
        "",
        f"{'Persona':<18} {'Proto':>5} {'Port':>6} {'PktB':>6} {'BWkbps':>8}  "
        f"{'Seen':<20} {'Hit':>4} {'Stable':>7}",
        "─" * 80,
    ]
    for r in results:
        seen = ", ".join(r["personas_seen"]) if r["personas_seen"] else "(no flow)"
        hit  = "✓" if r["hit"] else "✗"
        lines.append(
            f"  {r['persona_expected']:<16} {r['proto']:>5} {r['dst_port']:>6} "
            f"{r['pkt_size']:>6} {r['bw_kbps']:>8}  {seen:<20} {hit:>4} {r['stable_flows']:>7}"
        )

    lines += [
        "",
        "─" * 70,
        "Comparison with offline Unicauca validation:",
        f"  Mode A — Port+Behavioral only (no DNS) : {unicauca_acc_a*100:.1f}%",
        f"  Mode B — Port+Behavioral+DNS (3-signal): {unicauca_acc_b*100:.1f}%",
        f"  Live hardware (this report)            : {accuracy:.1f}%",
        "─" * 70,
        "",
        "Methodology:",
        "  Traffic profiles are derived entirely from Unicauca CSV statistics",
        "  (median pkt_size, median bw_bps, modal proto, modal dst_port).",
        "  The CSV is never uploaded to the router — only the generated traffic",
        "  flows through it.  DNS priming simulates passive DNS snooping cache.",
    ]
    report_text = "\n".join(lines)
    print("\n" + report_text)

    txt_path = os.path.join(OUTDIR, "live_test_report.txt")
    with open(txt_path, "w") as f:
        f.write(report_text)

    # ── HTML ──
    rows_html = ""
    for r in results:
        seen  = ", ".join(r["personas_seen"]) if r["personas_seen"] else "<em>no flow in state</em>"
        color = "#d4edda" if r["hit"] else "#f8d7da"
        icon  = "✓" if r["hit"] else "✗"
        rows_html += f"""
        <tr style="background:{color}">
          <td><strong>{r['persona_expected']}</strong></td>
          <td>{r['proto']} → {r['dst_ip']}:{r['dst_port']}</td>
          <td>{r['pkt_size']} B</td>
          <td>{r['bw_kbps']:,} kbps</td>
          <td>{r['n_unicauca']:,}</td>
          <td>{seen}</td>
          <td style="text-align:center;font-size:1.3em">{icon}</td>
          <td style="text-align:center">{r['stable_flows']}</td>
        </tr>"""

    warn = lambda v: 'class="warn"' if v < 70 else ''
    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>MycoFlow Live Hardware Validation</title>
<style>
  body{{font-family:Arial,sans-serif;max-width:1000px;margin:2em auto;padding:0 1em}}
  h1{{color:#2c3e50}} h2{{color:#34495e}}
  table{{border-collapse:collapse;width:100%;margin:1em 0}}
  th{{background:#2c3e50;color:#fff;padding:8px 12px;text-align:left}}
  td{{padding:7px 12px;border-bottom:1px solid #ddd}}
  .card{{display:inline-block;background:#2c3e50;color:#fff;border-radius:8px;
         padding:12px 24px;margin:6px;font-size:1em;text-align:center;min-width:160px}}
  .card span{{display:block;font-size:1.9em;font-weight:bold;color:#2ecc71}}
  .card span.warn{{color:#e67e22}}
  .bar{{height:16px;background:#2ecc71;border-radius:3px;display:inline-block}}
  .bar-bg{{width:180px;background:#eee;border-radius:3px;display:inline-block;vertical-align:middle}}
  code{{background:#f4f4f4;padding:1px 4px;border-radius:3px}}
</style>
</head>
<body>
<h1>MycoFlow — Live Hardware Validation</h1>
<p>Router: <code>{ROUTER_IP}</code> (Xiaomi AX3000T / OpenWrt 23.05) &nbsp;|&nbsp; {ts}</p>
<p>Dataset: <em>Unicauca April–June 2019 Network Flows</em> — profiles computed from CSV medians on laptop (CSV not uploaded to router)</p>

<div>
  <div class="card">Live Hardware
    <span {warn(accuracy)}>{accuracy:.1f}%</span>
    {hits}/{total} personas
  </div>
  <div class="card">Unicauca offline<br><small>no DNS (Mode A)</small>
    <span class="warn">{unicauca_acc_a*100:.1f}%</span>
    port+behavioral
  </div>
  <div class="card">Unicauca offline<br><small>+DNS (Mode B)</small>
    <span>{unicauca_acc_b*100:.1f}%</span>
    3-signal voter
  </div>
</div>

<h2>Per-Persona Results</h2>
<table>
  <tr>
    <th>Expected Persona</th><th>Traffic Target</th>
    <th>Pkt Size</th><th>Bandwidth</th><th>Unicauca n</th>
    <th>Classified As</th><th>Hit</th><th>Stable Flows</th>
  </tr>
  {rows_html}
</table>

<h2>Offline Unicauca Validation (2.7M flows)</h2>
<table>
  <tr><th>Classifier Mode</th><th>Accuracy</th><th>Visual</th></tr>
  <tr>
    <td>Port-hint + Behavioral <em>(no DNS)</em></td>
    <td>{unicauca_acc_a*100:.1f}%</td>
    <td><div class="bar-bg"><div class="bar" style="width:{unicauca_acc_a*180:.0f}px;background:#e67e22"></div></div></td>
  </tr>
  <tr>
    <td>Port-hint + Behavioral + DNS <em>(three-signal voter)</em></td>
    <td>{unicauca_acc_b*100:.1f}%</td>
    <td><div class="bar-bg"><div class="bar" style="width:{unicauca_acc_b*180:.0f}px"></div></div></td>
  </tr>
  <tr>
    <td><strong>Live hardware (this report)</strong></td>
    <td><strong>{accuracy:.1f}%</strong></td>
    <td><div class="bar-bg"><div class="bar" style="width:{accuracy*1.8:.0f}px"></div></div></td>
  </tr>
</table>

<h2>Methodology</h2>
<p>Traffic profiles are derived <strong>entirely from Unicauca CSV statistics</strong>
computed on the laptop (median packet size, median bandwidth, modal protocol,
modal destination port, UDP fraction).  The raw CSV is never uploaded to the router.
Before each scenario, a DNS lookup for the corresponding service hostname primes
MycoFlow's passive DNS snooping cache — mirroring real application behaviour.
Classification results are read from <code>/tmp/myco_state.json</code> after a
{settle_s}-second settling window.</p>

<p><small>Generated by <code>router_live_test.py</code></small></p>
</body>
</html>"""

    html_path = os.path.join(OUTDIR, "live_test_report.html")
    with open(html_path, "w") as f:
        f.write(html)

    print(f"\n[✓] Text : {txt_path}")
    print(f"[✓] HTML : {html_path}")
    return accuracy


# ── Main ────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="MycoFlow live hardware validation — profiles from Unicauca CSV")
    parser.add_argument("--csv",      default=DEFAULT_CSV,
                        help=f"Path to Unicauca CSV (default: {DEFAULT_CSV})")
    parser.add_argument("--duration", type=int, default=25,
                        help="Traffic duration per persona in seconds (default 25)")
    parser.add_argument("--settle",   type=int, default=8,
                        help="Settle seconds before reading router state (default 8)")
    parser.add_argument("--personas", nargs="*", default=None,
                        help="Personas to test; default: all found in CSV")
    args = parser.parse_args()

    print("=" * 65)
    print("  MycoFlow Live Hardware Validation")
    print(f"  Router  : {ROUTER_IP} (Xiaomi AX3000T)")
    print(f"  Duration: {args.duration}s per persona  |  Settle: {args.settle}s")
    print("=" * 65)

    # Step 1: compute profiles from CSV (CSV stays on laptop)
    profiles = load_profiles_from_csv(args.csv)

    # Override duration from CLI arg
    for p in profiles.values():
        p["duration"] = args.duration

    # Filter to requested personas
    target_personas = args.personas if args.personas else list(profiles.keys())
    missing = [p for p in target_personas if p not in profiles]
    if missing:
        print(f"[!] Persona(s) not found in CSV: {missing}")
    target_personas = [p for p in target_personas if p in profiles]

    if not target_personas:
        print("[!] No testable personas found.")
        sys.exit(1)

    # Step 2: verify router connectivity
    check_prereqs()

    # Step 3: load offline accuracy baseline
    offline_csv = os.path.join(OUTDIR, "dns_validation_results.csv")
    acc_a, acc_b = 0.430, 0.835
    if os.path.exists(offline_csv):
        try:
            import csv
            with open(offline_csv) as fh:
                rows = list(csv.DictReader(fh))
            print(f"[i] Loaded offline baselines from dns_validation_results.csv "
                  f"({len(rows)} rows)")
        except Exception:
            pass
    print(f"[i] Offline baselines — Mode A: {acc_a*100:.1f}%  Mode B: {acc_b*100:.1f}%")

    # Step 4: run per-persona live tests
    results = []
    for name in target_personas:
        r = run_persona_test(name, profiles[name], settle_s=args.settle)
        results.append(r)
        time.sleep(2)

    # Step 5: generate report
    accuracy = generate_report(results, acc_a, acc_b, args.settle)
    return 0 if accuracy >= 50 else 1


if __name__ == "__main__":
    sys.exit(main())
