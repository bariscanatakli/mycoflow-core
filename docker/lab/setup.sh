#!/bin/bash
# MycoFlow Lab — ip netns topolojisi
#
# Topoloji:
#   [netns:server 10.0.1.2] <──veth-wan──> [netns:router 10.0.1.1/10.0.2.1] <──veth-lan──> [netns:client 10.0.2.2]
#
# Router netns: tc shaping + mycoflowd + NAT
# Traffic client→server gerçekten router netns'inden geçer.

set -e

# 20Mbit: bufferbloat'ı görünür yapar, iperf3 için yeterli hız
# Gerçek OpenWrt testi 100Mbit+ ile yapılır; Docker'da 20Mbit daha dramatik sonuç verir
WAN_BW_KBIT="${WAN_BW_KBIT:-20000}"
SERVER_IP="10.0.1.2"
ROUTER_WAN_IP="10.0.1.1"
ROUTER_LAN_IP="10.0.2.1"
CLIENT_IP="10.0.2.2"

echo "[lab] Creating network namespaces..."
# Temizle (restart idempotency)
ip netns del server 2>/dev/null || true
ip netns del router 2>/dev/null || true
ip netns del client 2>/dev/null || true

ip netns add server
ip netns add router
ip netns add client

echo "[lab] Creating veth pairs..."
# WAN link: server ←─── veth-srv/veth-wan ───→ router
ip link add veth-srv type veth peer name veth-wan
ip link set veth-srv netns server
ip link set veth-wan netns router

# LAN link: router ←─── veth-lan/veth-cli ───→ client
ip link add veth-lan type veth peer name veth-cli
ip link set veth-lan netns router
ip link set veth-cli netns client

echo "[lab] Configuring server netns..."
ip netns exec server ip link set lo up
ip netns exec server ip link set veth-srv up
ip netns exec server ip addr add "${SERVER_IP}/24" dev veth-srv
ip netns exec server ip route add default via "$ROUTER_WAN_IP"

echo "[lab] Configuring router netns..."
ip netns exec router ip link set lo up
ip netns exec router ip link set veth-wan up
ip netns exec router ip link set veth-lan up
ip netns exec router ip addr add "${ROUTER_WAN_IP}/24" dev veth-wan
ip netns exec router ip addr add "${ROUTER_LAN_IP}/24" dev veth-lan
ip netns exec router sysctl -qw net.ipv4.ip_forward=1

# NAT: client'ı server'a yönlendir
ip netns exec router iptables -t nat -A POSTROUTING -o veth-wan -j MASQUERADE
ip netns exec router iptables -A FORWARD -i veth-lan -o veth-wan -j ACCEPT
ip netns exec router iptables -A FORWARD -i veth-wan -o veth-lan \
    -m state --state RELATED,ESTABLISHED -j ACCEPT

echo "[lab] Configuring client netns..."
ip netns exec client ip link set lo up
ip netns exec client ip link set veth-cli up
ip netns exec client ip addr add "${CLIENT_IP}/24" dev veth-cli
ip netns exec client ip route add default via "$ROUTER_LAN_IP"

echo "[lab] Setting initial WAN shaping: HTB+fq_codel ${WAN_BW_KBIT}kbit..."
# Bandwidth cap — yoksa veth link 10G → bufferbloat gözlemlenemez
ip netns exec router tc qdisc replace dev veth-wan root handle 1: htb default 10
ip netns exec router tc class replace dev veth-wan parent 1: classid 1:10 \
    htb rate "${WAN_BW_KBIT}kbit" burst 32kbit
ip netns exec router tc qdisc replace dev veth-wan parent 1:10 handle 10: fq_codel

echo "[lab] Starting iperf3 server in server netns..."
ip netns exec server iperf3 -s --forceflush --daemon \
    --logfile /tmp/iperf3-server.log
sleep 1

echo "[lab] Connectivity check: client → server..."
if ip netns exec client ping -c 2 -W 2 "$SERVER_IP" > /dev/null 2>&1; then
    echo "[lab] ✓ Client → Server OK"
else
    echo "[lab] ✗ Connectivity FAILED — check NAT/forwarding"
fi

echo "[lab] mycoflowd: $(mycoflowd --version 2>/dev/null || ls -lh /usr/sbin/mycoflowd)"
echo ""
echo "[lab] Lab ready!"
echo "[lab]   Server:  ip netns exec server <cmd>"
echo "[lab]   Router:  ip netns exec router <cmd>"
echo "[lab]   Client:  ip netns exec client <cmd>"
echo "[lab]   iperf3 target: $SERVER_IP"
echo "[lab]   Benchmark: /bench/docker-bench.sh"

exec sleep infinity
