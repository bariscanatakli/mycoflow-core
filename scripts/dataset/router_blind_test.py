#!/usr/bin/env python3
"""
router_blind_test.py — IEEE-defensible in-vivo classifier validation
====================================================================

Stratified random per-flow replay of the Unicauca dataset against
the live MycoFlow daemon on the production router (10.10.1.1).

CRITICAL DIFFERENCES vs router_live_test.py:
  - NO CHARACTERISTIC_PORTS override         (test contamination)
  - NO DNS_HOSTS hardcoded prime             (answer-feeding)
  - NO PARALLEL_CONNS hardcoded              (answer-feeding)
  - Per-flow replay using actual CSV row     (not per-persona median)
  - Two ablation modes:
      A) ip-only: no DNS query before connect (Mode A)
      B) hostname: socket.getaddrinfo on CSV   (Mode B)
  - Conntrack state flush between flows      (test isolation)
  - JSONL output for resumable runs

Usage:
  python3 router_blind_test.py --mode A --n-per-persona 150 --seed 42
  python3 router_blind_test.py --mode B --n-per-persona 150 --seed 42
"""

import argparse, json, os, random, socket, subprocess, sys, threading, time
from pathlib import Path

# ── Configuration ──────────────────────────────────────────────────────────────
ROUTER_IP   = os.environ.get("MYCOFLOW_ROUTER_IP", "10.10.1.1")
ROUTER_USER = os.environ.get("MYCOFLOW_ROUTER_USER", "root")
ROUTER_PASS = os.environ.get("MYCOFLOW_ROUTER_PASS", "sukranflat7")
SSH_OPTS    = ["-o", "StrictHostKeyChecking=no",
               "-o", "ConnectTimeout=5",
               "-o", "LogLevel=ERROR",
               "-o", "BatchMode=yes" if os.environ.get("MYCOFLOW_SSH_KEYAUTH") else "BatchMode=no"]
# When MYCOFLOW_SSH_KEYAUTH=1, skip sshpass and rely on the host's SSH agent /
# ~/.ssh keys. Lets us run from a box (Ubuntu testbed) where sshpass isn't
# installed and an SSH key has been pushed to the router's authorized_keys.
USE_KEYAUTH = bool(os.environ.get("MYCOFLOW_SSH_KEYAUTH"))

REPO_ROOT   = Path(__file__).resolve().parent.parent.parent
DEFAULT_CSV = REPO_ROOT / "archive" / "Unicauca-dataset-April-June-2019-Network-flows.csv"
RESULTS_DIR = REPO_ROOT / "results" / "blind_test"
RESULTS_DIR.mkdir(parents=True, exist_ok=True)

# Per-flow execution timing
# Traffic must be active when we read state — short-lived TCP flows get evicted
# from the classifier's flow table after the connection closes.
FLOW_PRE_READ_S      = 1.8   # generate traffic this long before reading state
FLOW_POST_READ_S     = 0.6   # keep flow alive a bit longer so classifier settles
FLOW_TOTAL_S         = FLOW_PRE_READ_S + FLOW_POST_READ_S + 0.6  # ~3.0s

# Bandwidth / packet size sanity bounds.
# BW_BPS_MAX is capped at 10 Mbps to not saturate the test laptop's uplink
# during concurrent real-world usage. All classifier thresholds sit below
# 10 Mbps (VIDEO ≤8M, STREAMING ≥5M, BULK/VOIP ≤200k), so the cap preserves
# classification fidelity while respecting the ISP's upload budget.
PKT_SIZE_MIN, PKT_SIZE_MAX = 32, 1460
BW_BPS_MIN,   BW_BPS_MAX   = 1_000, 10_000_000

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

# The router emits per-flow 'service' values in state.flows that are 1:1
# aligned with Unicauca's ground-truth labels — we compare directly.
# (See src/myco_service.c SERVICE_NAMES.)
# Keep this here as legacy alias for the temporal_split_validate.py import.
GT_TO_MYCO = {
    "game_rt": "game_rt", "game_launcher": "game_launcher",
    "voip_call": "voip_call", "video_conf": "video_conf",
    "video_live": "video_live", "video_vod": "video_vod",
    "bulk_dl": "bulk_dl", "torrent": "torrent",
    "file_sync": "file_sync", "web_interactive": "web_interactive",
    "system": "system",
}

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

