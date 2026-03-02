#!/bin/bash
# MycoFlow QEMU Lab — Network topology + OpenWrt VM boot
#
# Topoloji:
#   [netns:server 10.0.1.2] ──br-wan── [QEMU OpenWrt WAN=10.0.1.1 / LAN=192.168.1.1] ──br-lan── [gamer 192.168.1.10]
#                                                                                                    [bulk  192.168.1.20]
#
# OpenWrt x86-64 varsayılan ağ yapılandırması:
#   eth0 → br-lan (LAN, 192.168.1.1)  — ilk NIC
#   eth1 → wan (DHCP)                 — ikinci NIC
#
# Bu yüzden QEMU'da NIC sırası: LAN ilk, WAN ikinci

set -e

WAN_BW_KBIT="${WAN_BW_KBIT:-20000}"
OPENWRT_IMG="/openwrt.img"
OPENWRT_DISK="/openwrt-run.img"

log() { echo "[qemu-lab] $*"; }

# ── 0. OpenWrt disk image hazırla ──────────────────────────────────────────
log "Preparing OpenWrt disk image..."
cp "$OPENWRT_IMG" "$OPENWRT_DISK"
qemu-img resize -f raw "$OPENWRT_DISK" 512M 2>/dev/null || true

# ── 1. Bridge'ler oluştur ─────────────────────────────────────────────────
log "Creating bridges..."

# WAN bridge: server + OpenWrt WAN arasında
ip link add br-wan type bridge 2>/dev/null || true
ip link set br-wan up
ip addr add 10.0.1.254/24 dev br-wan 2>/dev/null || true

# LAN bridge: clients + OpenWrt LAN arasında
# OpenWrt varsayılan: 192.168.1.1/24
ip link add br-lan type bridge 2>/dev/null || true
ip link set br-lan up
ip addr add 192.168.1.254/24 dev br-lan 2>/dev/null || true

# ── 2. TAP devices (QEMU NIC'ler) ─────────────────────────────────────────
log "Creating tap devices for QEMU..."

# tap-lan: OpenWrt'de eth0 olacak → br-lan'a girer (LAN)
ip tuntap add tap-lan mode tap 2>/dev/null || true
ip link set tap-lan up
ip link set tap-lan master br-lan

# tap-wan: OpenWrt'de eth1 olacak → wan interface (DHCP→static yapılacak)
ip tuntap add tap-wan mode tap 2>/dev/null || true
ip link set tap-wan up
ip link set tap-wan master br-wan

# ── 3. Server netns (WAN tarafı) ──────────────────────────────────────────
log "Creating server netns (10.0.1.2)..."
ip netns del server 2>/dev/null || true
ip netns add server

ip link add veth-srv type veth peer name veth-srv-br 2>/dev/null || true
ip link set veth-srv netns server
ip link set veth-srv-br master br-wan
ip link set veth-srv-br up

ip netns exec server ip link set lo up
ip netns exec server ip link set veth-srv up
ip netns exec server ip addr add 10.0.1.2/24 dev veth-srv
ip netns exec server ip route add default via 10.0.1.1

# ── 4. Gamer netns (LAN tarafı, 192.168.1.10) ────────────────────────────
log "Creating gamer netns (192.168.1.10)..."
ip netns del gamer 2>/dev/null || true
ip netns add gamer

ip link add veth-cli1 type veth peer name veth-cli1-br 2>/dev/null || true
ip link set veth-cli1 netns gamer
ip link set veth-cli1-br master br-lan
ip link set veth-cli1-br up

ip netns exec gamer ip link set lo up
ip netns exec gamer ip link set veth-cli1 up
ip netns exec gamer ip addr add 192.168.1.10/24 dev veth-cli1
ip netns exec gamer ip route add default via 192.168.1.1

# ── 5. Bulk netns (LAN tarafı, 192.168.1.20) ─────────────────────────────
log "Creating bulk netns (192.168.1.20)..."
ip netns del bulk 2>/dev/null || true
ip netns add bulk

ip link add veth-cli2 type veth peer name veth-cli2-br 2>/dev/null || true
ip link set veth-cli2 netns bulk
ip link set veth-cli2-br master br-lan
ip link set veth-cli2-br up

ip netns exec bulk ip link set lo up
ip netns exec bulk ip link set veth-cli2 up
ip netns exec bulk ip addr add 192.168.1.20/24 dev veth-cli2
ip netns exec bulk ip route add default via 192.168.1.1

# ── 6. VoIP netns (LAN tarafı, 192.168.1.11) ─────────────────────────────
log "Creating voip netns (192.168.1.11)..."
ip netns del voip 2>/dev/null || true
ip netns add voip

