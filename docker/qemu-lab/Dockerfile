# MycoFlow QEMU OpenWrt Lab
# Gerçek OpenWrt ortamında CAKE diffserv4 + per-device DSCP testi
#
# Mimari:
#   Docker container (privileged) içinde:
#   - QEMU VM: OpenWrt x86-64 (router, CAKE, mycoflowd)
#   - netns:server (iperf3 server, WAN tarafı)
#   - netns:gamer  (gaming client, LAN tarafı)
#   - netns:bulk   (bulk download client, LAN tarafı)
#
# Kullanım:
#   docker compose -f docker-compose.qemu.yml up --build -d
#   docker logs -f mycoflow-qemu   # wait for "Lab ready!"
#   ./scripts/qemu-bench.sh

# ── Stage 1: Build mycoflowd (static x86_64) ─────────────────────────────
FROM ubuntu:22.04 AS builder

RUN apt-get update -qq && apt-get install -y -qq \
    cmake gcc make pkg-config \
    zlib1g-dev libelf-dev linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/ /src/
COPY CMakeLists.txt /CMakeLists.txt

RUN cmake /src/.. -DSTATIC_BUILD=ON -DCMAKE_BUILD_TYPE=Release 2>&1 && \
    make -j$(nproc) mycoflowd && \
    strip src/mycoflowd && \
    ls -lh src/mycoflowd

# ── Stage 2: QEMU runtime ────────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update -qq && apt-get install -y -qq \
    qemu-system-x86 \
    iproute2 \
    iptables \
    iputils-ping \
    iperf3 \
    bridge-utils \
    openssh-client \
    sshpass \
    python3 \
    procps \
    kmod \
    wget \
    gzip \
    qemu-utils \
    && rm -rf /var/lib/apt/lists/*

# Download OpenWrt x86-64 combined image (ext4, ~120MB)
# gunzip returns exit 2 on "trailing garbage" which is normal for combined partition images
RUN wget -q -O /openwrt.img.gz \
    "https://downloads.openwrt.org/releases/23.05.5/targets/x86/64/openwrt-23.05.5-x86-64-generic-ext4-combined.img.gz" \
    && (gunzip /openwrt.img.gz || true) \
    && test -f /openwrt.img \
    && ls -lh /openwrt.img

# Copy mycoflowd binary
COPY --from=builder /build/src/mycoflowd /opt/mycoflowd
RUN chmod +x /opt/mycoflowd

# Copy lab scripts
COPY docker/qemu-lab/setup.sh /setup.sh
COPY docker/qemu-lab/configure-openwrt.sh /configure-openwrt.sh
RUN chmod +x /setup.sh /configure-openwrt.sh

CMD ["/setup.sh"]