# ── Hostname catalog (for Mode B only — derived from real services) ─────────
# These are EXAMPLES of what real flows of each type would resolve to.
# We use this only in Mode B so the router's DNS snoop sees realistic hostnames.
# NOTE: These are NOT cherry-picked classifier hints — they are real service
# hostnames that any user would query. The classifier learns persona from
# these via its own DNS hint module — the same module under evaluation.
SERVICE_HOSTNAMES = {
    "Steam":            ["api.steampowered.com", "steamcommunity.com"],
    "Xbox":             ["xboxlive.com", "live.xbox.com"],
    "Skype":            ["api.skype.com", "config.edge.skype.com"],
    "SkypeCall":        ["api.skype.com"],
    "GoogleHangoutDuo": ["meet.google.com", "duo.google.com"],
    "WhatsAppCall":     ["g.whatsapp.net", "mmg.whatsapp.net"],
    "Webex":            ["webex.com", "api.webex.com"],
    "FaceTime":         ["facetime.apple.com"],
    "YouTube":          ["www.youtube.com", "youtubei.googleapis.com"],
    "Twitch":           ["twitch.tv", "api.twitch.tv"],
    "Netflix":          ["netflix.com", "api.netflix.com"],
    "Spotify":          ["spotify.com", "api.spotify.com"],
    "GooglePlay":       ["play.google.com"],
    "Dropbox":          ["dropbox.com", "api.dropboxapi.com"],
    "GoogleDrive":      ["drive.google.com", "googleapis.com"],
    "OneDrive":         ["onedrive.live.com"],
    "iCloud":           ["icloud.com", "api.icloud.com"],
    "AmazonS3":         ["s3.amazonaws.com"],
    "BitTorrent":       [],  # P2P — no DNS by design
    "uTorrent":         [],
    "eDonkey":          [],
    "DNS":              [],
    "NetBIOS":          [],
    "NTP":              ["pool.ntp.org"],
    "DHCP":             [],
    # Generic web — will use any real hostname for that service
    "_default_web":     ["www.google.com", "www.cloudflare.com"],
}

# Reachable targets for traffic generation.
# UDP can go to 8.8.8.8 on any port (packets dropped but visible in conntrack).
# TCP requires a listener — Cloudflare 1.1.1.1 responds on 53/80/443; other ports
# drop silently on most networks (testbed ISP-dependent). Unreachable TCP ports
# are flagged as "tcp_unreachable" in results and excluded from accuracy math.
UDP_TARGET = "8.8.8.8"
TCP_TARGET = "1.1.1.1"
# Reachable-port whitelist used in Mode A fallback. Set MYCOFLOW_TCP_OPEN=1 on
# a clean LAN-attached testbed (Ubuntu server, etc.) where outbound TCP to
# arbitrary ports actually works — drops the whitelist so the test harness
# stops conservatively flagging tcp_unreachable for ports like 27015 or 6881.
TCP_REACHABLE_PORTS = (None
                       if os.environ.get("MYCOFLOW_TCP_OPEN")
                       else {53, 80, 443, 8080, 8443})

# ── SSH helpers ────────────────────────────────────────────────────────────────
def router_ssh(cmd, timeout=10):
    if USE_KEYAUTH:
        argv = ["ssh"] + SSH_OPTS + [f"{ROUTER_USER}@{ROUTER_IP}", cmd]
    else:
        argv = ["sshpass", "-p", ROUTER_PASS, "ssh"] + SSH_OPTS + \
               [f"{ROUTER_USER}@{ROUTER_IP}", cmd]
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
        return result.stdout, result.stderr, result.returncode
    except subprocess.TimeoutExpired:
        return "", "timeout", -1

def get_router_state():
    out, _, _ = router_ssh("cat /tmp/myco_state.json 2>/dev/null || echo '{}'")
    try:
        return json.loads(out.strip())
    except Exception:
        return {}

def get_local_lan_ip():
    """Detect the LAN IP the router sees us from.

    In WSL2/NAT/VPN setups, the local socket's IP (e.g. 172.30.x from WSL)
    differs from what the router sees (Windows host's 10.10.1.x). We detect
    by reading the router's own conntrack for our SSH session."""
    out, _, rc = router_ssh(
        "grep 'dst=" + ROUTER_IP + " .*dport=22' /proc/net/nf_conntrack | "
        "grep -oE 'src=10\\.[0-9.]+' | head -1 | cut -d= -f2",
        timeout=5)
    ip = out.strip()
    if ip and ip.startswith("10.") and ip != ROUTER_IP:
        return ip
    # Fallback: local socket IP (works when laptop is on same LAN directly)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((ROUTER_IP, 1))
        return s.getsockname()[0]
    finally:
        s.close()

