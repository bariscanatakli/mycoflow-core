#!/usr/bin/env python3
"""
unicauca_explore.py
-------------------
Exploratory Data Analysis for the Unicauca April-June 2019
Network Flows dataset.

Outputs:
  results/unicauca_label_dist.png   — category / web_service bar charts
  results/unicauca_persona_dist.png — MycoFlow persona mapping distribution
  results/unicauca_features.png     — Feature distributions per persona
  results/unicauca_summary.csv      — Per-persona flow counts + feature stats
"""

import os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
import warnings
warnings.filterwarnings("ignore")

# ── Paths ─────────────────────────────────────────────────────────────────────
DATASET = os.path.join(os.path.dirname(__file__), "../../archive/"
                       "Unicauca-dataset-April-June-2019-Network-flows.csv")
OUTDIR  = os.path.join(os.path.dirname(__file__), "../../results/unicauca")
os.makedirs(OUTDIR, exist_ok=True)

# ── Load ──────────────────────────────────────────────────────────────────────
print("Loading dataset …")
df = pd.read_csv(DATASET, low_memory=False)
print(f"  Rows: {len(df):,}  |  Columns: {df.shape[1]}")

# ── MycoFlow Persona Mapping ───────────────────────────────────────────────────
GAMING_SERVICES   = {"Steam", "Xbox", "Playstation", "Starcraft"}
VOIP_SERVICES     = {"Skype", "SkypeCall", "IMO", "GoogleHangoutDuo",
                     "WhatsAppCall", "SIP", "H323", "Webex", "FaceTime"}
VIDEO_SERVICES    = {"YouTube", "Twitch", "RTMP"}
STREAMING_SERVICES= {"AppleiTunes", "Netflix", "Spotify", "GooglePlay"}
CLOUD_SERVICES    = {"Dropbox", "GoogleDrive", "OneDrive", "UbuntuONE",
                     "iCloud", "AmazonS3", "Box"}
TORRENT_SERVICES  = {"BitTorrent", "uTorrent", "eDonkey", "Gnutella",
                     "Kazaa", "SoulSeek"}
BULK_SERVICES     = {"HTTP", "FTP", "HTTP_Proxy", "WindowsUpdate",
                     "UbuntuUpdate", "SoftwareUpdate", "AppleUpdate"}
SYSTEM_SERVICES   = {"DNS", "NetBIOS", "STUN", "NTP", "DHCP",
                     "LLMNR", "mDNS"}

def assign_persona(row):
    cat = str(row.get("category", "")).strip()
    svc = str(row.get("web_service", "")).strip()
    app = str(row.get("application_protocol", "")).strip()
    proto  = int(row.get("proto", 0))
    dport  = int(row.get("dst_port", 0))
    avg_ps = float(row.get("avg_ps", 0))
    octets = float(row.get("octetTotalCount", 0))
    pkts   = float(row.get("pktTotalCount", 0) or 1)

    # Game signals
    if svc in GAMING_SERVICES:
        if proto == 17 or dport in {27015, 27016, 27020, 27021, 27040}:
            return "game_rt"
        return "game_launcher"

    # VoIP / video conference
    if cat == "VoIP" or svc in VOIP_SERVICES:
        if app == "STUN" or proto == 17 and dport in {3478, 5004, 5005, 19302}:
            return "voip_call"
        if svc in {"GoogleHangoutDuo", "Webex", "FaceTime"}:
            return "video_conf"
        return "voip_call"

    # Video live / vod
    if svc in VIDEO_SERVICES or (cat == "Media" and svc == "YouTube"):
        return "video_live"
    if svc in STREAMING_SERVICES or cat == "Streaming":
        return "video_vod"

    # Torrent / P2P
    if cat == "Download-FileTransfer-FileSharing" or svc in TORRENT_SERVICES:
        return "torrent"

    # File sync (Cloud)
    if svc in CLOUD_SERVICES:
        return "file_sync"

    # Bulk download
    if cat == "SoftwareUpdate" or svc in BULK_SERVICES:
        return "bulk_dl"
    if cat == "Cloud":
        bpp = octets / pkts if pkts else 0
        return "file_sync" if bpp < 500 else "bulk_dl"

    # Web / social / chat
    if cat in {"Web", "SocialNetwork", "Chat", "Email", "Collaborative"}:
        return "web_interactive"

    # System
    if cat in {"Network", "System", "RPC"} or svc in SYSTEM_SERVICES:
        return "system"

    return "unknown"

print("Assigning personas …")
df["persona"] = df.apply(assign_persona, axis=1)

# ── 1. Persona Distribution ───────────────────────────────────────────────────
persona_counts = df["persona"].value_counts()
print("\n── Persona Distribution ──")
print(persona_counts.to_string())

fig, ax = plt.subplots(figsize=(10, 5))
bars = ax.bar(persona_counts.index, persona_counts.values,
              color=plt.cm.tab10.colors[:len(persona_counts)])
