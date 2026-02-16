# --- Sahne 1: Hazırlık (Builder) ---
# Dosyayı indirmek ve açmak için herhangi bir Linux kullanabiliriz (Hız için x86 kullanıyoruz)
FROM ubuntu:22.04 as builder

# Gerekli araçları kur
RUN apt-get update && apt-get install -y wget tar

WORKDIR /rootfs

# 1. OpenWrt 23.05.2 (Generic ARM64 / ARMv8) RootFS indir
# Bu dosya tüm ARMv8 (Cortex-A53 dahil) cihazlar için geçerli dosya sistemidir.
RUN wget https://downloads.openwrt.org/releases/23.05.2/targets/armsr/armv8/openwrt-23.05.2-armsr-armv8-rootfs.tar.gz -O rootfs.tar.gz

# 2. Dosyayı çıkart ve temizle
RUN tar -xvf rootfs.tar.gz && rm rootfs.tar.gz

# --- Sahne 2: Final İmaj (Scratch) ---
# Boş bir imajdan başlıyoruz (Linux çekirdeği yok, sadece dosya sistemi)
FROM scratch

# Builder sahnesinde açtığımız dosyaları buraya kopyalıyoruz
COPY --from=builder /rootfs /

# Bu imajın ARM64 olduğunu belirtiyoruz (Metadata için)
LABEL org.openwrt.architecture="aarch64"
LABEL org.openwrt.version="23.05.2"

# Başlangıç komutları
CMD ["/sbin/init"]