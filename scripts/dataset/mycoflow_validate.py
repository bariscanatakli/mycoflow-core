#!/usr/bin/env python3
"""
mycoflow_validate.py
--------------------
Validates MycoFlow's three-signal classifier (port-hint + behavioral)
against the Unicauca labeled dataset.

Methodology:
  1. Load Unicauca flows.
  2. Assign ground-truth MycoFlow persona labels (from Unicauca category/service).
  3. Run MycoFlow's port-hint + behavioral heuristic on the same features.
  4. Compare predicted vs. ground-truth.
  5. Report accuracy, per-class precision/recall/F1, confusion matrix.

Outputs:
  results/unicauca/validation_report.txt
  results/unicauca/confusion_matrix.png
  results/unicauca/per_class_metrics.png
  results/unicauca/validation_results.csv
"""

import os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as mticker
from sklearn.metrics import (classification_report, confusion_matrix,
                             accuracy_score, ConfusionMatrixDisplay)
import warnings
warnings.filterwarnings("ignore")

# ── Paths ─────────────────────────────────────────────────────────────────────
DATASET = os.path.join(os.path.dirname(__file__), "../../archive/"
                       "Unicauca-dataset-April-June-2019-Network-flows.csv")
OUTDIR  = os.path.join(os.path.dirname(__file__), "../../results/unicauca")
os.makedirs(OUTDIR, exist_ok=True)

# ── Port Hint Table (mirrors myco_hint.c) ─────────────────────────────────────
# (proto, dport) → persona
PORT_HINTS = {
    # game_rt
    (17, 27015): "game_rt", (17, 27016): "game_rt",
    (17, 27017): "game_rt", (17, 27020): "game_rt",
    (17, 27021): "game_rt", (17, 27040): "game_rt",
    (17, 7777):  "game_rt", (17, 25565): "game_rt",   # Minecraft
    (17, 3724):  "game_rt",                            # WoW
    # voip_call
    (17, 3478):  "voip_call", (17, 3479): "voip_call",
    (17, 5004):  "voip_call", (17, 5005): "voip_call",
    (17, 19302): "voip_call", (17, 5060): "voip_call",
    (17, 5061):  "voip_call", (6,  5060): "voip_call",
    # video_conf
    (6,  443):   None,   # HTTPS — need behavioral to decide
    # bulk / file
    (6,  21):    "bulk_dl",  (6,  20):  "bulk_dl",    # FTP
    (6,  8080):  "bulk_dl",
    # torrent
    (6,  6881):  "torrent",  (6,  6882): "torrent",
    (6,  6883):  "torrent",  (17, 6881): "torrent",
    (17, 6969):  "torrent",
    # system
    (17, 53):    "system",   (6,  53):  "system",
    (17, 67):    "system",   (17, 68):  "system",
    (17, 123):   "system",   (17, 5353): "system",
    (6,  139):   "system",   (17, 137): "system",
    (17, 5355):  "system",
}

# ── Behavioral Classifier (mirrors myco_profile.c heuristics) ─────────────────
def behavioral_classify(proto, dport, avg_ps, pkts, octets, duration_s):
    """
    Simplified MycoFlow behavioral fingerprint.
    Returns a persona string.
    """
    bw_bps = (octets * 8 / duration_s) if duration_s > 0 else 0

    # game_rt: small UDP, low bandwidth
    if proto == 17 and avg_ps < 150 and bw_bps < 500_000:
        return "game_rt"

    # voip_call: very small UDP, very low bandwidth
    if proto == 17 and avg_ps < 200 and bw_bps < 200_000:
        return "voip_call"

    # video_conf: medium UDP, medium bandwidth
    if proto == 17 and 200 <= avg_ps <= 800 and bw_bps < 3_000_000:
        return "video_conf"

    # torrent: many small-medium TCP, high packet count
    if proto == 6 and pkts > 100 and avg_ps < 500 and bw_bps < 1_000_000:
        return "torrent"

    # bulk_dl: large packets, high bandwidth
    if avg_ps > 800 and bw_bps > 500_000:
        return "bulk_dl"

    # video_live / video_vod: large packets, moderate-high bandwidth
    if avg_ps > 500 and 200_000 < bw_bps < 25_000_000:
        return "video_live"

    # file_sync: medium TCP, moderate bandwidth
    if proto == 6 and 200 < avg_ps <= 800 and bw_bps < 1_000_000:
        return "file_sync"

    # web_interactive: small-medium TCP
    if proto == 6 and avg_ps < 600:
        return "web_interactive"

    return "unknown"


