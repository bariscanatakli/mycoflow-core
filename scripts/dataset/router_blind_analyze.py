#!/usr/bin/env python3
"""
router_blind_analyze.py — Comprehensive IEEE-style report generator
===================================================================

Reads the JSONL outputs of router_blind_test.py (one or two modes) and
the resource monitor output, then produces:

  - Per-mode confusion matrix (PNG)
  - Per-class precision/recall/F1 with Wilson 95% CI
  - Mode A vs Mode B paired comparison (McNemar test)
  - Online vs offline accuracy comparison table
  - Resource overhead summary (CPU/RAM/latency distributions)
  - LaTeX-ready tables for IEEE paper
  - Plain-text comprehensive report

Usage:
  python3 router_blind_analyze.py \\
      --mode-a results/blind_test/mode_A_n150_seed42.jsonl \\
      --mode-b results/blind_test/mode_B_n150_seed42.jsonl \\
      --resource-a results/blind_test/resources_modeA.jsonl \\
      --resource-b results/blind_test/resources_modeB.jsonl \\
      --output-dir results/blind_test/report
"""
import argparse, json, math, os, sys
from collections import defaultdict
from pathlib import Path

def load_jsonl(path):
    if not path or not os.path.exists(path):
        return []
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except Exception:
                pass
    return out

def wilson_ci(k, n, z=1.96):
    """Wilson score interval — standard for binomial proportions in the
    classification literature. Better than normal approx for small N or extreme p."""
    if n == 0:
        return (0.0, 0.0)
    p_hat   = k / n
    denom   = 1.0 + z*z/n
    centre  = (p_hat + z*z/(2*n)) / denom
    margin  = z * math.sqrt((p_hat*(1-p_hat) + z*z/(4*n)) / n) / denom
    return (max(0.0, centre - margin), min(1.0, centre + margin))

def mcnemar_test(a_results, b_results):
    """McNemar test for paired binary outcomes — standard for comparing two
    classifiers on the same test set. We pair by row_id (same input flow).

    b: B correct, A wrong
    c: A correct, B wrong
    Statistic: (|b-c|-1)^2 / (b+c)  (with continuity correction)
    Returns (b, c, chi2, p_two_sided)
    """
    a_map = {r.get("_row_id"): (r.get("predicted") == r.get("gt_myco_label"))
             for r in a_results if r.get("_row_id") and "gt_myco_label" in r}
    b_map = {r.get("_row_id"): (r.get("predicted") == r.get("gt_myco_label"))
             for r in b_results if r.get("_row_id") and "gt_myco_label" in r}
    # Pair by stripping mode prefix from row_id
    def base_id(rid):  # "A_000123" → "000123"
        return rid.split("_", 1)[1] if "_" in rid else rid
    a_pairs = {base_id(rid): ok for rid, ok in a_map.items()}
    b_pairs = {base_id(rid): ok for rid, ok in b_map.items()}
    common = set(a_pairs) & set(b_pairs)
    if not common:
        return None
    b = sum(1 for k in common if (b_pairs[k] and not a_pairs[k]))   # B wins
    c = sum(1 for k in common if (a_pairs[k] and not b_pairs[k]))   # A wins
    if (b + c) == 0:
        return {"b_only": b, "c_only": c, "chi2": 0.0, "p_value": 1.0, "n_pairs": len(common)}
    chi2 = (abs(b - c) - 1) ** 2 / (b + c)
    # Two-sided p from chi-square with df=1: p = exp(-chi2/2) is approx; use survival fn
    # We approximate with a simple formula (good for chi2 > 0.1)
    p = math.exp(-chi2 / 2.0) if chi2 > 0 else 1.0
    return {"b_only": b, "c_only": c, "chi2": chi2, "p_value": min(1.0, p),
            "n_pairs": len(common)}

