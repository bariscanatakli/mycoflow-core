#!/bin/sh
# Traffic generation script — runs mixed profiles against the gateway
# Usage: ./generate.sh [profile]
#   profiles: bulk, interactive, mixed (default: mixed)

GATEWAY="${GATEWAY:-10.10.10.1}"
PROFILE="${1:-mixed}"

echo "=== MycoFlow Traffic Generator ==="
echo "Gateway: $GATEWAY | Profile: $PROFILE"

run_bulk() {
    echo "[BULK] Starting large download + iperf3 stream..."
    # iperf3 TCP stream (10s)
    iperf3 -c "$GATEWAY" -t 10 -P 4 2>/dev/null &
    # Large HTTP download (if server available)
    wget -q -O /dev/null "http://$GATEWAY/cgi-bin/luci" 2>/dev/null &
    wait
    echo "[BULK] Done"
}

run_interactive() {
    echo "[INTERACTIVE] Starting ping flood + small UDP packets..."
    # Rapid pings (small packets, low latency sensitive)
    ping -c 100 -i 0.05 -s 64 "$GATEWAY" > /dev/null 2>&1 &
    # Small UDP packets (gaming-like)
    iperf3 -c "$GATEWAY" -u -t 10 -b 1M -l 128 2>/dev/null &
    wait
    echo "[INTERACTIVE] Done"
}

run_mixed() {
    echo "[MIXED] Round 1: Interactive..."
    run_interactive
    sleep 2

    echo "[MIXED] Round 2: Bulk..."
    run_bulk
    sleep 2

    echo "[MIXED] Round 3: Concurrent..."
    run_interactive &
    run_bulk &
    wait
    echo "[MIXED] All rounds complete"
}

case "$PROFILE" in
    bulk)        run_bulk ;;
    interactive) run_interactive ;;
    mixed)       run_mixed ;;
    loop)
        echo "[LOOP] Continuous mixed traffic..."
        while true; do
            run_mixed
            sleep 5
        done
        ;;
    *) echo "Unknown profile: $PROFILE"; exit 1 ;;
esac