ax.set_title("MycoFlow Persona Distribution — Unicauca Dataset", fontsize=13, fontweight="bold")
ax.set_xlabel("Persona")
ax.set_ylabel("Flow Count")
ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{int(x):,}"))
for bar in bars:
    ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 50,
            f"{int(bar.get_height()):,}", ha="center", va="bottom", fontsize=8)
plt.xticks(rotation=30, ha="right")
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "persona_distribution.png"), dpi=150)
plt.close()
print(f"  → Saved persona_distribution.png")

# ── 2. Category + Web Service Heatmap ────────────────────────────────────────
fig, axes = plt.subplots(1, 2, figsize=(16, 6))

# Category
cat_counts = df["category"].value_counts().head(15)
axes[0].barh(cat_counts.index[::-1], cat_counts.values[::-1],
             color="steelblue")
axes[0].set_title("Top 15 Unicauca Categories")
axes[0].set_xlabel("Flow Count")
axes[0].xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{int(x):,}"))

# Web Service
svc_counts = df["web_service"].value_counts().head(20)
axes[1].barh(svc_counts.index[::-1], svc_counts.values[::-1],
             color="darkorange")
axes[1].set_title("Top 20 Web Services")
axes[1].set_xlabel("Flow Count")
axes[1].xaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{int(x):,}"))

plt.suptitle("Unicauca Dataset — Label Distributions", fontsize=13, fontweight="bold")
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "label_distribution.png"), dpi=150)
plt.close()
print(f"  → Saved label_distribution.png")

# ── 3. Feature Distributions per Persona ─────────────────────────────────────
FEATURES = ["avg_ps", "pktTotalCount", "octetTotalCount", "flowDuration"]
FEATURE_LABELS = ["Avg Packet Size (B)", "Total Packets", "Total Bytes", "Flow Duration (s)"]
PERSONAS_PLOT = ["game_rt", "voip_call", "video_conf", "video_live",
                 "video_vod", "bulk_dl", "file_sync", "torrent",
                 "web_interactive", "system"]

fig, axes = plt.subplots(2, 2, figsize=(14, 10))
axes = axes.flatten()

for ax, feat, label in zip(axes, FEATURES, FEATURE_LABELS):
    data = []
    labels = []
    for p in PERSONAS_PLOT:
        subset = df[df["persona"] == p][feat].dropna()
        if len(subset) > 5:
            vals = np.clip(subset.values, 0, subset.quantile(0.99))
            data.append(vals)
            labels.append(f"{p}\n(n={len(subset):,})")
    ax.boxplot(data, labels=labels, patch_artist=True,
               boxprops=dict(facecolor="lightsteelblue", color="navy"),
               medianprops=dict(color="red", linewidth=2),
               flierprops=dict(marker=".", markersize=2, alpha=0.3))
    ax.set_title(label, fontsize=11)
    ax.tick_params(axis="x", labelsize=7, rotation=20)
    ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{int(x):,}"))

plt.suptitle("Feature Distributions per MycoFlow Persona — Unicauca Dataset",
             fontsize=13, fontweight="bold")
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "feature_distributions.png"), dpi=150)
plt.close()
print(f"  → Saved feature_distributions.png")

# ── 4. Protocol Split per Persona ────────────────────────────────────────────
proto_map = {6: "TCP", 17: "UDP", 1: "ICMP"}
df["proto_name"] = df["proto"].map(proto_map).fillna("Other")

pivot = df.groupby(["persona", "proto_name"]).size().unstack(fill_value=0)
pivot = pivot.loc[pivot.sum(axis=1) > 0]

fig, ax = plt.subplots(figsize=(11, 5))
pivot.plot(kind="bar", ax=ax, colormap="Set2", edgecolor="grey", linewidth=0.3)
ax.set_title("Protocol Split per Persona", fontsize=13, fontweight="bold")
ax.set_xlabel("Persona")
ax.set_ylabel("Flow Count")
ax.yaxis.set_major_formatter(mticker.FuncFormatter(lambda x, _: f"{int(x):,}"))
plt.xticks(rotation=30, ha="right")
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "proto_per_persona.png"), dpi=150)
plt.close()
print(f"  → Saved proto_per_persona.png")

# ── 5. Summary CSV ────────────────────────────────────────────────────────────
summary = df.groupby("persona").agg(
    flow_count       = ("pktTotalCount", "count"),
    avg_pkt_size_med = ("avg_ps",         "median"),
    avg_pkt_size_mean= ("avg_ps",         "mean"),
    total_pkts_med   = ("pktTotalCount",  "median"),
    total_bytes_med  = ("octetTotalCount","median"),
    duration_med_s   = ("flowDuration",   "median"),
    udp_pct          = ("proto_name",     lambda x: (x == "UDP").mean() * 100),
).round(2)
summary = summary.sort_values("flow_count", ascending=False)
summary.to_csv(os.path.join(OUTDIR, "persona_summary.csv"))
print(f"\n── Per-Persona Summary ──")
print(summary.to_string())
print(f"\n  → Saved persona_summary.csv")
print(f"\n✓ All outputs in: {OUTDIR}")