ip link add veth-cli3 type veth peer name veth-cli3-br 2>/dev/null || true
ip link set veth-cli3 netns voip
ip link set veth-cli3-br master br-lan
ip link set veth-cli3-br up

ip netns exec voip ip link set lo up
ip netns exec voip ip link set veth-cli3 up
ip netns exec voip ip addr add 192.168.1.11/24 dev veth-cli3
ip netns exec voip ip route add default via 192.168.1.1

# ── 7. Video netns (LAN tarafı, 192.168.1.12) ────────────────────────────
log "Creating video netns (192.168.1.12)..."
ip netns del video 2>/dev/null || true
ip netns add video

ip link add veth-cli4 type veth peer name veth-cli4-br 2>/dev/null || true
ip link set veth-cli4 netns video
ip link set veth-cli4-br master br-lan
ip link set veth-cli4-br up

ip netns exec video ip link set lo up
ip netns exec video ip link set veth-cli4 up
ip netns exec video ip addr add 192.168.1.12/24 dev veth-cli4
ip netns exec video ip route add default via 192.168.1.1

# ── 8. Stream netns (LAN tarafı, 192.168.1.13) ───────────────────────────
log "Creating stream netns (192.168.1.13)..."
ip netns del stream 2>/dev/null || true
ip netns add stream

ip link add veth-cli5 type veth peer name veth-cli5-br 2>/dev/null || true
ip link set veth-cli5 netns stream
ip link set veth-cli5-br master br-lan
ip link set veth-cli5-br up

ip netns exec stream ip link set lo up
ip netns exec stream ip link set veth-cli5 up
ip netns exec stream ip addr add 192.168.1.13/24 dev veth-cli5
ip netns exec stream ip route add default via 192.168.1.1

# ── 9. Torrent netns (LAN tarafı, 192.168.1.14) ──────────────────────────
log "Creating torrent netns (192.168.1.14)..."
ip netns del torrent 2>/dev/null || true
ip netns add torrent

ip link add veth-cli6 type veth peer name veth-cli6-br 2>/dev/null || true
ip link set veth-cli6 netns torrent
ip link set veth-cli6-br master br-lan
ip link set veth-cli6-br up

ip netns exec torrent ip link set lo up
ip netns exec torrent ip link set veth-cli6 up
ip netns exec torrent ip addr add 192.168.1.14/24 dev veth-cli6
ip netns exec torrent ip route add default via 192.168.1.1

# ── 10. QEMU başlat ───────────────────────────────────────────────────────
KVM_FLAG=""
if [ -e /dev/kvm ]; then
    KVM_FLAG="-enable-kvm"
    log "KVM acceleration available"
else
    log "KVM not available, using TCG (slower but works)"
fi

log "Starting QEMU OpenWrt VM..."
# NIC sırası önemli:
#   İlk NIC (tap-lan) → OpenWrt eth0 → br-lan (LAN, 192.168.1.1)
#   İkinci NIC (tap-wan) → OpenWrt eth1 → wan (DHCP→static 10.0.1.1)
#   Üçüncü NIC (user-mode NAT) → OpenWrt eth2 → internet (opkg install için)
qemu-system-x86_64 \
    $KVM_FLAG \
    -m 256 -smp 2 \
    -drive file="$OPENWRT_DISK",format=raw,if=virtio \
    -netdev tap,id=lan,ifname=tap-lan,script=no,downscript=no \
    -device e1000,netdev=lan,mac=52:54:00:00:02:01 \
    -netdev tap,id=wan,ifname=tap-wan,script=no,downscript=no \
    -device e1000,netdev=wan,mac=52:54:00:00:01:01 \
    -netdev user,id=mgmt,net=10.0.99.0/24,dhcpstart=10.0.99.10 \
    -device e1000,netdev=mgmt,mac=52:54:00:00:99:01 \
    -display none \
    -daemonize \
    -pidfile /tmp/qemu.pid \
    -serial file:/tmp/qemu-console.log

log "QEMU started (PID: $(cat /tmp/qemu.pid 2>/dev/null || echo '?'))"

# ── 11. OpenWrt boot'unu bekle ────────────────────────────────────────────
# OpenWrt varsayılan: br-lan = 192.168.1.1, dropbear SSH aktif
log "Waiting for OpenWrt to boot (SSH on 192.168.1.1)..."

MAX_WAIT=180
WAITED=0

