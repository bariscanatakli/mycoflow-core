#!/usr/bin/env python3
"""
mycoflow_validate_dns.py
------------------------
Compares two classifier modes on the Unicauca dataset:

  Mode A — Port-hint + Behavioral only (offline, no DNS)
            Simulates what MycoFlow v2 (per-device only) can do without
            knowing the remote hostname.

  Mode B — Port-hint + Behavioral + DNS snooping (live daemon)
            Simulates MycoFlow v3: uses the 'web_service' column as a proxy
            for what passive DNS snooping would resolve at runtime.
            This is valid because MycoFlow's DNS cache maps hostnames to
            service classes the same way ntopng identified them in Unicauca.

The gap between A and B quantifies the contribution of the DNS signal
(weight 0.6 in the three-signal voter).

Outputs:
  results/unicauca/dns_comparison.png
  results/unicauca/dns_validation_report.txt
  results/unicauca/dns_validation_results.csv
"""

import os
import pandas as pd
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from sklearn.metrics import (classification_report, accuracy_score,
                             precision_recall_fscore_support)
import warnings
warnings.filterwarnings("ignore")

DATASET = os.path.join(os.path.dirname(__file__), "../../archive/"
                       "Unicauca-dataset-April-June-2019-Network-flows.csv")
OUTDIR  = os.path.join(os.path.dirname(__file__), "../../results/unicauca")
os.makedirs(OUTDIR, exist_ok=True)

# ── DNS Service → Persona Map (mirrors myco_dns.c suffix table) ───────────────
DNS_MAP = {
    # game_rt
    "Steam": "game_rt", "Valve": "game_rt", "Starcraft": "game_rt",
    # game_launcher
    "Xbox": "game_launcher", "Playstation": "game_launcher",
    # voip_call
    "Skype": "voip_call", "SkypeCall": "voip_call",
    "IMO": "voip_call", "WhatsAppCall": "voip_call",
    "SIP": "voip_call", "H323": "voip_call",
    # video_conf
    "GoogleHangoutDuo": "video_conf", "Webex": "video_conf",
    "FaceTime": "video_conf", "Zoom": "video_conf",
    # video_live
    "YouTube": "video_live", "Twitch": "video_live", "RTMP": "video_live",
    # video_vod
    "AppleiTunes": "video_vod", "Netflix": "video_vod",
    "Spotify": "video_vod", "GooglePlay": "video_vod",
    # file_sync
    "Dropbox": "file_sync", "GoogleDrive": "file_sync",
    "OneDrive": "file_sync", "UbuntuONE": "file_sync",
    "iCloud": "file_sync", "AmazonS3": "file_sync",
    # bulk_dl
    "WindowsUpdate": "bulk_dl", "UbuntuUpdate": "bulk_dl",
    "SoftwareUpdate": "bulk_dl", "AppleUpdate": "bulk_dl",
    # web_interactive
    "Facebook": "web_interactive", "Twitter": "web_interactive",
    "Instagram": "web_interactive", "WhatsApp": "web_interactive",
    "Telegram": "web_interactive", "Signal": "web_interactive",
    "Messenger": "web_interactive", "Reddit": "web_interactive",
    "Google": "web_interactive", "GoogleServices": "web_interactive",
    "Microsoft": "web_interactive", "Office365": "web_interactive",
    "Amazon": "web_interactive", "Cloudflare": "web_interactive",
    "MSN": "web_interactive", "Apple": "web_interactive",
    # system
    "DNS": "system", "NetBIOS": "system", "STUN": "system",
    "NTP": "system", "DHCP": "system", "LLMNR": "system",
    "mDNS": "system", "HTTP_Proxy": "system",
}

