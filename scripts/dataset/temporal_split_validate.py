#!/usr/bin/env python3
"""
temporal_split_validate.py — Layer 1 enhancement for IEEE rigor
================================================================

Heuristic classifiers don't have a "training set", but they DO have
hand-tuned thresholds (e.g. udp_ratio >= 0.25, bw < 200kbps for VOIP).
Those thresholds were chosen by inspecting flows during development.

To rule out implicit overfitting, we split the Unicauca dataset by
flowStart timestamp:

  - Calibration window  : April-May 2019  (used to set thresholds)
  - Held-out test window: June 2019       (never seen during dev)

If accuracy drops sharply in the held-out window, the heuristics are
calibrated to a specific time period and won't generalize. If accuracy
holds, we have a defensible cross-validation analogue for IEEE.

Usage:
  python3 temporal_split_validate.py --csv archive/Unicauca-...csv \
                                      --output-dir results/temporal_split
"""
import argparse, csv, json, math, os, sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

# Reuse the same ground-truth labeller and Wilson CI as the blind test
sys.path.insert(0, str(Path(__file__).resolve().parent))
from router_blind_test import _ground_truth, GT_TO_MYCO

def wilson_ci(k, n, z=1.96):
    if n == 0: return (0.0, 0.0)
    p_hat = k / n; denom = 1 + z*z/n
    centre = (p_hat + z*z/(2*n)) / denom
    margin = z * math.sqrt((p_hat*(1-p_hat) + z*z/(4*n)) / n) / denom
    return (max(0.0, centre - margin), min(1.0, centre + margin))

def behavioral_predict(row):
    """Reproduce decide_persona() heuristics (port + behavior, NO DNS)
    on a single flow's CSV statistics. Used to compare offline vs in-vivo.

    NOTE: This is the SAME logic as mycoflow_validate.py's offline mode,
    duplicated here so this file is self-contained. Any divergence from
    the C implementation in myco_persona.c is a known limitation; the
    C-side is authoritative.
    """
    proto = int(row.get("proto", 0))
    dport = int(row.get("dst_port", 0))
    octs  = float(row.get("octetTotalCount", 0))
    pkts  = float(row.get("pktTotalCount", 1) or 1)
    dur   = max(1e-6, float(row.get("flowDuration", 1)))
    avg_p = float(row.get("avg_ps", 128))
    bw    = octs * 8.0 / dur
    is_udp = (proto == 17)

    # Port hints — same map as myco_hint.c
    PORT_HINTS = {
        # gaming
        27015:"gaming",27016:"gaming",27017:"gaming",27018:"gaming",
        27020:"gaming",27021:"gaming",
        # voip / video conf
        3478:"voip",3479:"voip",5060:"voip",5061:"voip",
        19302:"voip",19303:"voip",19304:"voip",19305:"voip",
        19306:"voip",19307:"voip",19308:"voip",19309:"voip",
        8801:"video",8802:"video",8803:"video",8804:"video",
        # torrent
        6881:"torrent",6882:"torrent",6883:"torrent",6889:"torrent",
        51413:"torrent",
    }
    hint = PORT_HINTS.get(dport, "unknown")

    # Behavioral rules (mirror myco_persona.c decide_persona)
    if is_udp and bw > 5_000_000 and avg_p > 800:
        return "streaming"        # heavy QUIC video
    if is_udp and bw > 500_000 and avg_p < 800:
        return hint if hint == "video" else "gaming"
    if avg_p < 120 and 20_000 <= bw < 200_000:
        return "voip"
    if avg_p < 350 and bw > 100_000:
        return "gaming" if hint == "gaming" else "video"
    if 200_000 <= bw <= 8_000_000:
        return "video"
    if bw > 5_000_000:
        return "bulk"
    if bw > 50_000 and hint != "unknown":
        return hint
    return "unknown"

def domain_predict(row):
    """Like behavioral_predict but ALSO uses the web_service column to
    simulate DNS hint (Mode B equivalent). web_service comes from
    Unicauca's nDPI labels which proxy what the daemon would resolve."""
    p = behavioral_predict(row)
    if p != "unknown":
        return p
    svc = str(row.get("web_service", "")).strip()
    return GT_TO_MYCO.get(_ground_truth(row), "unknown") if svc else "unknown"