while [ $WAITED -lt $MAX_WAIT ]; do
    if sshpass -p '' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
       -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
       root@192.168.1.1 echo ok 2>/dev/null; then
        log "OpenWrt reachable at 192.168.1.1 (LAN default)"
        break
    fi
    sleep 3
    WAITED=$((WAITED + 3))
    if [ $((WAITED % 15)) -eq 0 ]; then
        log "  waiting... (${WAITED}s/${MAX_WAIT}s)"
    fi
done

if [ $WAITED -ge $MAX_WAIT ]; then
    log "ERROR: OpenWrt VM did not become reachable in ${MAX_WAIT}s"
    log "Console log (last 30 lines):"
    tail -30 /tmp/qemu-console.log 2>/dev/null || true
    exit 1
fi

# ── 12. OpenWrt yapılandır ────────────────────────────────────────────────
log "Configuring OpenWrt..."
export OPENWRT_LAN_IP="192.168.1.1"
export WAN_BW_KBIT
/configure-openwrt.sh

# ── 13. iperf3 server başlat (multiple ports for multi-flow benchmark) ───
log "Starting iperf3 servers in server netns (ports 5201-5210)..."
# Port mapping:
#   5201: gamer (gaming traffic)
#   5202: voip (VoIP traffic — UDP)
#   5203: video (video call — bidirectional UDP)
#   5204: warm-up bulk (avoids port conflict during warm-up)
#   5205: stream (streaming — reverse TCP download)
#   5206: torrent (BitTorrent simulation — parallel TCP)
#   5207-5210: extra torrent parallel sessions
for PORT in 5201 5202 5203 5204 5205 5206 5207 5208 5209 5210; do
    ip netns exec server iperf3 -s -p ${PORT} --forceflush --daemon \
        --logfile /tmp/iperf3-server-${PORT}.log
done
sleep 1

# ── 14. Bağlantı testi ───────────────────────────────────────────────────
log "Connectivity tests..."

log "  gamer (192.168.1.10) → OpenWrt LAN (192.168.1.1)..."
ip netns exec gamer ping -c 2 -W 2 192.168.1.1 > /dev/null 2>&1 \
    && log "  OK" || log "  FAIL"

log "  gamer → server (10.0.1.2) through OpenWrt..."
if ip netns exec gamer ping -c 3 -W 3 10.0.1.2 > /dev/null 2>&1; then
    log "  OK — traffic flows through OpenWrt!"
else
    log "  FAIL — checking intermediate..."
    sshpass -p '' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR root@192.168.1.1 \
        "ping -c 1 -W 2 10.0.1.2" > /dev/null 2>&1 \
        && log "    OpenWrt → server OK" || log "    OpenWrt → server FAIL"
fi

log "  bulk (192.168.1.20) → server (10.0.1.2)..."
ip netns exec bulk ping -c 2 -W 3 10.0.1.2 > /dev/null 2>&1 \
    && log "  OK" || log "  FAIL"

log "  voip (192.168.1.11) → server..."
ip netns exec voip ping -c 2 -W 3 10.0.1.2 > /dev/null 2>&1 \
    && log "  OK" || log "  FAIL"

log "  video (192.168.1.12) → server..."
ip netns exec video ping -c 2 -W 3 10.0.1.2 > /dev/null 2>&1 \
    && log "  OK" || log "  FAIL"

log "  stream (192.168.1.13) → server..."
ip netns exec stream ping -c 2 -W 3 10.0.1.2 > /dev/null 2>&1 \
    && log "  OK" || log "  FAIL"

log "  torrent (192.168.1.14) → server..."
ip netns exec torrent ping -c 2 -W 3 10.0.1.2 > /dev/null 2>&1 \
    && log "  OK" || log "  FAIL"

# ── Done ──────────────────────────────────────────────────────────────────
log ""
log "============================================"
log "  QEMU OpenWrt Lab Ready!"
log "============================================"
log "  OpenWrt VM:   ssh root@192.168.1.1 (LAN)"
log "  WAN IP:       10.0.1.1"
log "  Server:       netns:server  10.0.1.2   (iperf3 :5201-5210)"
log "  Gamer:        netns:gamer   192.168.1.10"
log "  Bulk:         netns:bulk    192.168.1.20"
log "  VoIP:         netns:voip    192.168.1.11"
log "  Video:        netns:video   192.168.1.12"
log "  Stream:       netns:stream  192.168.1.13"
log "  Torrent:      netns:torrent 192.168.1.14"
log "  WAN BW:       ${WAN_BW_KBIT} kbit/s"
log "  Benchmark:    /bench/qemu-bench.sh"
log "============================================"

exec sleep infinity
