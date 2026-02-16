Harika! Phase 1'i başarıyla tamamladık ve sağlam bir altyapımız var. Şimdi raporundaki 6 aylık yol haritasına  sadık kalarak, bir Yazılım Mühendisi titizliğiyle hazırlanmış kapsamlı bir **Master To-Do List** hazırladım.

Bu liste, projenin başından sonuna kadar (tez teslimine kadar) yapman gereken her şeyi teknik detaylarıyla kapsar.

---

# 🍄 MycoFlow: Master To-Do List

**Durum:** Phase 1 (Altyapı) Tamamlandı ✅ | Phase 2 Devam Ediyor 🚀
**Hedef Donanım:** Xiaomi AX3000T (MT7981B)
**Teknolojiler:** C, eBPF, OpenWrt, Lua, Shell Script

---

## 🟢 PHASE 1: Toolchain & Baseline (Hafta 1-4) [TAMAMLANDI]

*Bu aşama, geliştirdiğimiz Docker altyapısı ile büyük ölçüde tamamlandı.*

* [X] **Geliştirme Ortamı (Factory) Kurulumu**
* [X] Docker & VS Code Dev Container kurulumu.
* [X] OpenWrt SDK (MT7981B / Filogic) entegrasyonu.
* [X] Cross-Compiler (`aarch64-openwrt-linux-musl-gcc`) testi.
* [X] **Simülasyon Ortamı (Field) Kurulumu**
* [X] QEMU User Static ile ARM64 emülasyonu.
* [X] Docker Compose ile sanal OpenWrt router kurulumu.
* [X] "Hello World" ve basit Daemon testi.
* [ ] **Router Erişim ve Baseline (Donanım Gelince)**
* [ ] Router'a SSH erişimi ve `opkg update` testi.
* [ ] Varsayılan CAKE performansı ölçümü (`flent` rrul testi).
* [ ] Ham performans testi (`iperf3`).

---

## 🟡 PHASE 2: MycoFlow Daemon & Control Loop (Hafta 5-8)

*Şu an buradayız. Hedef: Akıllı olmayan ama çalışan bir kontrol döngüsü.*

