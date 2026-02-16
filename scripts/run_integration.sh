#!/usr/bin/env bash
# MycoFlow Integration Test Harness
# Runs build, unit tests, and a short daemon smoke test in a Docker container.
#
# Usage: ./scripts/run_integration.sh [--no-docker]
#   --no-docker  Run tests directly on the host (requires cmake, gcc, clang)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="/tmp/mycoflow-build"
DAEMON_TIMEOUT=6
USE_DOCKER=true

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "${GREEN}✓ $1${NC}"; }
fail() { echo -e "${RED}✗ $1${NC}"; FAILURES=$((FAILURES + 1)); }
info() { echo -e "${YELLOW}→ $1${NC}"; }

FAILURES=0

# Parse args
for arg in "$@"; do
    case $arg in
        --no-docker) USE_DOCKER=false ;;
    esac
done

run_tests() {
    local ws="$1"

    info "Step 1: CMake configure"
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake "$ws" 2>&1 | tail -5
    pass "CMake configured"

    info "Step 2: Build"
    make -j"$(nproc)" 2>&1 | tail -3
    pass "Build succeeded"

    info "Step 3: BPF object check"
    if [ -f src/mycoflow.bpf.o ]; then
        pass "BPF object generated ($(stat -c%s src/mycoflow.bpf.o) bytes)"
    else
        fail "BPF object NOT found (clang may be missing)"
    fi

    info "Step 4: Unit tests (CTest)"
    if ctest --output-on-failure 2>&1; then
        pass "All unit tests passed"
    else
        fail "Unit tests failed"
    fi

    info "Step 5: Daemon smoke test (${DAEMON_TIMEOUT}s)"
    local LOG="/tmp/mycoflow_smoke.log"
    timeout "$DAEMON_TIMEOUT" src/mycoflowd > "$LOG" 2>&1 || true

    # Check log output
    if grep -q "MycoFlow daemon starting" "$LOG"; then
        pass "Daemon started"
    else
        fail "Daemon did not start"
    fi

    if grep -q "netlink socket ready" "$LOG" 2>/dev/null; then
        pass "Netlink socket initialized"
    else
        fail "Netlink socket failed"
    fi

    if grep -q "baseline" "$LOG"; then
        pass "Baseline captured"
    else
        fail "Baseline not captured"
    fi

    if grep -q "loop:" "$LOG"; then
        pass "Main loop executed"
        # Show a sample loop line
        grep "loop:" "$LOG" | head -1
    else
        fail "Main loop did not execute"
    fi

    if grep -q "shutdown complete" "$LOG"; then
        pass "Clean shutdown"
    else
        fail "No clean shutdown (may have been killed by timeout)"
    fi

    echo ""
    echo "═══════════════════════════════════════════"
    if [ "$FAILURES" -eq 0 ]; then
        echo -e "${GREEN}ALL CHECKS PASSED${NC}"
    else
        echo -e "${RED}${FAILURES} CHECK(S) FAILED${NC}"
    fi
    echo "═══════════════════════════════════════════"
    return "$FAILURES"
}

if $USE_DOCKER; then
    info "Running in Docker (ubuntu:22.04, --privileged)"
    docker run --rm \
        -v "$PROJECT_DIR:/workspace" \
        -w /workspace \
        --privileged \
        ubuntu:22.04 \
        bash -c "
            apt-get update -qq && \
            apt-get install -y -qq cmake gcc clang llvm libbpf-dev \
                pkg-config zlib1g-dev libelf-dev iproute2 >/dev/null 2>&1 && \
            bash /workspace/scripts/run_integration.sh --no-docker
        "
else
    run_tests "$PROJECT_DIR"
fi
