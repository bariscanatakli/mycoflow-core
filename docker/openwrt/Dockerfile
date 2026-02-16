# Stage 1: Build mycoflowd (static with glibc)
FROM ubuntu:22.04 AS builder

RUN apt-get update -qq && apt-get install -y -qq \
    cmake gcc make pkg-config \
    zlib1g-dev libelf-dev \
    linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/ /src/
COPY CMakeLists.txt /CMakeLists.txt

# Build static binary — no ubus (auto-skipped by CMake), no libbpf in static
RUN mkdir -p /build && cd /build && \
    cmake /src/.. \
      -DCMAKE_EXE_LINKER_FLAGS="-static" \
      2>&1 && \
    make -j$(nproc) mycoflowd 2>&1 && \
    strip src/mycoflowd && \
    ls -la src/mycoflowd

# Stage 2: OpenWrt with LuCI
FROM openwrt/rootfs:x86-64-23.05.5

# Install packages and upgrade core
RUN mkdir -p /var/lock /var/run /var/opkg-lists && \
    opkg update && \
    opkg upgrade ubus rpcd uhttpd procd netifd libubox libubus libjson-c && \
    opkg install \
        luci \
        luci-compat \
        luci-app-opkg \
        lua \
        libubus-lua \
        libubox-lua \
        luci-lib-nixio \
        luci-lib-jsonc \
    && rm -rf /var/opkg-lists/*

# Copy built binary
COPY --from=builder /build/src/mycoflowd /usr/bin/mycoflowd
RUN chmod +x /usr/bin/mycoflowd

# Copy LuCI app files
COPY luci-app-mycoflow/htdocs/luci-static/resources/view/mycoflow.js \
     /www/luci-static/resources/view/mycoflow.js
COPY luci-app-mycoflow/root/usr/share/luci/menu.d/luci-app-mycoflow.json \
     /usr/share/luci/menu.d/luci-app-mycoflow.json
COPY luci-app-mycoflow/root/usr/share/rpcd/acl.d/luci-app-mycoflow.json \
     /usr/share/rpcd/acl.d/luci-app-mycoflow.json

# Copy UCI config
COPY openwrt/files/etc/config/mycoflow /etc/config/mycoflow
COPY openwrt/files/etc/init.d/mycoflowd /etc/init.d/mycoflowd
RUN chmod +x /etc/init.d/mycoflowd

# Copy setup script to uci-defaults (runs on first boot)
COPY docker/openwrt/setup.sh /etc/uci-defaults/99-mycoflow-setup
RUN chmod +x /etc/uci-defaults/99-mycoflow-setup

# Copy bridge
COPY docker/openwrt/myco_bridge.lua /usr/sbin/myco_bridge.lua

EXPOSE 80

CMD ["/sbin/init"]
