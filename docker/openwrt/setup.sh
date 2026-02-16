#!/bin/sh
# OpenWrt first-boot setup (runs via /etc/uci-defaults/99-mycoflow)

echo "=== MycoFlow First Boot Setup ==="

# ── Network Config ───────────────────────────────────
# Configure eth0 as LAN (bridge), eth1 as WAN (dhcp)
uci batch <<-'EOF'
set network.lan.ifname='eth0'
set network.lan.proto='static'
set network.lan.ipaddr='10.10.10.1'
set network.lan.netmask='255.255.255.0'
set network.wan.ifname='eth1'
set network.wan.proto='dhcp'
commit network

set mycoflow.core=mycoflow
set mycoflow.core.enabled='1'
set mycoflow.core.egress_iface='eth1'
set mycoflow.core.sample_hz='2'
set mycoflow.core.ewma_alpha='0.3'
set mycoflow.core.baseline_samples='5'
set mycoflow.core.max_bandwidth_kbit='100000'
set mycoflow.core.min_bandwidth_kbit='1000'
set mycoflow.core.bandwidth_step_kbit='1000'
set mycoflow.core.no_tc='1'
set mycoflow.core.dummy_metrics='0'
set mycoflow.core.ebpf_enabled='0'
commit mycoflow
EOF

# ── Enable Services ──────────────────────────────────
/etc/init.d/mycoflowd enable
/etc/init.d/rpcd enable
/etc/init.d/uhttpd enable
/etc/init.d/dnsmasq enable
/etc/init.d/firewall enable

# ── Start Bridge via rc.local ────────────────────────
# (Quick hack until we make a proper init script for the bridge)
sed -i '/exit 0/d' /etc/rc.local
echo "lua /usr/sbin/myco_bridge.lua &" >> /etc/rc.local
echo "exit 0" >> /etc/rc.local

# ── Set Root Password ────────────────────────────────
echo -e "mycoflow\nmycoflow" | passwd root

# ── Done ─────────────────────────────────────────────
# This script is self-deleting in uci-defaults, but for Docker we keep it.
exit 0