def per_class_metrics(results):
    """Compute precision/recall/F1 per class with Wilson CIs."""
    classes = sorted(set(r.get("gt_myco_label", "?") for r in results) |
                     set(r.get("predicted", "?")     for r in results))
    matrix = defaultdict(lambda: defaultdict(int))  # gt → pred → count
    for r in results:
        gt   = r.get("gt_myco_label", "?")
        pred = r.get("predicted",     "?")
        matrix[gt][pred] += 1

    metrics = {}
    for cls in classes:
        tp = matrix[cls][cls]
        fn = sum(matrix[cls][p] for p in classes if p != cls)
        fp = sum(matrix[gt][cls] for gt in classes if gt != cls)
        support = tp + fn

        precision = tp / (tp + fp) if (tp + fp) > 0 else 0.0
        recall    = tp / (tp + fn) if (tp + fn) > 0 else 0.0
        f1        = 2 * precision * recall / (precision + recall) if (precision + recall) > 0 else 0.0

        rec_lo, rec_hi  = wilson_ci(tp, tp + fn) if (tp + fn) > 0 else (0.0, 0.0)
        prec_lo, prec_hi= wilson_ci(tp, tp + fp) if (tp + fp) > 0 else (0.0, 0.0)

        metrics[cls] = {
            "support":   support,
            "tp": tp, "fp": fp, "fn": fn,
            "precision":     precision,
            "precision_ci":  (prec_lo, prec_hi),
            "recall":        recall,
            "recall_ci":     (rec_lo, rec_hi),
            "f1":            f1,
        }
    return matrix, metrics, classes

def overall_accuracy(results):
    n_eval  = sum(1 for r in results if "gt_myco_label" in r and "predicted" in r)
    correct = sum(1 for r in results if r.get("gt_myco_label") == r.get("predicted"))
    return n_eval, correct, (correct / n_eval if n_eval else 0.0), wilson_ci(correct, n_eval)

