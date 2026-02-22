#!/bin/sh
# Router container init — NAT + initial tc + mycoflowd

WAN_BW_KBIT="${WAN_BW_KBIT:-100000}"

# ── Auto-detect WAN/LAN interfaces ───────────────────────────────────────
# WAN = default route interface (Docker bridge → internet)
# LAN = the other non-loopback ethernet interface
# NOTE: Strip @ifNN suffix that Docker veth pairs add — iptables/tc need clean names
WAN_IFACE=$(ip route show default 2>/dev/null \
    | awk '/default/ {print $5}' | sed 's/@.*//' | head -1)
WAN_IFACE="${WAN_IFACE:-eth0}"

# LAN = first non-lo, non-WAN interface (also strip @ifNN)
LAN_IFACE=$(ip -o link show | awk -F': ' '{print $2}' | sed 's/@.*//' \
    | grep -v lo | grep -v "^${WAN_IFACE}$" | head -1)
LAN_IFACE="${LAN_IFACE:-eth1}"

# Export for child processes (mycoflowd, benchmark script)
export WAN_IFACE LAN_IFACE

echo "[router] Auto-detected: WAN=$WAN_IFACE  LAN=$LAN_IFACE  BW=${WAN_BW_KBIT}kbit"

# ── NAT: LAN → WAN ───────────────────────────────────────────────────────
iptables -t nat -A POSTROUTING -o "$WAN_IFACE" -j MASQUERADE
iptables -A FORWARD -i "$LAN_IFACE" -o "$WAN_IFACE" -j ACCEPT
iptables -A FORWARD -i "$WAN_IFACE" -o "$LAN_IFACE" \
    -m state --state RELATED,ESTABLISHED -j ACCEPT
echo "[router] NAT configured"

# ── IFB: ingress shaping (best-effort, WSL2'de olmayabilir) ─────────────
if modprobe ifb 2>/dev/null; then
    echo "[router] ifb module loaded — ingress shaping available"
    ip link add ifb0 type ifb 2>/dev/null || true
    ip link set ifb0 up 2>/dev/null || true
else
    echo "[router] ifb not available — ingress shaping skipped"
fi

# ── Initial WAN shaping: HTB + fq_codel ──────────────────────────────────
# Bandwidth cap gerekli: yoksa Docker link 10G → bufferbloat gözlemlenemez.
# CAKE tercih edilir (OpenWrt'de mevcut), fq_codel/htb Docker/WSL2 fallback.
# Benchmark script her senaryo öncesi bunu değiştirecek.
if tc qdisc replace dev "$WAN_IFACE" root cake bandwidth "${WAN_BW_KBIT}kbit" 2>/dev/null; then
    echo "[router] Initial qdisc: CAKE ${WAN_BW_KBIT}kbit on $WAN_IFACE"
else
    # Fallback: HTB (rate limiter) + fq_codel (AQM)
    tc qdisc replace dev "$WAN_IFACE" root handle 1: htb default 10
    tc class replace dev "$WAN_IFACE" parent 1: classid 1:10 htb rate "${WAN_BW_KBIT}kbit" burst 32kbit
    tc qdisc replace dev "$WAN_IFACE" parent 1:10 handle 10: fq_codel
    echo "[router] Initial qdisc: HTB+fq_codel ${WAN_BW_KBIT}kbit on $WAN_IFACE (CAKE unavailable)"
fi

# ── mycoflowd ─────────────────────────────────────────────────────────────
# Başlangıçta durdurulmuş; benchmark script ihtiyaç duyunca başlatacak
echo "[router] mycoflowd binary: $(ls -lh /usr/sbin/mycoflowd 2>/dev/null || echo 'NOT FOUND')"
echo "[router] Ready. Waiting for benchmark commands..."

exec sleep infinity
