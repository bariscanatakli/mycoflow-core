#!/bin/bash
# OpenWrt VM'i SSH üzerinden yapılandır
# setup.sh tarafından çağrılır, container içinde çalışır

set -e

OPENWRT_LAN_IP="${OPENWRT_LAN_IP:-192.168.1.1}"
WAN_BW_KBIT="${WAN_BW_KBIT:-20000}"

SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

log() { echo "[configure] $*"; }

owrt() {
    sshpass -p '' ssh $SSH_OPTS root@"$OPENWRT_LAN_IP" "$@"
}

# ── 1. Management NIC: eth2 → internet (opkg install için) ───────────────
log "Configuring management NIC (eth2) for internet access..."

owrt "
# eth2 = QEMU user-mode NAT → internet erişimi
uci set network.mgmt=interface
uci set network.mgmt.device='eth2'
uci set network.mgmt.proto='dhcp'

# Firewall: mgmt zone'u wan'a ekle
uci add_list firewall.@zone[1].network='mgmt'

uci commit network
uci commit firewall
/etc/init.d/network restart
sleep 5
/etc/init.d/firewall restart
sleep 3
"

# eth2 DHCP IP aldı mı kontrol et
RETRIES=15
for i in $(seq 1 $RETRIES); do
    if owrt "ip addr show eth2 2>/dev/null | grep -q 'inet '" 2>/dev/null; then
        log "eth2 got IP (internet access)"
        break
    fi
    sleep 2
    [ "$i" -eq "$RETRIES" ] && log "WARN: eth2 no IP — opkg may fail"
done

# DNS fix — QEMU user-mode gateway is DNS proxy
owrt "
if ! ping -c 1 -W 3 downloads.openwrt.org > /dev/null 2>&1; then
    # QEMU user-mode default gateway = DNS proxy
    GW=\$(ip route show dev eth2 2>/dev/null | awk '/default/ {print \$3}')
    [ -n \"\$GW\" ] && echo \"nameserver \$GW\" >> /tmp/resolv.conf.d/resolv.conf.auto
    echo 'nameserver 8.8.8.8' >> /tmp/resolv.conf.d/resolv.conf.auto
fi
" 2>/dev/null || true

# ── 2. CAKE ve gerekli paketleri yükle ───────────────────────────────────
log "Installing required packages via opkg..."

owrt "
opkg update 2>&1 | tail -3

# CAKE kernel modülü
opkg install kmod-sched-cake 2>&1 | tail -3 || true
insmod sch_cake 2>/dev/null || true

# tc
opkg install tc-full 2>&1 | tail -3 || true

# iptables (OpenWrt 23.05 uses nftables by default, need iptables-nft compat)
opkg install iptables-nft 2>&1 | tail -3 || true
opkg install iptables-mod-extra 2>&1 | tail -3 || true
opkg install kmod-ipt-extra 2>&1 | tail -3 || true
opkg install iptables-mod-ipopt 2>&1 | tail -3 || true

# conntrack
opkg install kmod-nf-conntrack 2>&1 | tail -3 || true

echo '--- Verify ---'

echo -n 'CAKE: '
if tc qdisc add dev lo root cake bandwidth 1000kbit 2>/dev/null; then
    tc qdisc del dev lo root 2>/dev/null
    echo 'OK'
else
    echo 'FAILED'
fi

echo -n 'DSCP: '
if iptables -t mangle -N _test 2>/dev/null; then
    if iptables -t mangle -A _test -j DSCP --set-dscp-class cs4 2>/dev/null; then
        echo 'OK'
    else
        echo 'FAILED'
    fi
    iptables -t mangle -F _test 2>/dev/null
    iptables -t mangle -X _test 2>/dev/null
else
    echo 'SKIP (chain exists)'
fi

echo -n 'conntrack: '
[ -f /proc/net/nf_conntrack ] && echo 'OK' || echo 'MISSING'
"

# ── 3. WAN interface: static 10.0.1.1 ────────────────────────────────────
log "Configuring WAN interface (eth1 → 10.0.1.1)..."

owrt "
uci set network.wan.proto='static'
uci set network.wan.ipaddr='10.0.1.1'
uci set network.wan.netmask='255.255.255.0'
uci delete network.wan.gateway 2>/dev/null; true
uci delete network.wan.dns 2>/dev/null; true
uci delete network.wan6 2>/dev/null; true

# Firewall: WAN zone input kabul + masquerade
uci set firewall.@zone[1].input='ACCEPT'
uci set firewall.@zone[1].masq='1'
uci set firewall.@zone[1].mtu_fix='1'

uci commit network
uci commit firewall
/etc/init.d/network restart
sleep 3
/etc/init.d/firewall restart
sleep 2
"

# SSH hâlâ çalışıyor mu?
RETRIES=10
for i in $(seq 1 $RETRIES); do
    if owrt "echo ok" 2>/dev/null; then
        log "OpenWrt reachable after network restart"
        break
    fi
    sleep 2
    [ "$i" -eq "$RETRIES" ] && { log "ERROR: Lost SSH after restart"; exit 1; }
done

# ── 4. mycoflowd binary yükle (ssh + cat, sftp yok) ──────────────────────
log "Uploading mycoflowd binary..."
sshpass -p '' ssh $SSH_OPTS root@"$OPENWRT_LAN_IP" \
    'cat > /usr/bin/mycoflowd && chmod +x /usr/bin/mycoflowd' < /opt/mycoflowd
owrt "ls -lh /usr/bin/mycoflowd"
log "mycoflowd installed"

# ── 5. Bağlantı doğrulama ────────────────────────────────────────────────
log "Verifying connectivity from OpenWrt..."
owrt "ping -c 1 -W 2 192.168.1.10" > /dev/null 2>&1 && log "  → gamer OK" || log "  → gamer FAIL"
owrt "ping -c 1 -W 2 192.168.1.20" > /dev/null 2>&1 && log "  → bulk OK" || log "  → bulk FAIL"
owrt "ping -c 1 -W 2 10.0.1.2" > /dev/null 2>&1 && log "  → server OK" || log "  → server FAIL"

log "OpenWrt configuration complete"
