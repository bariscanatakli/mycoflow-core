# MycoFlow DevContainer — OpenWrt Cross-Compilation Environment
# Target: Xiaomi AX3000T (MediaTek MT7981B / Filogic 820) — ARM64 (Cortex-A53)
# SDK: OpenWrt 23.05.2

FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

# ── System dependencies ──────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Native build tools (for quick syntax checks & unit tests)
    build-essential cmake gcc g++ gdb \
    # Libraries for native builds (stubs)
    libjson-c-dev \
    # OpenWrt SDK dependencies
    python3 python3-distutils python-is-python3 \
    libncurses-dev gawk unzip file \
    rsync wget curl ca-certificates \
    xz-utils git \
    # Utilities
    vim nano less tree \
    && rm -rf /var/lib/apt/lists/*

# ── OpenWrt SDK download & setup ─────────────────────────────
ENV OPENWRT_VER=23.05.2
ENV SDK_NAME=openwrt-sdk-${OPENWRT_VER}-mediatek-filogic_gcc-12.3.0_musl.Linux-x86_64
ENV SDK_URL=https://downloads.openwrt.org/releases/${OPENWRT_VER}/targets/mediatek/filogic/${SDK_NAME}.tar.xz
ENV SDK_DIR=/opt/openwrt-sdk

RUN mkdir -p /tmp/sdk && cd /tmp/sdk \
    && wget -q --show-progress "${SDK_URL}" -O sdk.tar.xz \
    && tar -xf sdk.tar.xz \
    && mv ${SDK_NAME} ${SDK_DIR} \
    && rm -rf /tmp/sdk

# ── Toolchain paths ──────────────────────────────────────────
# The staging_dir contains the cross-compiler toolchain
ENV STAGING_DIR=${SDK_DIR}/staging_dir
ENV TOOLCHAIN_DIR=${STAGING_DIR}/toolchain-aarch64_cortex-a53_gcc-12.3.0_musl
ENV PATH="${TOOLCHAIN_DIR}/bin:${PATH}"

# Verify cross-compiler is available
RUN aarch64-openwrt-linux-musl-gcc --version || echo "WARN: cross-compiler not found in expected path"

# ── CMake cross-compilation toolchain file ───────────────────
RUN cat > /opt/openwrt-toolchain.cmake << 'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_PREFIX "aarch64-openwrt-linux-musl-")
set(CMAKE_C_COMPILER   ${CROSS_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_PREFIX}g++)
set(CMAKE_FIND_ROOT_PATH $ENV{TOOLCHAIN_DIR})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

# ── Workspace ────────────────────────────────────────────────
WORKDIR /workspace
