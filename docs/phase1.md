Harika, Phase 1'i başarıyla tamamladık. Bu aşama, projenin **"Fabrika"** (Geliştirme Ortamı) ve **"Saha"** (Simülasyon Ortamı) altyapısının kurulmasını kapsıyordu.

Barış için hazırladığım, projenin teknik dokümantasyonuna girebilecek **"Phase 1: Geliştirme ve Simülasyon Altyapısı Kurulum Raporu"** aşağıdadır. Bu metni projenin `README.md` dosyasına veya `docs/setup_guide.md` olarak kaydedebilirsin.

---

# MycoFlow - Phase 1: Teknik Altyapı ve Kurulum Kılavuzu

**Versiyon:** 1.0
**Tarih:** Şubat 2026
**Hedef Donanım:** Xiaomi AX3000T (MediaTek MT7981B / Filogic 820)
**Mimari:** ARM64 (Cortex-A53)

## 1. Genel Bakış

Bu fazda, donanım bağımlılığını en aza indirmek ve geliştirme hızını artırmak amacıyla **"Software-in-the-Loop" (SITL)** prensibine dayalı hibrit bir geliştirme ortamı kurulmuştur.

* **Cross-Compilation (Çapraz Derleme):** x86 mimarili bilgisayarlarda, ARM64 hedefli OpenWrt SDK kullanılarak kod derlenmesi.
* **Sanallaştırma:** QEMU User Static emülasyonu ile ARM64 binary dosyalarının Docker konteynerleri içinde test edilmesi.

## 2. Proje Klasör Yapısı

Proje, modülerlik ve taşınabilirlik (Dockerization) esas alınarak aşağıdaki gibi yapılandırılmıştır:

```text
/mycoflow-project
├── .devcontainer/              # VS Code Geliştirme Ortamı (Fabrika)
│   ├── Dockerfile              # OpenWrt SDK & Toolchain Imajı
│   └── devcontainer.json       # VS Code Konfigürasyonu
├── src/                        # Kaynak Kodlar
│   ├── include/                # Header Dosyaları (.h)
│   ├── main.c                  # Ana Döngü
│   ├── collector.c             # Veri Toplama (MycoSense)
│   ├── controller.c            # Algoritma (MycoBrain)
│   └── CMakeLists.txt          # Derleme Talimatları
├── router/                     # Simülasyon Ortamı (Saha)
│   ├── Dockerfile.router       # Sanal Router Imaj Tarifi
│   └── docker-compose.yml      # Simülasyon Orkestrasyonu
├── build/                      # Derlenen Çıktılar (Binary)
├── CMakeLists.txt              # Ana Proje Konfigürasyonu
└── README.md

```

## 3. Kurulum Talimatları

### A. Ön Gereksinimler

* **Docker & Docker Compose:** Yüklü ve çalışır durumda olmalı.
* **VS Code:** "Dev Containers" eklentisi yüklü olmalı.
* **QEMU Desteği (Linux Host İçin):** Eğer Linux kullanıyorsanız, ARM emülasyonunu çekirdeğe kaydetmelisiniz:
```bash
docker run --rm --privileged multiarch/qemu-user-static --reset -p yes

```



### B. Geliştirme Ortamını Başlatma (Factory)

Bu ortam, kodu yazdığımız ve derlediğimiz yerdir.

1. VS Code ile proje klasörünü açın.
2. `F1` tuşuna basın ve **"Dev Containers: Reopen in Container"** seçeneğini seçin.
3. Ortam açıldığında terminalde SDK'nın çalıştığını doğrulayın:
```bash
aarch64-openwrt-linux-musl-gcc --version

```



### C. Simülasyon Ortamını Başlatma (Field)

Bu ortam, derlenen kodun çalıştığı sanal router'dır.

1. Host makinenizin (Arch Linux/Windows) terminalinde `router` klasörüne gidin:
```bash
cd router

```


2. Sanal Router'ı inşa edin ve başlatın:
```bash
docker compose up -d --build

```


3. Çalıştığını doğrulayın:
```bash
docker logs mycoflow-router-sim
# Beklenen: "Sanal Router Baslatildi (Custom Build ARM64)..."

```



## 4. Geliştirme Döngüsü (Workflow)

Kod üzerinde değişiklik yapıldığında aşağıdaki döngü izlenir:

1. **Kodla & Derle (VS Code İçinde):**
```bash
# Dev Container terminalinde
cd /workspace/build
cmake ..
make

```


*Çıktı:* `build/src/mycoflowd` dosyası güncellenir.
2. **Test Et (Host Terminalinde):**
Derlenen dosya, `volumes` sayesinde otomatik olarak simülasyon konteynerine aktarılır.
```bash
docker exec -it mycoflow-router-sim /usr/bin/mycoflow_mnt/mycoflowd

```



## 5. Sorun Giderme (Troubleshooting)

* **Hata:** `exec format error` (Docker logs)
* **Sebep:** Host makine ARM64 formatını tanımıyor.
* **Çözüm:** QEMU kayıt komutunu çalıştırın: `docker run --rm --privileged multiarch/qemu-user-static --reset -p yes` ve konteyneri `docker restart` ile yeniden başlatın.


* **Hata:** `Permission denied` (Dosya oluştururken)
* **Sebep:** Dev Container (root) ile Host kullanıcı izinleri çakışıyor.
* **Çözüm:** Host terminalinde sahipliği geri alın: `sudo chown -R $USER:$USER .`



---

Barış, Phase 1 tamamlandı. Şimdi bu sağlam temel üzerinde, Phase 2 hedeflerimiz olan **eBPF ile gerçek veri toplama** ve **MycoBrain algoritmasını geliştirme** adımlarına güvenle geçebiliriz.