def mycoflow_classify(row):
    """
    Three-signal classifier: port-hint (w=0.6) + behavioral (w=0.4).
    Simplified version of myco_classifier.c.
    """
    proto    = int(row.get("proto", 0))
    dport    = int(row.get("dst_port", 0))
    avg_ps   = float(row.get("avg_ps", 0))
    pkts     = float(row.get("pktTotalCount", 1) or 1)
    octets   = float(row.get("octetTotalCount", 0))
    duration = float(row.get("flowDuration", 1) or 1)

    # Signal 1: Port hint
    port_class = PORT_HINTS.get((proto, dport), None)

    # Signal 2: Behavioral fingerprint
    behav_class = behavioral_classify(proto, dport, avg_ps, pkts, octets, duration)

    # Weighted decision: port hint wins if present and agrees
    if port_class and port_class != "unknown":
        if behav_class == port_class or behav_class == "unknown":
            return port_class          # w=1.0 port hint
        # Disagree: port hint higher weight (0.6 vs 0.4)
        return port_class
    return behav_class


# ── Ground-Truth Mapper (same as unicauca_explore.py) ─────────────────────────
GAMING_SERVICES    = {"Steam", "Xbox", "Playstation", "Starcraft"}
VOIP_SERVICES      = {"Skype", "SkypeCall", "IMO", "GoogleHangoutDuo",
                      "WhatsAppCall", "SIP", "H323", "Webex", "FaceTime"}
VIDEO_SERVICES     = {"YouTube", "Twitch", "RTMP"}
STREAMING_SERVICES = {"AppleiTunes", "Netflix", "Spotify", "GooglePlay"}
CLOUD_SERVICES     = {"Dropbox", "GoogleDrive", "OneDrive", "UbuntuONE",
                      "iCloud", "AmazonS3", "Box"}
TORRENT_SERVICES   = {"BitTorrent", "uTorrent", "eDonkey", "Gnutella",
                      "Kazaa", "SoulSeek"}

def ground_truth(row):
    cat  = str(row.get("category", "")).strip()
    svc  = str(row.get("web_service", "")).strip()
    app  = str(row.get("application_protocol", "")).strip()
    proto  = int(row.get("proto", 0))
    dport  = int(row.get("dst_port", 0))
    avg_ps = float(row.get("avg_ps", 0))
    octets = float(row.get("octetTotalCount", 0))
    pkts   = float(row.get("pktTotalCount", 1) or 1)

    if svc in GAMING_SERVICES:
        if proto == 17 or dport in {27015, 27016, 27020, 27021, 27040}:
            return "game_rt"
        return "game_launcher"
    if cat == "VoIP" or svc in VOIP_SERVICES:
        if svc in {"GoogleHangoutDuo", "Webex", "FaceTime"}:
            return "video_conf"
        return "voip_call"
    if svc in VIDEO_SERVICES or (cat == "Media" and svc == "YouTube"):
        return "video_live"
    if svc in STREAMING_SERVICES or cat == "Streaming":
        return "video_vod"
    if cat == "Download-FileTransfer-FileSharing" or svc in TORRENT_SERVICES:
        return "torrent"
    if svc in CLOUD_SERVICES:
        return "file_sync"
    if cat == "SoftwareUpdate":
        return "bulk_dl"
    if cat == "Cloud":
        bpp = octets / pkts if pkts else 0
        return "file_sync" if bpp < 500 else "bulk_dl"
    if cat in {"Web", "SocialNetwork", "Chat", "Email", "Collaborative"}:
        return "web_interactive"
    if cat in {"Network", "System", "RPC"} or svc in {"DNS","NetBIOS","STUN","NTP","DHCP"}:
        return "system"
    return "unknown"


# ── Load & Classify ───────────────────────────────────────────────────────────
print("Loading dataset …")
df = pd.read_csv(DATASET, low_memory=False)
print(f"  Total rows: {len(df):,}")

