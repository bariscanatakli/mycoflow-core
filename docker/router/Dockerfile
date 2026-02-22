# Stage 1: Build mycoflowd statically
FROM ubuntu:22.04 AS builder

RUN apt-get update -qq && apt-get install -y -qq \
    cmake gcc make pkg-config \
    zlib1g-dev libelf-dev linux-libc-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY src/ /src/
COPY CMakeLists.txt /CMakeLists.txt

RUN cmake /src/.. \
      -DSTATIC_BUILD=ON \
      -DCMAKE_BUILD_TYPE=Release \
      2>&1 && \
    make -j$(nproc) mycoflowd && \
    strip src/mycoflowd

# Stage 2: Router runtime — Debian slim + network tools
FROM debian:bookworm-slim

RUN apt-get update -qq && apt-get install -y -qq \
    iproute2 \
    iptables \
    iputils-ping \
    iperf3 \
    kmod \
    procps \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/src/mycoflowd /usr/sbin/mycoflowd
RUN chmod +x /usr/sbin/mycoflowd

COPY docker/router/init.sh /init.sh
RUN chmod +x /init.sh

CMD ["/init.sh"]