# ── Traffic generators (lifted from router_live_test.py) ──────────────────────
def send_udp_stream(dst_ip, dst_port, pkt_size, bw_bps, duration, src_port=0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.5)
    if src_port:
        try:
            sock.bind(("", src_port))
        except OSError:
            pass  # port may be in use; fall back to ephemeral
    actual_sport = sock.getsockname()[1]
    payload  = random.randbytes(max(1, pkt_size - 28))
    pps      = max(1, int(bw_bps // (pkt_size * 8)))
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
    return sent, actual_sport

def send_tcp_stream(dst_ip, dst_port, pkt_size, bw_bps, duration, src_port=0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(3)
    if src_port:
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("", src_port))
        except OSError:
            pass
    try:
        sock.connect((dst_ip, dst_port))
        actual_sport = sock.getsockname()[1]
        chunk    = b'X' * max(1, pkt_size)
        interval = (pkt_size * 8) / bw_bps if bw_bps > 0 else 0.01
        end      = time.time() + duration
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
        return sent, actual_sport
    except Exception:
        try:
            actual_sport = sock.getsockname()[1]
        except Exception:
            actual_sport = 0
        sock.close()
        return 0, actual_sport

# ── DNS prime (Mode B only) ───────────────────────────────────────────────────
# The daemon's DNS sniffer uses AF_INET SOCK_RAW which only captures packets
# addressed to the router itself (not transit traffic). So we must send the
# DNS query directly to the router's IP (port 53) to prime the cache.
import struct as _struct

def _build_dns_query(hostname, qid):
    parts = hostname.rstrip(".").split(".")
    q = b""
    for p in parts:
        b = p.encode("ascii")
        q += bytes([len(b)]) + b
    q += b"\x00"  # root label terminator
    q += b"\x00\x01"   # QTYPE=A
    q += b"\x00\x01"   # QCLASS=IN
    header = _struct.pack(">HHHHHH", qid, 0x0100, 1, 0, 0, 0)  # RD=1
    return header + q

def _parse_dns_answer_a(pkt):
    """Extract the first A record IP from a DNS response, or None."""
    try:
        flags = _struct.unpack(">H", pkt[2:4])[0]
        qdcount = _struct.unpack(">H", pkt[4:6])[0]
        ancount = _struct.unpack(">H", pkt[6:8])[0]
        if (flags & 0x000F) != 0 or ancount == 0:
            return None  # RCODE != 0 or no answers
        # Skip header + QDs
        off = 12
        for _ in range(qdcount):
            while pkt[off] != 0:
                off += pkt[off] + 1
            off += 1 + 4  # terminator + QTYPE + QCLASS
        for _ in range(ancount):
            # Name (may be pointer 0xC0??)
            if pkt[off] & 0xC0:
                off += 2
            else:
                while pkt[off] != 0:
                    off += pkt[off] + 1
                off += 1
            rtype = _struct.unpack(">H", pkt[off:off+2])[0]
            off += 2 + 2 + 4  # TYPE + CLASS + TTL
            rdlen = _struct.unpack(">H", pkt[off:off+2])[0]
            off += 2
            if rtype == 1 and rdlen == 4:  # A record
                return "{}.{}.{}.{}".format(*pkt[off:off+4])
            off += rdlen
    except Exception:
        return None
    return None

def maybe_dns_prime(hostname):
    """Send a DNS query directly to a public upstream (1.1.1.1) so the
    response transits through the router. The daemon's AF_PACKET sniffer
    captures transit DNS, populating the IP→service cache before our
    test traffic begins.

    Returns the resolved IP, or None on failure."""
    if not hostname:
        return None
    import random as _r
    qid = _r.randint(1, 0xFFFF)
    pkt = _build_dns_query(hostname, qid)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2)
    try:
        sock.sendto(pkt, ("1.1.1.1", 53))
        resp, _ = sock.recvfrom(2048)
        return _parse_dns_answer_a(resp)
    except Exception:
        # Fallback to router's resolver if upstream is blocked
        try:
            sock2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock2.settimeout(2)
            sock2.sendto(pkt, (ROUTER_IP, 53))
            resp, _ = sock2.recvfrom(2048)
            sock2.close()
            return _parse_dns_answer_a(resp)
        except Exception:
            return None
    finally:
        sock.close()

# ── Sampling ──────────────────────────────────────────────────────────────────
def load_and_sample(csv_path, n_per_persona, seed):
    """If a pre-sampled small CSV exists (from prepare_blind_sample.py),
    load that. Otherwise parse the full 1.2 GB Unicauca CSV in memory."""
    import pandas as pd
    presampled = os.path.join(
        os.path.dirname(csv_path) if os.path.dirname(csv_path) else ".",
        f"blind_sample_n{n_per_persona}_seed{seed}.csv")
    if os.path.exists(presampled):
        print(f"[load] using pre-sampled file: {presampled}", flush=True)
        out = pd.read_csv(presampled)
        print(f"[load] {len(out):,} flows × {out['_gt_persona'].nunique()} personas (fast path)",
              flush=True)
        return out

    print(f"[load] reading {csv_path} (~1.2 GB) — slow path, takes 30-60s …", flush=True)
    print(f"[hint] run prepare_blind_sample.py once to skip this on future runs.",
          flush=True)
    df = pd.read_csv(csv_path, low_memory=False)
    print(f"[load] {len(df):,} rows total", flush=True)

    print(f"[label] applying ground-truth rules …", flush=True)
    df["_persona"] = df.apply(_ground_truth, axis=1)
    df = df[df["_persona"] != "unknown"].copy()
    print(f"[label] {len(df):,} rows with known persona", flush=True)

    for col in ["proto", "dst_port", "avg_ps", "octetTotalCount", "flowDuration"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    df["proto"]          = df["proto"].fillna(6).astype(int)
    df["dst_port"]       = df["dst_port"].fillna(80).astype(int).clip(lower=1, upper=65535)
    df["avg_ps"]         = df["avg_ps"].fillna(128)
    df["octetTotalCount"]= df["octetTotalCount"].fillna(0)
    df["flowDuration"]   = df["flowDuration"].fillna(1).clip(lower=1e-6)
    df["_bw_bps"] = df["octetTotalCount"] * 8.0 / df["flowDuration"]

    print(f"[sample] stratified random N={n_per_persona} per persona, seed={seed} …",
          flush=True)
    samples = []
    for persona, grp in df.groupby("_persona"):
        n = min(n_per_persona, len(grp))
        sub = grp.sample(n=n, random_state=seed).copy()
        sub["_gt_persona"] = persona
        samples.append(sub)
    out = pd.concat(samples, ignore_index=True)
    print(f"[sample] {len(out):,} flows selected across {out['_gt_persona'].nunique()} personas",
          flush=True)
    return out

# ── Per-flow test ─────────────────────────────────────────────────────────────
def replay_one_flow(row, mode, lan_ip):
    """Replay a single flow against the router and capture classifier output.

    Attribution: read state.flows[] (per-flow service classification) and
    match by (src=lan_ip, dst, dport, proto) — unique per test flow.
    This isolates our synthetic flow from other traffic on the same laptop."""
    proto    = int(row["proto"])
    dst_port = int(row["dst_port"])
    pkt_size = max(PKT_SIZE_MIN, min(PKT_SIZE_MAX, int(row["avg_ps"])))
    bw_bps   = max(BW_BPS_MIN,  min(BW_BPS_MAX,  int(row["_bw_bps"])))
    web_svc  = str(row.get("web_service", "")).strip()

    # Mode B: trigger an organic DNS lookup using a real hostname for this service
    dns_resolved_ip = None
    dns_prime_host  = None
    if mode == "B":
        candidates = SERVICE_HOSTNAMES.get(web_svc, SERVICE_HOSTNAMES["_default_web"])
        if candidates:
            dns_prime_host  = random.choice(candidates)
            dns_resolved_ip = maybe_dns_prime(dns_prime_host)

    # Pick destination IP:
    # - Mode B: use the DNS-resolved IP so the router's DNS snooper links
    #   dst_ip → hostname → service classification.
    # - Mode A (or B fallback): UDP→8.8.8.8, TCP→1.1.1.1 for reachable ports.
    #
    # For TCP flows whose dst_port is not in the Cloudflare whitelist AND
    # no resolved IP is available, we flag as tcp_unreachable since a SYN
    # to an unresponsive port yields too little data for classification.
    tcp_unreachable = False
    if dns_resolved_ip:
        dst_ip = dns_resolved_ip
    elif proto == 17:
        dst_ip = UDP_TARGET
    elif TCP_REACHABLE_PORTS is None or dst_port in TCP_REACHABLE_PORTS:
        # Either testbed is open (TCP_REACHABLE_PORTS=None) or the port is
        # in the constrained-network whitelist. Use the TCP fallback target.
        dst_ip = TCP_TARGET
    else:
        tcp_unreachable = True
        dst_ip = TCP_TARGET

    # Give the AF_PACKET DNS sniffer a moment to parse the response and
    # insert the IP→service mapping into the cache before traffic begins.
    # Without this, the first classifier_tick cycles see dns_hint=UNKNOWN
    # and the flow is skipped (line: don't track idle unknowns).
    if mode == "B" and dns_resolved_ip:
        time.sleep(0.3)

    # Send traffic in a background thread so we can read state mid-flight.
    # Flows must be ACTIVE when state is read (short TCP connections get
    # evicted from the classifier flow table after close).
    # Capture actual source port for 5-tuple state.flows lookup.
    t_start = time.time()
    sent_counter = {"n": 0, "sport": 0}
    def traffic_worker():
        duration = FLOW_PRE_READ_S + FLOW_POST_READ_S
        if tcp_unreachable:
            return
        if proto == 17:
            n, sp = send_udp_stream(dst_ip, dst_port, pkt_size, bw_bps, duration)
        else:
            n, sp = send_tcp_stream(dst_ip, dst_port, pkt_size, bw_bps, duration)
        sent_counter["n"] = n
        sent_counter["sport"] = sp

    th = threading.Thread(target=traffic_worker, daemon=True)
    th.start()

    # Wait long enough for the classifier (sample_hz=2 → 0.5s/cycle) to pick up
    # the flow. 1.8s ≈ 3–4 tick cycles, enough for service_t to stabilize.
    time.sleep(FLOW_PRE_READ_S)
    state_after = get_router_state()
    t_state = time.time()
    # Let traffic complete naturally
    th.join(timeout=FLOW_POST_READ_S + 1.0)
    sent = sent_counter["n"]
    actual_sport = sent_counter["sport"]
    t_traffic_end = time.time()

    # Per-flow attribution — match by 5-tuple to isolate from concurrent
    # host traffic. If actual_sport unknown (TCP connect failed), fall back
    # to 4-tuple matching with preference for non-unknown services.
    predicted       = "unknown"
    matched_entries = 0
    flow_match      = None
    for f in state_after.get("flows", []):
        if not (f.get("src") == lan_ip and
                f.get("dst") == dst_ip and
                int(f.get("dport", 0)) == dst_port and
                int(f.get("proto", 0)) == proto):
            continue
        matched_entries += 1
        # 5-tuple exact match wins
        if actual_sport and int(f.get("sport", 0)) == actual_sport:
            flow_match = f
            break
        # Fallback: prefer flows with a definitive (non-unknown) service
        if flow_match is None or (
            flow_match.get("service") in (None, "unknown") and
            f.get("service") not in (None, "unknown")):
            flow_match = f

    if flow_match:
        predicted = flow_match.get("service", "unknown")

    # Also record device-level persona as secondary signal
    device_persona = "unknown"
    device_bw      = 0.0
    device_flows   = 0
    for d in state_after.get("devices", []):
        if d.get("ip") == lan_ip:
            device_persona = d.get("persona", "unknown")
            device_bw      = float(d.get("rx_bps", 0)) + float(d.get("tx_bps", 0))
            device_flows   = int(d.get("flows", 0))
            break

    return {
        "ts":              time.time(),
        "gt_persona":      row["_gt_persona"],
        "gt_myco_label":   row["_gt_persona"],   # direct 1:1 with service enum
        "predicted":       predicted,
        "flow_matched":    (flow_match is not None),
        "n_flow_matches":  matched_entries,
        "flow_mark":       (flow_match or {}).get("mark"),
        "flow_stable":     (flow_match or {}).get("stable"),
        "flow_rtt_ms":     (flow_match or {}).get("rtt_ms"),
        "device_persona":  device_persona,
        "device_bw_bps":   device_bw,
        "device_flows":    device_flows,
        "proto":           proto,
        "dst_port":        dst_port,
        "dst_ip":          dst_ip,
        "pkt_size":        pkt_size,
        "bw_bps":          bw_bps,
        "web_service":     web_svc,
        "dns_prime_host":  dns_prime_host,
        "packets_sent":    sent,
        "tcp_unreachable": tcp_unreachable,
        "latency_traffic": round(t_traffic_end - t_start, 3),
        "latency_total":   round(t_state - t_start, 3),
        "mode":            mode,
    }

# ── Main run loop ─────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Blinded in-vivo MycoFlow validation")
    parser.add_argument("--mode", choices=["A", "B"], required=True,
                        help="A: IP-only (no DNS), B: hostname (organic DNS)")
    parser.add_argument("--csv", default=str(DEFAULT_CSV))
    parser.add_argument("--n-per-persona", type=int, default=150)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--output", default=None,
                        help="JSONL output path (default: results/blind_test/mode_{A,B}_n{N}_seed{S}.jsonl)")
    parser.add_argument("--resume", action="store_true",
                        help="Skip flow IDs already in the output file")
    parser.add_argument("--limit", type=int, default=None,
                        help="Stop after this many flows (for smoke-testing)")
    args = parser.parse_args()

    if not args.output:
        args.output = str(RESULTS_DIR / f"mode_{args.mode}_n{args.n_per_persona}_seed{args.seed}.jsonl")

    # Sanity checks: only require sshpass on the password-auth path.
    if not USE_KEYAUTH and subprocess.run(["which", "sshpass"], capture_output=True).returncode != 0:
        sys.exit("[!] sshpass missing — apt install sshpass (or set MYCOFLOW_SSH_KEYAUTH=1 for key-based)")
    _, _, rc = router_ssh("echo ping", timeout=5)
    if rc != 0:
        sys.exit(f"[!] cannot reach router {ROUTER_IP}")
    print(f"[✓] router {ROUTER_IP} reachable")

    lan_ip = get_local_lan_ip()
    print(f"[✓] local LAN IP: {lan_ip}")

    # Load and sample
    df = load_and_sample(args.csv, args.n_per_persona, args.seed)
    if args.limit:
        df = df.head(args.limit)

    # Resume support
    done_idx = set()
    if args.resume and os.path.exists(args.output):
        with open(args.output) as f:
            for line in f:
                try:
                    d = json.loads(line)
                    done_idx.add(d.get("_row_id"))
                except Exception:
                    pass
        print(f"[resume] {len(done_idx)} flows already done, skipping")

    out_fp = open(args.output, "a", buffering=1)  # line-buffered
    n_total   = len(df)
    eta_s     = (n_total - len(done_idx)) * FLOW_TOTAL_S
    print(f"[run] mode={args.mode}  flows={n_total}  ETA≈{eta_s/60:.1f} min")
    print(f"[run] output: {args.output}")

    # Track persona match rate online for visibility
    correct = 0
    seen    = 0
    t0      = time.time()
    for idx, (_, row) in enumerate(df.iterrows(), 1):
        row_id = f"{args.mode}_{idx:06d}"
        if row_id in done_idx:
            continue
        try:
            result = replay_one_flow(row, args.mode, lan_ip)
        except Exception as e:
            result = {"_row_id": row_id, "error": str(e), "gt_persona": row["_gt_persona"]}
        result["_row_id"] = row_id
        out_fp.write(json.dumps(result) + "\n")

        seen += 1
        if result.get("predicted") == result.get("gt_myco_label"):
            correct += 1

        if idx % 10 == 0 or idx == n_total:
            elapsed = time.time() - t0
            rate    = idx / elapsed if elapsed > 0 else 0
            eta     = (n_total - idx) / rate if rate > 0 else 0
            acc     = correct / seen if seen > 0 else 0
            print(f"  [{idx:>5}/{n_total}] gt={result.get('gt_persona','?'):<16} "
                  f"pred={result.get('predicted','?'):<10} "
                  f"acc-so-far={acc:.3f}  ETA={eta/60:.1f} min", flush=True)

    out_fp.close()
    print(f"\n[done] mode {args.mode} complete — {seen} flows in {(time.time()-t0)/60:.1f} min")
    print(f"[done] online accuracy estimate: {correct/max(1,seen):.4f}")
    print(f"[done] results: {args.output}")
    print(f"[done] run analyzer: python3 router_blind_analyze.py --mode-a <A.jsonl> --mode-b <B.jsonl>")

if __name__ == "__main__":
    main()