# ── Port Hint Table ────────────────────────────────────────────────────────────
PORT_HINTS = {
    (17, 27015): "game_rt",  (17, 27020): "game_rt",
    (17, 27021): "game_rt",  (17, 27040): "game_rt",
    (17, 7777):  "game_rt",  (17, 25565): "game_rt",
    (17, 3478):  "voip_call",(17, 3479):  "voip_call",
    (17, 5004):  "voip_call",(17, 5060):  "voip_call",
    (6,  5060):  "voip_call",
    (6,  21):    "bulk_dl",  (6,  20):    "bulk_dl",
    (6,  6881):  "torrent",  (17, 6881):  "torrent",
    (17, 6969):  "torrent",
    (17, 53):    "system",   (6,  53):    "system",
    (17, 67):    "system",   (17, 68):    "system",
    (17, 123):   "system",   (17, 5353):  "system",
    (6,  139):   "system",   (17, 137):   "system",
}

def behavioral_classify(proto, dport, avg_ps, pkts, octets, duration_s):
    bw = (octets * 8 / duration_s) if duration_s > 0 else 0
    if proto == 17 and avg_ps < 150 and bw < 500_000:
        return "game_rt"
    if proto == 17 and avg_ps < 200 and bw < 200_000:
        return "voip_call"
    if proto == 17 and 200 <= avg_ps <= 800 and bw < 3_000_000:
        return "video_conf"
    if proto == 6 and pkts > 100 and avg_ps < 500 and bw < 1_000_000:
        return "torrent"
    if avg_ps > 800 and bw > 500_000:
        return "bulk_dl"
    if avg_ps > 500 and 200_000 < bw < 25_000_000:
        return "video_live"
    if proto == 6 and 200 < avg_ps <= 800 and bw < 1_000_000:
        return "file_sync"
    if proto == 6 and avg_ps < 600:
        return "web_interactive"
    return "unknown"

def classify_mode_a(row):
    """Port-hint + Behavioral only (no DNS)."""
    proto  = int(row["proto"])
    dport  = int(row["dst_port"])
    avg_ps = float(row["avg_ps"])
    pkts   = float(row["pktTotalCount"] or 1)
    octets = float(row["octetTotalCount"])
    dur    = float(row["flowDuration"] or 1)
    hint = PORT_HINTS.get((proto, dport))
    if hint:
        return hint
    return behavioral_classify(proto, dport, avg_ps, pkts, octets, dur)

def classify_mode_b(row):
    """Port-hint + Behavioral + DNS snooping (web_service as proxy)."""
    proto  = int(row["proto"])
    dport  = int(row["dst_port"])
    avg_ps = float(row["avg_ps"])
    pkts   = float(row["pktTotalCount"] or 1)
    octets = float(row["octetTotalCount"])
    dur    = float(row["flowDuration"] or 1)
    svc    = str(row["web_service"]).strip()

    # Signal 1: DNS (weight 0.6) — from web_service proxy
    dns_class = DNS_MAP.get(svc)

    # Signal 2: Port hint (weight 0.3)
    port_class = PORT_HINTS.get((proto, dport))

    # Signal 3: Behavioral (weight 0.1)
    behav_class = behavioral_classify(proto, dport, avg_ps, pkts, octets, dur)

    # Weighted decision
    if dns_class:
        return dns_class       # DNS dominates (0.6)
    if port_class:
        return port_class      # Port hint (0.3)
    return behav_class         # Behavioral fallback (0.1)

# ── Ground Truth ──────────────────────────────────────────────────────────────
GT_MAP = {
    "Steam":           "game_rt",
    "Xbox":            "game_launcher",
    "Playstation":     "game_launcher",
    "Starcraft":       "game_rt",
    "Skype":           "voip_call",
    "SkypeCall":       "voip_call",
    "IMO":             "voip_call",
    "WhatsAppCall":    "voip_call",
    "GoogleHangoutDuo":"video_conf",
    "Webex":           "video_conf",
    "YouTube":         "video_live",
    "Twitch":          "video_live",
    "AppleiTunes":     "video_vod",
    "Netflix":         "video_vod",
    "Spotify":         "video_vod",
    "Dropbox":         "file_sync",
    "GoogleDrive":     "file_sync",
    "OneDrive":        "file_sync",
    "UbuntuONE":       "file_sync",
    "WindowsUpdate":   "bulk_dl",
    "Facebook":        "web_interactive",
    "Twitter":         "web_interactive",
    "Instagram":       "web_interactive",
    "WhatsApp":        "web_interactive",
    "Telegram":        "web_interactive",
    "Signal":          "web_interactive",
    "Messenger":       "web_interactive",
    "DNS":             "system",
    "NetBIOS":         "system",
    "DHCP":            "system",
    "NTP":             "system",
    "STUN":            "system",
}

