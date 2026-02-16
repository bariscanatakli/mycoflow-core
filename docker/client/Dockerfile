FROM alpine:3.19

RUN apk add --no-cache \
    iperf3 \
    curl \
    bind-tools \
    iputils \
    bash \
    wget

# Keep container running
CMD ["sleep", "infinity"]
