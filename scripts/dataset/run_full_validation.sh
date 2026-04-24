#!/bin/bash
# run_full_validation.sh — IEEE-grade comprehensive validation orchestrator
# =========================================================================
# Runs the full 3-layer validation pipeline:
#   Layer 1: Temporal split (offline, ~5 min)
#   Layer 2: Mode A blind test + resource monitor (~2 h)
#   Layer 2: Mode B blind test + resource monitor (~2 h)
#   Layer 3: Resource analysis (folded into analyzer)
#   Final  : Comprehensive report generation
#
# Usage:
#   ./run_full_validation.sh                    # full N=150 per persona
#   ./run_full_validation.sh smoke              # smoke test, N=10 per persona
#   ./run_full_validation.sh --skip-temporal    # skip Layer 1 (slow CSV reload)
#   ./run_full_validation.sh --resume           # resume incomplete runs

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RESULTS="$REPO_ROOT/results/blind_test"
CSV="$REPO_ROOT/archive/Unicauca-dataset-April-June-2019-Network-flows.csv"
SEED=42
mkdir -p "$RESULTS"

# Parse args
N=150
SKIP_TEMPORAL=0
RESUME=""
SMOKE=0
for arg in "$@"; do
    case $arg in
        smoke)            SMOKE=1; N=10 ;;
        --skip-temporal)  SKIP_TEMPORAL=1 ;;
        --resume)         RESUME="--resume" ;;
        --n=*)            N="${arg#*=}" ;;
        *)                echo "Unknown arg: $arg"; exit 1 ;;
    esac
done

echo "============================================================"
echo "MycoFlow IEEE-grade comprehensive validation"
echo "============================================================"
echo "  N per persona  : $N  (full=150, smoke=10)"
echo "  Seed           : $SEED  (reproducible)"
echo "  Results dir    : $RESULTS"
echo "  Resume         : ${RESUME:-no}"
echo "  Skip temporal  : $SKIP_TEMPORAL"
echo "  ETA (full)     : ~5 hours total"
echo "============================================================"

# ── Pre-flight checks ───────────────────────────────────────────────────────
echo
echo "[pre] Checking dependencies …"
command -v sshpass    >/dev/null || { echo "[!] sshpass missing"; exit 1; }
command -v python3    >/dev/null || { echo "[!] python3 missing"; exit 1; }
python3 -c 'import pandas, numpy, matplotlib' 2>/dev/null || \
    { echo "[!] pip install pandas numpy matplotlib"; exit 1; }
[ -f "$CSV" ] || { echo "[!] CSV not found: $CSV"; exit 1; }

echo "[pre] Pinging router …"
sshpass -p sukranflat7 ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
        root@10.10.1.1 'echo router-alive' || { echo "[!] router unreachable"; exit 1; }

echo "[pre] All checks passed"

# ── Layer 1: Temporal split (offline, fast) ─────────────────────────────────
if [ "$SKIP_TEMPORAL" -eq 0 ]; then
    echo
    echo "============================================================"
    echo "Layer 1: Temporal split validation (offline)"
    echo "============================================================"
    python3 "$SCRIPT_DIR/temporal_split_validate.py" \
        --csv "$CSV" \
        --output-dir "$RESULTS/temporal_split" \
        2>&1 | tee "$RESULTS/temporal_split.log"
fi

# ── Helper: run one mode with parallel resource monitor ────────────────────
run_mode() {
    local MODE=$1
    local OUT="$RESULTS/mode_${MODE}_n${N}_seed${SEED}.jsonl"
    local MON="$RESULTS/resources_mode${MODE}.jsonl"
    local LOG="$RESULTS/mode_${MODE}.log"

    echo
    echo "============================================================"
    echo "Layer 2 + 3: Mode $MODE blind test + resource monitor"
    echo "============================================================"
    echo "  Output JSONL : $OUT"
    echo "  Monitor JSONL: $MON"
    echo "  Log          : $LOG"

    # Start resource monitor in background
    python3 "$SCRIPT_DIR/router_resource_monitor.py" \
        --output "$MON" --interval 1.0 \
        > "$RESULTS/monitor_${MODE}.log" 2>&1 &
    local MON_PID=$!
    echo "[mon] PID=$MON_PID started"

    # Trap to ensure monitor killed even on Ctrl-C
    trap "kill $MON_PID 2>/dev/null; exit 130" INT TERM

    # Run blind test (foreground)
    python3 "$SCRIPT_DIR/router_blind_test.py" \
        --mode "$MODE" \
        --csv "$CSV" \
        --n-per-persona "$N" \
        --seed "$SEED" \
        --output "$OUT" \
        $RESUME \
        2>&1 | tee "$LOG"

    # Stop monitor
    kill -TERM $MON_PID 2>/dev/null || true
    wait $MON_PID 2>/dev/null || true
    trap - INT TERM
    echo "[mon] PID=$MON_PID stopped"
}

# ── Layer 2 + 3: Mode A then Mode B ─────────────────────────────────────────
run_mode A
echo "[orchestrator] sleeping 30s between modes for system to settle"
sleep 30
run_mode B

# ── Final report ────────────────────────────────────────────────────────────
echo
echo "============================================================"
echo "Generating final IEEE-grade report"
echo "============================================================"
python3 "$SCRIPT_DIR/router_blind_analyze.py" \
    --mode-a     "$RESULTS/mode_A_n${N}_seed${SEED}.jsonl" \
    --mode-b     "$RESULTS/mode_B_n${N}_seed${SEED}.jsonl" \
    --resource-a "$RESULTS/resources_modeA.jsonl" \
    --resource-b "$RESULTS/resources_modeB.jsonl" \
    --output-dir "$RESULTS/report" \
    2>&1 | tee "$RESULTS/analyze.log"

echo
echo "============================================================"
echo "Validation pipeline complete"
echo "============================================================"
echo "Artifacts:"
echo "  $RESULTS/temporal_split/temporal_split_report.txt"
echo "  $RESULTS/mode_A_n${N}_seed${SEED}.jsonl"
echo "  $RESULTS/mode_B_n${N}_seed${SEED}.jsonl"
echo "  $RESULTS/report/validation_report.txt"
echo "  $RESULTS/report/accuracy_table.tex"
echo "  $RESULTS/report/confusion_modeA.png"
echo "  $RESULTS/report/confusion_modeB.png"
echo "  $RESULTS/report/resources_mode{A,B}.png"
echo "  $RESULTS/report/all_flows.csv"
echo