print("Computing ground truth labels …")
df["gt_persona"] = df.apply(ground_truth, axis=1)

print("Running MycoFlow classifier …")
df["pred_persona"] = df.apply(mycoflow_classify, axis=1)

# Keep only rows where ground truth is not 'unknown'
df_eval = df[df["gt_persona"] != "unknown"].copy()
print(f"  Evaluable rows (gt != unknown): {len(df_eval):,}")

y_true = df_eval["gt_persona"]
y_pred = df_eval["pred_persona"]

# ── Metrics ───────────────────────────────────────────────────────────────────
acc = accuracy_score(y_true, y_pred)
labels = sorted(y_true.unique())
report = classification_report(y_true, y_pred, labels=labels, digits=3)

print(f"\n── Overall Accuracy: {acc*100:.2f}% ──\n")
print(report)

with open(os.path.join(OUTDIR, "validation_report.txt"), "w") as f:
    f.write(f"MycoFlow Classifier Validation vs Unicauca Dataset\n")
    f.write(f"{'='*52}\n\n")
    f.write(f"Total flows evaluated: {len(df_eval):,}\n")
    f.write(f"Overall accuracy:      {acc*100:.2f}%\n\n")
    f.write(report)
    f.write(f"\nPersona distribution (ground truth):\n")
    f.write(y_true.value_counts().to_string())
print("  → Saved validation_report.txt")

# ── Confusion Matrix ──────────────────────────────────────────────────────────
cm = confusion_matrix(y_true, y_pred, labels=labels)
fig, ax = plt.subplots(figsize=(12, 10))
disp = ConfusionMatrixDisplay(confusion_matrix=cm, display_labels=labels)
disp.plot(ax=ax, colorbar=True, cmap="Blues", values_format="d")
ax.set_title(f"MycoFlow Classifier — Confusion Matrix\n"
             f"Unicauca Dataset  |  Overall Accuracy: {acc*100:.1f}%",
             fontsize=12, fontweight="bold")
plt.xticks(rotation=35, ha="right", fontsize=8)
plt.yticks(fontsize=8)
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "confusion_matrix.png"), dpi=150)
plt.close()
print("  → Saved confusion_matrix.png")

# ── Per-Class Bar Chart ────────────────────────────────────────────────────────
from sklearn.metrics import precision_recall_fscore_support
prec, rec, f1, supp = precision_recall_fscore_support(
    y_true, y_pred, labels=labels, zero_division=0)

x = np.arange(len(labels))
w = 0.25
fig, ax = plt.subplots(figsize=(13, 5))
ax.bar(x - w,   prec, w, label="Precision", color="steelblue")
ax.bar(x,        rec, w, label="Recall",    color="darkorange")
ax.bar(x + w,    f1,  w, label="F1-Score",  color="forestgreen")
ax.set_title(f"Per-Class Metrics — MycoFlow vs. Unicauca  "
             f"(Accuracy: {acc*100:.1f}%)",
             fontsize=12, fontweight="bold")
ax.set_xticks(x)
ax.set_xticklabels(labels, rotation=30, ha="right", fontsize=9)
ax.set_ylim(0, 1.05)
ax.set_ylabel("Score")
ax.axhline(0.8, color="red", linestyle="--", linewidth=0.8, label="0.8 threshold")
ax.legend()
# Annotate support counts
for i, s in enumerate(supp):
    ax.text(x[i], -0.08, f"n={s:,}", ha="center", fontsize=6,
            transform=ax.get_xaxis_transform())
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "per_class_metrics.png"), dpi=150)
plt.close()
print("  → Saved per_class_metrics.png")

# ── Save results ──────────────────────────────────────────────────────────────
result_df = pd.DataFrame({
    "persona": labels,
    "precision": prec,
    "recall": rec,
    "f1": f1,
    "support": supp,
}).round(3)
result_df.to_csv(os.path.join(OUTDIR, "validation_results.csv"), index=False)
print("  → Saved validation_results.csv")

print(f"\n✓ All outputs in: {OUTDIR}")
print(f"\n── Quick Summary ──")
for lbl, p, r, f, s in zip(labels, prec, rec, f1, supp):
    bar = "█" * int(f * 20)
    print(f"  {lbl:<18} F1={f:.2f}  {bar:<20}  (n={s:,})")
