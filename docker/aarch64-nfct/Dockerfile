# Cross-compile mycoflowd for aarch64 with libnetfilter_conntrack support.
# Ubuntu's arm64 -dev packages only ship .so, not .a — so we build static
# libs from source using the cross-compiler, then link mycoflowd statically.
#
# Usage:
#   docker build -t mycoflow-aarch64-nfct -f docker/aarch64-nfct/Dockerfile .
#   docker run --rm mycoflow-aarch64-nfct cat /build/src/mycoflowd > mycoflowd.aarch64

FROM ubuntu:22.04

RUN dpkg --add-architecture arm64 && \
    sed -i 's/^deb \(http:\/\/archive\|http:\/\/security\)\(.*\)$/deb [arch=amd64] \1\2/' /etc/apt/sources.list && \
    echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy main universe" >> /etc/apt/sources.list && \
    echo "deb [arch=arm64] http://ports.ubuntu.com/ubuntu-ports jammy-updates main universe" >> /etc/apt/sources.list && \
    apt-get update -qq && \
    apt-get install -y -qq \
        cmake make \
        gcc-aarch64-linux-gnu \
        libc6-dev-arm64-cross \
        file wget \
        pkg-config dpkg-dev \
        linux-libc-dev-arm64-cross \
    && rm -rf /var/lib/apt/lists/*

# ── Build static nfct libs from source ──────────────────────────────────────
# Ubuntu arm64 .deb only ships .so; we need .a for fully-static binary.
ENV HOST=aarch64-linux-gnu
ENV PREFIX=/opt/nfct
ENV CC=aarch64-linux-gnu-gcc
ENV CFLAGS="-I/usr/include"

# libmnl 1.0.5
RUN mkdir -p /tmp/src && cd /tmp/src && \
    wget -q https://netfilter.org/projects/libmnl/files/libmnl-1.0.5.tar.bz2 && \
    tar xf libmnl-1.0.5.tar.bz2 && cd libmnl-1.0.5 && \
    ./configure --host=$HOST --prefix=$PREFIX \
        --enable-static --disable-shared --disable-dependency-tracking && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/src/libmnl*

# libnfnetlink 1.0.2
RUN mkdir -p /tmp/src && cd /tmp/src && \
    wget -q https://netfilter.org/projects/libnfnetlink/files/libnfnetlink-1.0.2.tar.bz2 && \
    tar xf libnfnetlink-1.0.2.tar.bz2 && cd libnfnetlink-1.0.2 && \
    ./configure --host=$HOST --prefix=$PREFIX \
        --enable-static --disable-shared --disable-dependency-tracking \
        PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/src/libnfnetlink*

# libnetfilter_conntrack 1.0.9
RUN mkdir -p /tmp/src && cd /tmp/src && \
    wget -q https://netfilter.org/projects/libnetfilter_conntrack/files/libnetfilter_conntrack-1.0.9.tar.bz2 && \
    tar xf libnetfilter_conntrack-1.0.9.tar.bz2 && cd libnetfilter_conntrack-1.0.9 && \
    ./configure --host=$HOST --prefix=$PREFIX \
        --enable-static --disable-shared --disable-dependency-tracking \
        PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig \
        PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig \
        CFLAGS="-I/usr/include -DNDEBUG" \
        LIBNFNETLINK_CFLAGS="-I$PREFIX/include" LIBNFNETLINK_LIBS="-L$PREFIX/lib -lnfnetlink" \
        LIBMNL_CFLAGS="-I$PREFIX/include" LIBMNL_LIBS="-L$PREFIX/lib -lmnl" && \
    make -j$(nproc) && make install && \
    rm -rf /tmp/src/libnetfilter*

# Verify static libs built
RUN ls -lh $PREFIX/lib/*.a

# ── Build mycoflowd ──────────────────────────────────────────────────────────
WORKDIR /build
COPY src/ /src/
COPY CMakeLists.txt /CMakeLists.txt

RUN cmake /src/.. \
        -DCMAKE_C_COMPILER=/usr/bin/aarch64-linux-gnu-gcc \
        -DCMAKE_EXE_LINKER_FLAGS="-static" \
        -DSTATIC_BUILD=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DHAVE_LIBNFCT_H:INTERNAL=1 \
        -DLIBNFCT_LIB:FILEPATH=/opt/nfct/lib/libnetfilter_conntrack.a \
        -DLIBNFNETLINK_LIB:FILEPATH=/opt/nfct/lib/libnfnetlink.a \
        -DLIBMNL_LIB:FILEPATH=/opt/nfct/lib/libmnl.a \
        "-DCMAKE_C_FLAGS=-I/opt/nfct/include -I/usr/include" \
    && make -j$(nproc) mycoflowd \
    && /usr/bin/aarch64-linux-gnu-strip src/mycoflowd \
    && ls -lh src/mycoflowd \
    && file src/mycoflowd \
    && echo "=== NFCT check ===" \
    && strings src/mycoflowd | grep -E "conntrack mark engine|libnetfilter_conntrack not present"
