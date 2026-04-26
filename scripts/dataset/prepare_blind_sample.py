#!/usr/bin/env python3
"""
prepare_blind_sample.py — One-time CSV preprocessing
====================================================

Loads the 1.2 GB Unicauca CSV ONCE, applies ground-truth labels,
performs stratified random sampling, and writes a small CSV (≈100 kB)
with only the columns the blind test needs.

This avoids re-loading 1.2 GB on every blind-test run (which is what
was crashing WSL2 sessions).

Usage:
  python3 prepare_blind_sample.py --n-per-persona 150 --seed 42
  → writes archive/blind_sample_n150_seed42.csv
"""
import argparse, os, sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "scripts" / "dataset"))
from router_blind_test import _ground_truth

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--csv",
                   default=str(REPO / "archive" /
                               "Unicauca-dataset-April-June-2019-Network-flows.csv"))
    p.add_argument("--n-per-persona", type=int, default=150)
    p.add_argument("--seed",          type=int, default=42)
    p.add_argument("--output",        default=None)
    args = p.parse_args()

    if not args.output:
        args.output = str(REPO / "archive" /
                          f"blind_sample_n{args.n_per_persona}_seed{args.seed}.csv")

    import pandas as pd

    print(f"[load] {args.csv} (~1.2 GB) — this takes 30-60s …", flush=True)
    df = pd.read_csv(args.csv, low_memory=False)
    print(f"[load] {len(df):,} rows", flush=True)

    print(f"[label] applying ground-truth rules …", flush=True)
    df["_persona"] = df.apply(_ground_truth, axis=1)
    df = df[df["_persona"] != "unknown"].copy()
    print(f"[label] {len(df):,} rows with known persona", flush=True)

    # Numeric coercion (same as router_blind_test.py)
    for col in ["proto", "dst_port", "avg_ps", "octetTotalCount", "flowDuration"]:
        df[col] = pd.to_numeric(df[col], errors="coerce")
    df["proto"]          = df["proto"].fillna(6).astype(int)
    df["dst_port"]       = df["dst_port"].fillna(80).astype(int).clip(lower=1, upper=65535)
    df["avg_ps"]         = df["avg_ps"].fillna(128)
    df["octetTotalCount"]= df["octetTotalCount"].fillna(0)
    df["flowDuration"]   = df["flowDuration"].fillna(1).clip(lower=1e-6)
    df["_bw_bps"] = df["octetTotalCount"] * 8.0 / df["flowDuration"]

    print(f"[sample] stratified random N={args.n_per_persona}/persona, seed={args.seed} …",
          flush=True)
    samples = []
    for persona, grp in df.groupby("_persona"):
        n = min(args.n_per_persona, len(grp))
        sub = grp.sample(n=n, random_state=args.seed).copy()
        sub["_gt_persona"] = persona
        samples.append(sub)
    out = pd.concat(samples, ignore_index=True)
    print(f"[sample] {len(out):,} flows × {out['_gt_persona'].nunique()} personas",
          flush=True)

    # Keep only columns the blind test reads
    keep = ["proto", "dst_port", "avg_ps", "octetTotalCount",
            "flowDuration", "_bw_bps", "web_service", "_gt_persona"]
    out = out[keep]
    out.to_csv(args.output, index=False)
    size_kb = os.path.getsize(args.output) / 1024
    print(f"[done] {args.output}  ({size_kb:.1f} kB, {len(out):,} rows)")
    print(f"       blind test will load this in <1s instead of re-parsing the 1.2 GB CSV.")

if __name__ == "__main__":
    main()