CATEGORY_GT = {
    "VoIP":      "voip_call",
    "Media":     "video_live",
    "Streaming": "video_vod",
    "Download-FileTransfer-FileSharing": "torrent",
    "SoftwareUpdate": "bulk_dl",
    "Cloud":     "file_sync",
    "Game":      "game_rt",
    "Network":   "system",
    "System":    "system",
    "Web":       "web_interactive",
    "SocialNetwork": "web_interactive",
    "Chat":      "web_interactive",
    "Email":     "web_interactive",
    "Collaborative": "web_interactive",
}

def ground_truth(row):
    svc = str(row["web_service"]).strip()
    cat = str(row["category"]).strip()
    if svc in GT_MAP:
        return GT_MAP[svc]
    if cat in CATEGORY_GT:
        return CATEGORY_GT[cat]
    return "unknown"

# ── Load & Run ────────────────────────────────────────────────────────────────
print("Loading dataset …")
df = pd.read_csv(DATASET, low_memory=False)
print(f"  Rows: {len(df):,}")

print("Computing ground truth …")
df["gt"] = df.apply(ground_truth, axis=1)
df_eval = df[df["gt"] != "unknown"].copy()
print(f"  Evaluable rows: {len(df_eval):,}")

print("Mode A: Port-hint + Behavioral …")
df_eval["pred_a"] = df_eval.apply(classify_mode_a, axis=1)

print("Mode B: Port-hint + Behavioral + DNS …")
df_eval["pred_b"] = df_eval.apply(classify_mode_b, axis=1)

y_true = df_eval["gt"]
y_a    = df_eval["pred_a"]
y_b    = df_eval["pred_b"]

acc_a = accuracy_score(y_true, y_a)
acc_b = accuracy_score(y_true, y_b)

labels = sorted(y_true.unique())

print(f"\n── Mode A (Port + Behavioral)     accuracy: {acc_a*100:.2f}%")
print(f"── Mode B (Port + Behavioral + DNS) accuracy: {acc_b*100:.2f}%")
print(f"── DNS contribution: +{(acc_b-acc_a)*100:.2f} pp\n")

# Per-class
p_a, r_a, f_a, s_a = precision_recall_fscore_support(
    y_true, y_a, labels=labels, zero_division=0)
p_b, r_b, f_b, s_b = precision_recall_fscore_support(
    y_true, y_b, labels=labels, zero_division=0)

# ── Plot: Side-by-Side F1 Comparison ─────────────────────────────────────────
x  = np.arange(len(labels))
w  = 0.35
fig, axes = plt.subplots(1, 2, figsize=(16, 6))

# F1 comparison
ax = axes[0]
ax.bar(x - w/2, f_a, w, label=f"Mode A — No DNS  (acc={acc_a*100:.1f}%)",
       color="tomato",     alpha=0.85, edgecolor="grey", linewidth=0.4)
ax.bar(x + w/2, f_b, w, label=f"Mode B — With DNS (acc={acc_b*100:.1f}%)",
       color="mediumseagreen", alpha=0.85, edgecolor="grey", linewidth=0.4)
ax.set_title("F1-Score per Class: Port+Behavioral vs. Port+Behavioral+DNS",
             fontsize=11, fontweight="bold")
ax.set_xticks(x)
ax.set_xticklabels(labels, rotation=35, ha="right", fontsize=9)
ax.set_ylim(0, 1.05)
ax.set_ylabel("F1-Score")
ax.axhline(0.8, color="navy", linestyle="--", linewidth=0.8,
           label="0.8 target")
ax.legend(fontsize=9)

