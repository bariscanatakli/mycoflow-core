#!/usr/bin/env bash
# MycoFlow Stress / Stability Test
# Runs mycoflowd for extended periods with monitoring for crashes and leaks.
#
# Usage: ./scripts/stress_test.sh [OPTIONS]
#   --duration HOURS    Test duration in hours (default: 1)
#   --valgrind          Run under Valgrind for leak detection
#   --asan              Build with AddressSanitizer
#   --output DIR        Output directory (default: stress_results/)

set -euo pipefail

DURATION_HOURS=1
USE_VALGRIND=false
USE_ASAN=false
OUTDIR="stress_results"
BINARY=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

info() { echo -e "${CYAN}[STRESS]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
pass() { echo -e "${GREEN}[OK]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; }

while [[ $# -gt 0 ]]; do
    case $1 in
        --duration)  DURATION_HOURS="$2"; shift 2 ;;
        --valgrind)  USE_VALGRIND=true;   shift ;;
        --asan)      USE_ASAN=true;       shift ;;
        --output)    OUTDIR="$2";         shift 2 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

DURATION_SEC=$((DURATION_HOURS * 3600))
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
mkdir -p "$OUTDIR"

LOGFILE="$OUTDIR/stress_${TIMESTAMP}.log"
REPORT="$OUTDIR/stress_report_${TIMESTAMP}.txt"

info "MycoFlow Stress Test"
info "Duration: ${DURATION_HOURS}h (${DURATION_SEC}s) | Valgrind: $USE_VALGRIND | ASan: $USE_ASAN"

# ── Build ───────────────────────────────────────────────────
BUILD_DIR="/tmp/mycoflow-stress-build"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

info "Building mycoflowd..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

CMAKE_OPTS=""
if $USE_ASAN; then
    CMAKE_OPTS="-DCMAKE_C_FLAGS='-fsanitize=address -fno-omit-frame-pointer -g'"
fi

cmake "$PROJECT_DIR" $CMAKE_OPTS > /dev/null 2>&1
make -j"$(nproc)" > /dev/null 2>&1
BINARY="$BUILD_DIR/src/mycoflowd"
pass "Build complete: $BINARY"

# ── Pre-test snapshot ───────────────────────────────────────
echo "=== MycoFlow Stress Test Report ===" > "$REPORT"
echo "Start: $(date)" >> "$REPORT"
echo "Duration: ${DURATION_HOURS}h" >> "$REPORT"
echo "Valgrind: $USE_VALGRIND | ASan: $USE_ASAN" >> "$REPORT"
echo "" >> "$REPORT"

# ── Run daemon ──────────────────────────────────────────────
info "Starting mycoflowd (timeout: ${DURATION_SEC}s)..."

VALGRIND_LOG=""
if $USE_VALGRIND; then
    VALGRIND_LOG="$OUTDIR/valgrind_${TIMESTAMP}.log"
    info "Valgrind output: $VALGRIND_LOG"
    timeout "$DURATION_SEC" valgrind \
        --leak-check=full \
        --show-leak-kinds=all \
        --track-origins=yes \
        --log-file="$VALGRIND_LOG" \
        "$BINARY" > "$LOGFILE" 2>&1 || true
else
    timeout "$DURATION_SEC" "$BINARY" > "$LOGFILE" 2>&1 || true
fi

# ── Post-test analysis ─────────────────────────────────────
info "Analyzing results..."

FAILURES=0

# Check 1: Did it start?
if grep -q "MycoFlow daemon starting" "$LOGFILE"; then
    pass "Daemon started successfully"
    echo "✓ Daemon started" >> "$REPORT"
else
    fail "Daemon failed to start"
    echo "✗ Daemon failed to start" >> "$REPORT"
    FAILURES=$((FAILURES + 1))
fi

# Check 2: Did it run loops?
LOOP_COUNT=$(grep -c "loop:" "$LOGFILE" 2>/dev/null || echo "0")
info "Loop iterations: $LOOP_COUNT"
echo "Loop iterations: $LOOP_COUNT" >> "$REPORT"
if [ "$LOOP_COUNT" -gt 0 ]; then
    pass "Main loop executed ($LOOP_COUNT iterations)"
else
    fail "No loop iterations detected"
    FAILURES=$((FAILURES + 1))
fi

# Check 3: Any crashes / segfaults?
if grep -qi "segfault\|segmentation\|SIGSEGV\|abort\|SIGABRT" "$LOGFILE"; then
    fail "Crash detected in logs"
    echo "✗ Crash detected" >> "$REPORT"
    FAILURES=$((FAILURES + 1))
else
    pass "No crashes detected"
    echo "✓ No crashes" >> "$REPORT"
fi

# Check 4: Clean shutdown?
if grep -q "shutdown complete" "$LOGFILE"; then
    pass "Clean shutdown"
    echo "✓ Clean shutdown" >> "$REPORT"
else
    warn "No clean shutdown (killed by timeout — expected)"
    echo "△ Killed by timeout (expected)" >> "$REPORT"
fi

# Check 5: Valgrind leaks
if $USE_VALGRIND && [ -f "$VALGRIND_LOG" ]; then
    LEAKS=$(grep "definitely lost:" "$VALGRIND_LOG" | grep -v "0 bytes" || true)
    if [ -z "$LEAKS" ]; then
        pass "No memory leaks (Valgrind)"
        echo "✓ No memory leaks" >> "$REPORT"
    else
        fail "Memory leaks found: $LEAKS"
        echo "✗ Memory leaks: $LEAKS" >> "$REPORT"
        FAILURES=$((FAILURES + 1))
    fi
fi

# Check 6: ASan errors
if $USE_ASAN; then
    ASAN_ERRORS=$(grep -c "ERROR: AddressSanitizer" "$LOGFILE" 2>/dev/null || echo "0")
    if [ "$ASAN_ERRORS" -eq 0 ]; then
        pass "No ASan errors"
        echo "✓ No ASan errors" >> "$REPORT"
    else
        fail "$ASAN_ERRORS ASan errors found"
        echo "✗ $ASAN_ERRORS ASan errors" >> "$REPORT"
        FAILURES=$((FAILURES + 1))
    fi
fi

# ── Summary ─────────────────────────────────────────────────
echo "" >> "$REPORT"
echo "End: $(date)" >> "$REPORT"
echo "Failures: $FAILURES" >> "$REPORT"

echo ""
info "═══════════════════════════════════════════"
if [ "$FAILURES" -eq 0 ]; then
    pass "STRESS TEST PASSED"
else
    fail "$FAILURES CHECK(S) FAILED"
fi
info "Report: $REPORT"
info "Log: $LOGFILE"
if $USE_VALGRIND; then info "Valgrind: $VALGRIND_LOG"; fi
info "═══════════════════════════════════════════"

exit "$FAILURES"