* [X] **MycoFlowd Daemon İskeleti**
* [X] `ubus` entegrasyonu (OpenWrt'nin sistem veriyolu ile konuşma) — opsiyonel derleme.
* [X] `uci` konfigürasyon okuyucusu (Ayarları `/etc/config/mycoflow`'dan çekme) — temel okuma.
* [X] Minimal UCI şeması (enabled, egress_iface, sample_hz, max_cpu) ve doğrulama.
* [X] Döngü frekansı ve log seviyesi için konfigürasyonlu başlatma (varsayılanlarla çalışır).
* [X] Sıcak konfigürasyon yeniden yükleme (uci/ubus tetiklemeli, koşullara göre uygula).
* [X] Sinyal işleme (SIGTERM, SIGINT ile temiz kapanış).
* [X] Soğuk başlangıç kalibrasyonu: kısa probe penceresiyle idle RTT/jitter ölçümü ve persona önceliklerini başlatma.
* [X] **MycoAct (Eyleyici) Modülü**
* [X] `tc-cake` komutlarını C içinden çağıran wrapper fonksiyonları.
* [X] CAKE bant genişliği (bandwidth) parametresini dinamik değiştirme.
* [X] Hata yönetimi (TC komutu başarısız olursa ne olacak?).
* [X] Eylem zamanlayıcı: rate-limit (ρ ops/s) ve min cooldown (τ) ile adım büyüklüklerini sınırla.
* [X] **MycoSense (Algılayıcı) - v1 (Basit)**
* [X] `/proc/net/dev` veya `netlink` üzerinden basit paket sayacı okuma.

* [~] Ping (ICMP) probu ile anlık RTT ölçümü (socket yerine `ping` komutu ile).
* [~] Persona sinyalleri v0: basit RTT/jitter + akış simetrisi, sticky histeresis ve k-of-m uygulanıyor (DNS/SNI/size histogramı daha sonra).

* [X] Idle baseline: başlangıçta kısa kalibrasyon penceresiyle tin başına idle RTT/jitter referansı çıkar.
* [X] **Observability & Harness**
* [X] Yapılandırılabilir log seviyesi ve structured log formatı (timestamp + source + metric).
* [X] Opsiyonel metrik dökümü (dosya) ve dummy-metric besleyici test harness.
* [ ] Kaynak bütçesi koruması: CPU hedef <20% (peak 40%), RAM <64 MB; metrik tabloları için LRU sınırı.
* [X] **Refleksif Kontrol Döngüsü**
* [X] Histeresis algoritmasının C implementasyonu.
* [X] `k-of-m` oylama mantığı (Anlık sıçramaları filtreleme).
* [X] Rollback / safe-mode: aksiyon nedenleriyle snapshot logla; watchdog CPU veya metrik sıçramasında döngüyü dondurup son stabil konfige dön.

* [~] **OpenWrt Entegrasyonu (Kullanıcı Ucu)**
* [~] ubus yöntemlerini sürmek için hafif CLI veya LuCI stub (status/metrics/control çağrıları) — ubus yüzeyi hazır, CLI/LuCI yok.
* [~] ubus yüzeyi: myco.status, myco.persona (list/add/delete), myco.policy (get/set/boost/throttle) + rate-limit ve least-privilege ACL — temel yüzey var, rate-limit/ACL sonra.

* [ ] **Runtime & Paketleme (Erken)**
* [ ] init/service script ile daemon başlatma; router imajına `mycoflowd` ve varsayılan config kopyalama.
* [ ] Docker Compose router simülasyonunda healthcheck ve crash-loop backoff senaryosu.

---

## 🟠 PHASE 3: eBPF & Advanced Telemetry (Hafta 9-16)

*Projenin en teknik ve "havalı" kısmı. Kernel seviyesinde veri toplama.*

* [~] **eBPF Ortam Hazırlığı**

* [ ] Kernel config kontrolü (`kmod-ebpf` yüklü mü?).

* [~] `libbpf` kütüphanesinin SDK içine dahil edilmesi (scaffold başladı, obj staging eklendi).
* [~] **eBPF Kodu (Kernel Space - `.bpf.c`)**
* [~] `tc` (traffic control) hook noktasına takılacak eBPF programı (stub + tc attach scaffold).

* [ ] Paketlerin boyutlarını ve akış sürelerini map'lere kaydetme.
* [ ] RTT (Round Trip Time) ölçümü için TCP paketlerini izleme.

* [~] **eBPF Yükleyici (User Space)**
* [~] `mycoflowd` içinden eBPF programını kernele yükleme (Load & Attach) — yükleme + tc attach scaffold var.

* [ ] BPF Map'lerinden veriyi okuyup MycoSense modülüne aktarma.
* [ ] **Gelişmiş Metrikler**
* [ ] Jitter (Titreşim) hesaplama algoritması.
* [ ] Kuyruk doluluğu (Queue backlog) takibi.
* [ ] Flow tabloları için LRU/kapasite sınırı; eBPF map boyutlarını CPU/RAM bütçesine göre sabitle.

---

## 🔵 PHASE 4: UI & Persona Inference (Hafta 17-20)

*Kullanıcının sistemi göreceği ve "Persona" özelliklerinin ekleneceği faz.*

* [ ] **Web Arayüzü (LuCI App)**
* [ ] `luci-app-mycoflow` paket yapısının kurulması.
* [ ] Vue.js veya düz JS ile dashboard tasarımı.
* [ ] WebSocket ile canlı grafik çizimi (Gecikme, Bant Genişliği).
* [ ] Konfigürasyon sayfası (Aç/Kapa, Mod Seçimi).
* [ ] **Persona Çıkarımı (Heuristics)**
* [ ] DNS paketlerinden trafik türünü (Oyun, Video, İndirme) tahmin etme.
* [ ] Paket boyutu histogramına göre akış sınıflandırma.
* [ ] Önceliklendirme (Tin) mantığını persona'ya göre değiştirme.
* [ ] Persona sinyalleri: DNS/SNI kategori + paket boyutu histogramı + inter-arrival varyansı + akış simetrisi; yapışkan (sticky) histeresis ile sınıf geçişlerini yavaşlat.

---

## 🟣 PHASE 5: Test, Tuning & Benchmarking (Hafta 21-24)

*Sistemi zorlayıp ayarlarını yapacağımız aşama.*

* [ ] **Stres Testleri**
* [ ] Oyun Simülasyonu: Arka planda indirme varken ping testi.
* [ ] Kararlılık Testi: 24 saat boyunca sistemi açık bırakıp bellek sızıntısı (memory leak) kontrolü.
* [ ] Rollback Testi: Sistem çökerse kendini kurtarıyor mu?.
* [ ] **Parametre Tuning (Ayar)**
* [ ] Histeresis eşiklerinin () kalibrasyonu.
* [ ] Kontrol döngüsü frekansının (1Hz vs 2Hz) CPU kullanımına etkisi.
* [ ] **Raporlama Verisi**
* [ ] Statik CAKE vs. MycoFlow karşılaştırmalı grafiklerin oluşturulması.
* [ ] CPU ve RAM kullanım istatistiklerinin toplanması.
* [ ] Scriptli test profilleri (flent + iperf3, gaming/conferencing/mixed) 10+ dakikalık tekrarlarla; RTT/jitter/fairness ölçümü ve CSV/JSON çıktı.

---

## ⚪ PHASE 6: Paketleme & Tez Yazımı (Hafta 25-26)

*Ürünü teslim etme vakti.*

* [ ] **OpenWrt Paketi Oluşturma**
* [ ] `Makefile` dosyasının son hali (Package signature).
* [ ] `.ipk` dosyalarının oluşturulması ve bağımlılıkların tanımlanması.
* [ ] **Dokümantasyon**
* [ ] GitHub `README.md` (Kurulum, Kullanım, Lisans).
* [ ] Mimari şemaların son hallerinin çizilmesi.
* [ ] **Tez / Final Raporu**
* [ ] Deneysel sonuçların rapora eklenmesi.
* [ ] Gelecek çalışmalar bölümünün yazılması.
* [ ] Güvenlik/etik: ubus rate-limit ve imzalı paketler; payload muayenesi yok, varsayılan kapalı telemetri.

---

### 🔥 Acil (Bu Hafta Yapılacaklar)

1. **Phase 2** kapsamında, simülasyon ortamında (Docker) çalışan basit bir **MycoFlowd** yazmak.
2. Bu daemon'ın sahte verilerle (dummy metrics) karar alıp log basmasını sağlamak. (Zaten başladık!)
3. Sinyal yakalama + konfigürasyonlu döngü parametreleri (uci varsayılanları) ekleyip log seviyesini yönetmek.