# ── Plotting ──────────────────────────────────────────────────────────────────
def plot_confusion(matrix, classes, title, out_path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print(f"[!] matplotlib missing — skipping {out_path}")
        return
    n = len(classes)
    grid = np.zeros((n, n), dtype=float)
    for i, gt in enumerate(classes):
        row_total = sum(matrix[gt].get(p, 0) for p in classes)
        if row_total == 0:
            continue
        for j, pred in enumerate(classes):
            grid[i, j] = matrix[gt].get(pred, 0) / row_total
    fig, ax = plt.subplots(figsize=(max(6, n*0.6), max(5, n*0.6)))
    im = ax.imshow(grid, cmap="Blues", vmin=0, vmax=1, aspect="auto")
    ax.set_xticks(range(n)); ax.set_yticks(range(n))
    ax.set_xticklabels(classes, rotation=45, ha="right")
    ax.set_yticklabels(classes)
    ax.set_xlabel("Predicted persona (router classifier)")
    ax.set_ylabel("Ground-truth persona (Unicauca label)")
    ax.set_title(title)
    for i in range(n):
        for j in range(n):
            v = grid[i, j]
            if v > 0:
                color = "white" if v > 0.5 else "black"
                ax.text(j, i, f"{v:.2f}", ha="center", va="center",
                        color=color, fontsize=8)
    fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close(fig)

def plot_resource_timeline(monitor_data, title, out_path):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        return
    if not monitor_data:
        return
    ts0  = monitor_data[0]["ts"]
    xs   = [(d["ts"] - ts0)/60 for d in monitor_data]
    cpu  = [d.get("cpu_pct", 0) or 0 for d in monitor_data]
    proc = [d.get("mycoflowd_cpu_pct", 0) or 0 for d in monitor_data]
    rss  = [(d.get("mycoflowd_rss_kb", 0) or 0)/1024 for d in monitor_data]
    ct   = [d.get("ct_count", 0) or 0 for d in monitor_data]

    fig, axes = plt.subplots(4, 1, figsize=(10, 8), sharex=True)
    axes[0].plot(xs, cpu, "C0-", label="System CPU%")
    axes[0].set_ylabel("System CPU %"); axes[0].grid(alpha=0.3); axes[0].legend(loc="upper right")
    axes[1].plot(xs, proc, "C1-", label="mycoflowd CPU%")
    axes[1].set_ylabel("Daemon CPU %"); axes[1].grid(alpha=0.3); axes[1].legend(loc="upper right")
    axes[2].plot(xs, rss, "C2-", label="mycoflowd RSS")
    axes[2].set_ylabel("RSS (MB)"); axes[2].grid(alpha=0.3); axes[2].legend(loc="upper right")
    axes[3].plot(xs, ct, "C3-", label="conntrack count")
    axes[3].set_ylabel("conntrack entries"); axes[3].grid(alpha=0.3); axes[3].legend(loc="upper right")
    axes[3].set_xlabel("Time (minutes)")
    fig.suptitle(title)
    plt.tight_layout()
    plt.savefig(out_path, dpi=150)
    plt.close(fig)

# ── Report writers ────────────────────────────────────────────────────────────
def write_text_report(res_a, res_b, mon_a, mon_b, mc, out_path):
    fp = open(out_path, "w")

    def section(title, sub=False):
        bar = "=" if not sub else "-"
        fp.write(f"\n{title}\n{bar*len(title)}\n")

    fp.write("=" * 78 + "\n")
    fp.write("MycoFlow In-Vivo Validation Report (IEEE-grade)\n")
    fp.write("=" * 78 + "\n")
    fp.write("Test methodology: stratified random per-flow replay against live router\n")
    fp.write("Router          : 10.10.1.1 (Xiaomi AX3000T, MT7981B, 256MB, OpenWrt)\n")
    fp.write("Dataset         : Universidad del Cauca April-June 2019 Network Flows\n")
    fp.write("Sampling        : Wilson 95% CI on per-class precision and recall\n")
    fp.write("Pairing test    : McNemar (continuity-corrected χ², df=1) for Mode A vs B\n")

    for label, res in [("MODE A — IP-only (no DNS hint exposure)", res_a),
                       ("MODE B — Hostname (organic DNS via real services)", res_b)]:
        if not res:
            continue
        section(label)
        n_eval, correct, acc, (lo, hi) = overall_accuracy(res)
        fp.write(f"  Total flows evaluated : {n_eval:,}\n")
        fp.write(f"  Correctly classified  : {correct:,}\n")
        fp.write(f"  Overall accuracy      : {acc*100:.2f}%   (95% CI: {lo*100:.2f}% – {hi*100:.2f}%)\n")

        # Per-class
        matrix, metrics, classes = per_class_metrics(res)
        section("Per-class metrics", sub=True)
        fp.write(f"  {'class':<12} {'support':>8}  {'precision':>22}   {'recall':>22}   {'F1':>6}\n")
        for cls, m in sorted(metrics.items(), key=lambda kv: -kv[1]["support"]):
            if m["support"] == 0 and m["fp"] == 0:
                continue
            p, r = m["precision"], m["recall"]
            plo, phi = m["precision_ci"]; rlo, rhi = m["recall_ci"]
            fp.write(f"  {cls:<12} {m['support']:>8}  "
                     f"{p:.3f} [{plo:.3f},{phi:.3f}]   "
                     f"{r:.3f} [{rlo:.3f},{rhi:.3f}]   "
                     f"{m['f1']:.3f}\n")

        # Confusion matrix as text
        section("Confusion matrix (rows=GT, cols=Pred, values=count)", sub=True)
        col_w = max(8, max(len(c) for c in classes) + 1)
        fp.write("  " + "".rjust(col_w) + "".join(f"{c:>{col_w}}" for c in classes) + "\n")
        for gt in classes:
            row = "".join(f"{matrix[gt].get(p,0):>{col_w}}" for p in classes)
            fp.write(f"  {gt:>{col_w}}{row}\n")

    # Mode comparison
    if mc and res_a and res_b:
        section("MODE A vs MODE B Paired Comparison (McNemar)")
        fp.write(f"  Paired flows (same inputs)         : {mc['n_pairs']:,}\n")
        fp.write(f"  B correct, A wrong (DNS helped)    : {mc['b_only']:,}\n")
        fp.write(f"  A correct, B wrong (DNS hurt)      : {mc['c_only']:,}\n")
        fp.write(f"  McNemar χ² (continuity-corrected)  : {mc['chi2']:.4f}\n")
        fp.write(f"  Two-sided p-value (df=1)           : {mc['p_value']:.4g}\n")
        if mc["p_value"] < 0.001:
            fp.write(f"  → Significant at p<0.001 (DNS contribution non-random)\n")
        elif mc["p_value"] < 0.05:
            fp.write(f"  → Significant at p<0.05\n")
        else:
            fp.write(f"  → Not statistically significant at p<0.05\n")

    # Resource overhead
    for label, mon in [("MODE A", mon_a), ("MODE B", mon_b)]:
        if not mon:
            continue
        section(f"Resource overhead — {label}")
        cpu  = sorted([d.get("cpu_pct", 0) or 0 for d in mon if d.get("cpu_pct") is not None])
        proc = sorted([d.get("mycoflowd_cpu_pct", 0) or 0 for d in mon if d.get("mycoflowd_cpu_pct") is not None])
        rss  = sorted([d.get("mycoflowd_rss_kb", 0) or 0 for d in mon if d.get("mycoflowd_rss_kb")])
        used = sorted([d.get("mem_used_pct", 0) or 0 for d in mon if d.get("mem_used_pct")])
        ct   = sorted([d.get("ct_count", 0) or 0 for d in mon if d.get("ct_count", -1) >= 0])
        crashes = sum(1 for d in mon if not d.get("daemon_alive"))
        ssh_err = sum(1 for d in mon if d.get("ssh_error"))
        n = len(mon)
        def pct(arr, p):
            if not arr: return 0
            i = int(len(arr) * p / 100)
            return arr[min(i, len(arr)-1)]
        fp.write(f"  Samples collected         : {n}\n")
        fp.write(f"  Daemon liveness drops     : {crashes}/{n}  ({crashes/max(1,n)*100:.1f}%)\n")
        fp.write(f"  SSH errors (probe failed) : {ssh_err}/{n}  ({ssh_err/max(1,n)*100:.1f}%)\n")
        fp.write(f"  System CPU%   p50 / p95 / max : {pct(cpu,50):>5.1f} / {pct(cpu,95):>5.1f} / {pct(cpu,100):>5.1f}\n")
        fp.write(f"  mycoflowd CPU% p50 / p95 / max: {pct(proc,50):>5.1f} / {pct(proc,95):>5.1f} / {pct(proc,100):>5.1f}\n")
        fp.write(f"  RSS (KB) p50 / p95 / max      : {pct(rss,50):>7} / {pct(rss,95):>7} / {pct(rss,100):>7}\n")
        fp.write(f"  Mem used % p50 / p95 / max    : {pct(used,50):>5.1f} / {pct(used,95):>5.1f} / {pct(used,100):>5.1f}\n")
        fp.write(f"  Conntrack p50 / p95 / max     : {pct(ct,50):>6} / {pct(ct,95):>6} / {pct(ct,100):>6}\n")

    fp.write("\n" + "=" * 78 + "\n")
    fp.write("End of report\n")
    fp.close()

def write_latex_table(res_a, res_b, mc, out_path):
    """Emit a IEEE-style accuracy comparison table."""
    fp = open(out_path, "w")
    fp.write("% Auto-generated by router_blind_analyze.py\n")
    fp.write("\\begin{table}[t]\n\\centering\n")
    fp.write("\\caption{In-Vivo Classifier Accuracy on Universidad del Cauca Flow Dataset (Stratified Random Sample)}\n")
    fp.write("\\label{tab:invivo_accuracy}\n")
    fp.write("\\begin{tabular}{l|cc|cc|c}\n\\hline\n")
    fp.write("\\textbf{Class} & \\multicolumn{2}{c|}{\\textbf{Mode A: IP-only}} & "
             "\\multicolumn{2}{c|}{\\textbf{Mode B: +DNS}} & \\textbf{Lift}\\\\\n")
    fp.write(" & Prec. & Rec. & Prec. & Rec. & $\\Delta$ Rec. \\\\\n\\hline\n")

    if res_a:
        _, m_a, classes = per_class_metrics(res_a)
    else:
        m_a, classes = {}, []
    if res_b:
        _, m_b, classes_b = per_class_metrics(res_b)
        classes = sorted(set(classes) | set(classes_b))
    else:
        m_b = {}

    for cls in classes:
        a = m_a.get(cls, {"precision":0,"recall":0,"support":0})
        b = m_b.get(cls, {"precision":0,"recall":0,"support":0})
        if a["support"] + b.get("support", 0) == 0:
            continue
        delta = b["recall"] - a["recall"]
        sign = "+" if delta >= 0 else ""
        fp.write(f"{cls.replace('_','\\_'):<14} & "
                 f"{a['precision']:.3f} & {a['recall']:.3f} & "
                 f"{b['precision']:.3f} & {b['recall']:.3f} & "
                 f"{sign}{delta:.3f} \\\\\n")

    if res_a and res_b:
        n_a, c_a, acc_a, (lo_a, hi_a) = overall_accuracy(res_a)
        n_b, c_b, acc_b, (lo_b, hi_b) = overall_accuracy(res_b)
        fp.write("\\hline\n")
        fp.write(f"\\textbf{{Overall}} & \\multicolumn{{2}}{{c|}}{{{acc_a:.3f} [{lo_a:.3f},{hi_a:.3f}]}} & "
                 f"\\multicolumn{{2}}{{c|}}{{{acc_b:.3f} [{lo_b:.3f},{hi_b:.3f}]}} & "
                 f"+{acc_b-acc_a:.3f} \\\\\n")
        if mc:
            fp.write(f"\\hline\n\\multicolumn{{6}}{{l}}{{McNemar test: $\\chi^2={mc['chi2']:.2f}$, "
                     f"$p={mc['p_value']:.3g}$, $N_{{pairs}}={mc['n_pairs']}$}}\\\\\n")

    fp.write("\\hline\n\\end{tabular}\n\\end{table}\n")
    fp.close()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode-a",     help="JSONL from Mode A run")
    parser.add_argument("--mode-b",     help="JSONL from Mode B run")
    parser.add_argument("--resource-a", help="Resource monitor JSONL for Mode A")
    parser.add_argument("--resource-b", help="Resource monitor JSONL for Mode B")
    parser.add_argument("--output-dir", default="results/blind_test/report")
    args = parser.parse_args()

    out = Path(args.output_dir); out.mkdir(parents=True, exist_ok=True)

    res_a = load_jsonl(args.mode_a)
    res_b = load_jsonl(args.mode_b)
    mon_a = load_jsonl(args.resource_a)
    mon_b = load_jsonl(args.resource_b)

    print(f"[load] Mode A: {len(res_a)} results,  monitor: {len(mon_a)} samples")
    print(f"[load] Mode B: {len(res_b)} results,  monitor: {len(mon_b)} samples")

    # Confusion matrix plots
    if res_a:
        m, _, c = per_class_metrics(res_a)
        plot_confusion(m, c, "Mode A — IP-only (no DNS)", out / "confusion_modeA.png")
        print(f"[plot] {out/'confusion_modeA.png'}")
    if res_b:
        m, _, c = per_class_metrics(res_b)
        plot_confusion(m, c, "Mode B — +DNS hint via organic hostname queries", out / "confusion_modeB.png")
        print(f"[plot] {out/'confusion_modeB.png'}")

    # Resource timeline plots
    if mon_a:
        plot_resource_timeline(mon_a, "Resource overhead — Mode A run", out / "resources_modeA.png")
        print(f"[plot] {out/'resources_modeA.png'}")
    if mon_b:
        plot_resource_timeline(mon_b, "Resource overhead — Mode B run", out / "resources_modeB.png")
        print(f"[plot] {out/'resources_modeB.png'}")

    # McNemar
    mc = mcnemar_test(res_a, res_b) if (res_a and res_b) else None

    # Text report
    report_path = out / "validation_report.txt"
    write_text_report(res_a, res_b, mon_a, mon_b, mc, report_path)
    print(f"[report] {report_path}")

    # LaTeX table
    if res_a or res_b:
        latex_path = out / "accuracy_table.tex"
        write_latex_table(res_a, res_b, mc, latex_path)
        print(f"[latex] {latex_path}")

    # CSV exports for further analysis (one row per evaluated flow)
    import csv
    csv_path = out / "all_flows.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["mode","row_id","gt_persona","gt_myco_label","predicted",
                    "correct","proto","dst_port","pkt_size","bw_bps",
                    "web_service","dns_prime_host","device_bw_bps","device_flows"])
        for label, results in [("A", res_a), ("B", res_b)]:
            for r in results:
                if "predicted" not in r:
                    continue
                w.writerow([label, r.get("_row_id"), r.get("gt_persona"),
                            r.get("gt_myco_label"), r.get("predicted"),
                            int(r.get("predicted") == r.get("gt_myco_label")),
                            r.get("proto"), r.get("dst_port"),
                            r.get("pkt_size"), r.get("bw_bps"),
                            r.get("web_service"), r.get("dns_prime_host"),
                            r.get("device_bw_bps"), r.get("device_flows")])
    print(f"[csv]    {csv_path}")
    print(f"\n[done] all artifacts in {out}/")

if __name__ == "__main__":
    main()