# Overall accuracy bar
ax2 = axes[1]
modes  = ["Mode A\n(Port + Behavioral)", "Mode B\n(Port + Behavioral + DNS)"]
accs   = [acc_a * 100, acc_b * 100]
colors = ["tomato", "mediumseagreen"]
bars   = ax2.bar(modes, accs, color=colors, edgecolor="grey",
                 linewidth=0.5, width=0.5)
for bar, acc in zip(bars, accs):
    ax2.text(bar.get_x() + bar.get_width()/2,
             bar.get_height() + 0.5,
             f"{acc:.1f}%", ha="center", va="bottom",
             fontsize=14, fontweight="bold")
ax2.set_ylim(0, 105)
ax2.set_ylabel("Overall Accuracy (%)")
ax2.set_title("Overall Accuracy: DNS Signal Contribution",
              fontsize=11, fontweight="bold")
ax2.axhline(80, color="navy", linestyle="--", linewidth=0.8,
            label="80% target")
ax2.legend()

plt.suptitle("MycoFlow Three-Signal Classifier — Unicauca Validation\n"
             f"2,704,839 flows  |  DNS contribution: "
             f"+{(acc_b-acc_a)*100:.1f} percentage points",
             fontsize=12, fontweight="bold")
plt.tight_layout()
plt.savefig(os.path.join(OUTDIR, "dns_comparison.png"), dpi=150)
plt.close()
print("  → Saved dns_comparison.png")

# ── Detailed Report ───────────────────────────────────────────────────────────
report_a = classification_report(y_true, y_a, labels=labels, digits=3)
report_b = classification_report(y_true, y_b, labels=labels, digits=3)

with open(os.path.join(OUTDIR, "dns_validation_report.txt"), "w") as f:
    f.write("MycoFlow Classifier Validation — DNS Signal Ablation Study\n")
    f.write("=" * 60 + "\n")
    f.write(f"Dataset: Unicauca April-June 2019 Network Flows\n")
    f.write(f"Total flows: {len(df):,}  |  Evaluable: {len(df_eval):,}\n\n")
    f.write(f"Mode A — Port-hint + Behavioral (no DNS)\n")
    f.write(f"  Overall Accuracy: {acc_a*100:.2f}%\n\n")
    f.write(report_a)
    f.write("\n\n")
    f.write(f"Mode B — Port-hint + Behavioral + DNS snooping\n")
    f.write(f"  Overall Accuracy: {acc_b*100:.2f}%\n\n")
    f.write(report_b)
    f.write(f"\n\nDNS Signal Contribution: +{(acc_b-acc_a)*100:.2f} percentage points\n")

# ── Save CSV ──────────────────────────────────────────────────────────────────
result_df = pd.DataFrame({
    "persona":       labels,
    "support":       s_a,
    "f1_no_dns":     f_a.round(3),
    "f1_with_dns":   f_b.round(3),
    "prec_no_dns":   p_a.round(3),
    "prec_with_dns": p_b.round(3),
    "rec_no_dns":    r_a.round(3),
    "rec_with_dns":  r_b.round(3),
    "f1_delta":      (f_b - f_a).round(3),
})
result_df.to_csv(os.path.join(OUTDIR, "dns_validation_results.csv"), index=False)

# ── Console Summary ───────────────────────────────────────────────────────────
print(f"\n── Per-Class F1 Comparison ──")
print(f"{'Persona':<18} {'F1 (no DNS)':>11} {'F1 (+ DNS)':>10} {'Delta':>7}  {'Support':>9}")
print("─" * 62)
for lbl, fa, fb, sa in zip(labels, f_a, f_b, s_a):
    delta = fb - fa
    arrow = "↑" if delta > 0.01 else ("↓" if delta < -0.01 else "~")
    print(f"  {lbl:<16} {fa:>11.3f} {fb:>10.3f} {arrow}{delta:>+6.3f}  {sa:>9,}")
print(f"\n  {'OVERALL':<16} {acc_a:>11.3f} {acc_b:>10.3f} "
      f" +{acc_b-acc_a:>5.3f}  {len(df_eval):>9,}")
print(f"\n✓ All outputs in: {OUTDIR}")