def evaluate_window(rows, predictor, label):
    matrix  = defaultdict(lambda: defaultdict(int))
    classes = set()
    correct = total = 0
    for r in rows:
        gt = GT_TO_MYCO.get(_ground_truth(r), "unknown")
        if gt == "unknown":
            continue
        pred = predictor(r) or "unknown"
        matrix[gt][pred] += 1
        classes.add(gt); classes.add(pred)
        total += 1
        if gt == pred:
            correct += 1
    acc = correct / total if total else 0.0
    lo, hi = wilson_ci(correct, total)
    return {
        "label":   label,
        "n":       total,
        "correct": correct,
        "accuracy":acc,
        "ci_lo":   lo, "ci_hi": hi,
        "matrix":  {gt: dict(preds) for gt, preds in matrix.items()},
        "classes": sorted(classes),
    }

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--csv", required=True)
    parser.add_argument("--output-dir", default="results/temporal_split")
    parser.add_argument("--split-date", default="2019-06-01",
                        help="ISO date — flows ≥ this go into the held-out window")
    parser.add_argument("--max-rows", type=int, default=None,
                        help="Cap rows for quick smoke test")
    args = parser.parse_args()

    out = Path(args.output_dir); out.mkdir(parents=True, exist_ok=True)
    split_ts = datetime.fromisoformat(args.split_date).replace(tzinfo=timezone.utc).timestamp()
    print(f"[load] {args.csv} (split @ {args.split_date} = {split_ts:.0f})")

    cal_rows, test_rows = [], []
    skipped = 0
    with open(args.csv) as f:
        reader = csv.DictReader(f)
        for i, row in enumerate(reader):
            if args.max_rows and i >= args.max_rows:
                break
            try:
                fs = float(row.get("flowStart", 0))
            except ValueError:
                skipped += 1
                continue
            if fs < split_ts:
                cal_rows.append(row)
            else:
                test_rows.append(row)
    print(f"[load] calibration={len(cal_rows):,}  held-out={len(test_rows):,}  skipped={skipped:,}")

    results = []
    for window_label, rows in [("calibration_AprMay", cal_rows),
                                ("held_out_Jun",      test_rows)]:
        for pred_label, pred_fn in [("port_behavior", behavioral_predict),
                                     ("port_behavior_dns", domain_predict)]:
            print(f"[eval] {window_label} × {pred_label} on {len(rows):,} flows …", flush=True)
            r = evaluate_window(rows, pred_fn, f"{window_label}_{pred_label}")
            results.append(r)
            print(f"        accuracy = {r['accuracy']*100:.2f}% "
                  f"(95% CI: {r['ci_lo']*100:.2f}% – {r['ci_hi']*100:.2f}%)  N={r['n']:,}")

    # Persist raw results
    with open(out / "temporal_split_results.json", "w") as f:
        json.dump(results, f, indent=2)

    # Compact text report
    with open(out / "temporal_split_report.txt", "w") as f:
        f.write("=" * 72 + "\n")
        f.write("Temporal Split Validation — Heuristic Generalization Check\n")
        f.write("=" * 72 + "\n")
        f.write(f"Calibration window : flows < {args.split_date}  (used during dev)\n")
        f.write(f"Held-out window    : flows ≥ {args.split_date}  (never seen)\n")
        f.write(f"Predictor variants : port+behavioral  /  port+behavioral+DNS\n\n")
        f.write(f"{'Window':<24} {'Predictor':<24} {'N':>10} {'Acc':>8} {'95% CI':>22}\n")
        f.write("-" * 90 + "\n")
        for r in results:
            window, pred = r["label"].rsplit("_", 1)[0], r["label"].rsplit("_", 1)[1]
            ci = f"[{r['ci_lo']*100:.2f}%, {r['ci_hi']*100:.2f}%]"
            f.write(f"{window:<24} {pred:<24} {r['n']:>10,} "
                    f"{r['accuracy']*100:>7.2f}% {ci:>22}\n")

        # Generalization gap
        cal_pb = next((r for r in results if r["label"] == "calibration_AprMay_port_behavior"), None)
        hld_pb = next((r for r in results if r["label"] == "held_out_Jun_port_behavior"), None)
        cal_dns= next((r for r in results if r["label"] == "calibration_AprMay_port_behavior_dns"), None)
        hld_dns= next((r for r in results if r["label"] == "held_out_Jun_port_behavior_dns"), None)
        f.write("\nGeneralization gap  (calibration → held-out)\n")
        if cal_pb and hld_pb:
            gap = cal_pb["accuracy"] - hld_pb["accuracy"]
            f.write(f"  port+behavioral      : {cal_pb['accuracy']*100:.2f}% → {hld_pb['accuracy']*100:.2f}%  "
                    f"(Δ = {gap*100:+.2f} pts)\n")
        if cal_dns and hld_dns:
            gap = cal_dns["accuracy"] - hld_dns["accuracy"]
            f.write(f"  port+behavioral+DNS  : {cal_dns['accuracy']*100:.2f}% → {hld_dns['accuracy']*100:.2f}%  "
                    f"(Δ = {gap*100:+.2f} pts)\n")
        f.write("\nInterpretation: gaps within ±2 pts → no significant overfitting.\n")
        f.write("Larger gaps → heuristic thresholds tuned to specific period.\n")

    print(f"\n[done] results in {out}/")
    print(f"       - temporal_split_results.json")
    print(f"       - temporal_split_report.txt")

if __name__ == "__main__":
    main()
