# 🎭 MycoFlow — Öncelikli Sprint Backlog ve Ajan Atamaları

> **Orkestratör:** BMad (BMAD Orchestrator)
> **Temel:** [Proje Review Raporu](2026-02-16_project_review.md)
> **Oluşturma Tarihi:** 16 Şubat 2026
> **Kapsamlı Süre:** 16 Şubat — 15 Mayıs 2026 (~12 hafta)

---

## Öncelik Matrisi

| Öncelik | Açıklama | Sprint |
|---------|----------|--------|
| 🔴 P0 | Tez teslimi için **zorunlu** — Rapor-Kod uyumsuzluğunu kapatır | Sprint 1 (Şubat) |
| 🟠 P1 | Tez değerini **yükselten** — Benchmark ve eBPF | Sprint 2 (Mart) |
| 🟡 P2 | Tez sunumunu **güçlendiren** — UI ve paketleme | Sprint 3 (Nisan) |

---

## Epic & Story Özeti

| Epic | Story | Ajan | Öncelik | Sprint |
|------|-------|------|---------|--------|
| E1: Phase 2 Kapanış | S1.1 EWMA Filtre | 💻 James (Dev) | 🔴 P0 | 1 |
| E1: Phase 2 Kapanış | S1.2 Modüler Refactor | 🏗️ Winston → 💻 James | 🔴 P0 | 1 |
| E1: Phase 2 Kapanış | S1.3 Thread Safety | 💻 James (Dev) | 🔴 P0 | 1 |
| E1: Phase 2 Kapanış | S1.4 Unit Tests | 🧪 Quinn → 💻 James | 🔴 P0 | 1 |
| E2: Rapor Uyumu | S2.1 LaTeX Rapor Güncelle | 📊 Mary (Analyst) | 🔴 P0 | 1 |
| E2: Rapor Uyumu | S2.2 Persona Genişlet | 💻 James (Dev) | 🟠 P1 | 1-2 |
| E3: eBPF & Telemetri | S3.1 eBPF Map Read | 🏗️ Winston → 💻 James | 🟠 P1 | 2 |
| E3: eBPF & Telemetri | S3.2 Netlink Stats | 💻 James (Dev) | 🟠 P1 | 2 |
| E3: eBPF & Telemetri | S3.3 Flow Tablosu | 💻 James (Dev) | 🟡 P2 | 2 |
| E4: Test & Benchmark | S4.1 Test Harness | 🧪 Quinn (QA) | 🟠 P1 | 2 |
| E4: Test & Benchmark | S4.2 Benchmark Suite | 🧪 Quinn → 💻 James | 🟠 P1 | 2 |
| E4: Test & Benchmark | S4.3 Stres Test | 🧪 Quinn (QA) | 🟡 P2 | 2 |
| E5: UI & Paketleme | S5.1 LuCI Dashboard | 🎨 Sally → 💻 James | 🟡 P2 | 3 |
| E5: UI & Paketleme | S5.2 OpenWrt ipk | 💻 James (Dev) | 🟡 P2 | 3 |
| E5: UI & Paketleme | S5.3 Tez Yazımı | 📊 Mary → 📋 John | 🟡 P2 | 3 |

---

## Sprint 1: Phase 2 Kapanış & Kod-Rapor Uyumu (16 Şub → 8 Mar)

### 🔴 Epic 1 — Phase 2 Tamamlama

---

#### S1.1 — EWMA Filtre Uygulaması
| Alan | Detay |
|------|-------|
| **Ajan** | 💻 **James (Dev)** |
| **Öncelik** | 🔴 P0 |
| **Efor** | 3-4 saat |
| **Bağımlılık** | Yok |
| **Açıklama** | Rapordaki EWMA formülünü (`ê_t = α·e_t + (1-α)·ê_{t-1}`) koda ekle. `sense_sample()` çıktısına EWMA smoothing uygula. `control_decide()` fonksiyonunda raw delta yerine filtered error kullan. |
| **Kabul Kriterleri** | ① EWMA struct ve fonksiyonları var ② `α` parametresi UCI'dan okunuyor ③ Log çıktısında `ewma_rtt` ve `ewma_jitter` görünüyor ④ Unit test geçiyor |
| **Dosyalar** | `src/main.c` → yeni `ewma_update()` fonksiyonu |
| **Durum** | ⬜ Başlamadı |

---

#### S1.2 — Modüler Refactoring
| Alan | Detay |
|------|-------|
| **Ajan** | 🏗️ **Winston (Architect)** → sonra 💻 **James (Dev)** |
| **Öncelik** | 🔴 P0 |
| **Efor** | 4-6 saat |
| **Bağımlılık** | Yok (diğerleriyle paralel yapılabilir) |
| **Açıklama** | 1389 satırlık `main.c`'yi modüllere ayır. Winston mimariyi tasarlasın, James uygulasın. |
| **Hedef Yapı** | |

```
src/
├── main.c          (~150 satır — init + main loop)
├── config.c/h      (UCI + env + defaults)
├── sense.c/h       (metrik toplama + baseline)
├── persona.c/h     (persona heuristics + k-of-m)
├── control.c/h     (hysteresis + policy decision)
├── act.c/h         (CAKE tc wrapper)
├── ebpf.c/h        (eBPF load/attach/read)
├── ubus.c/h        (ubus API surface)
├── log.c/h         (structured logging)
└── CMakeLists.txt  (güncelleme)
```

| **Kabul Kriterleri** | ① Tüm modüller ayrı dosyalarda ② `cmake && make` başarılı ③ Mevcut davranış değişmedi ④ Docker sim'de çalışıyor |
| **Durum** | ⬜ Başlamadı |

---

#### S1.3 — Thread Safety Düzeltmesi
| Alan | Detay |
|------|-------|
| **Ajan** | 💻 **James (Dev)** |
| **Öncelik** | 🔴 P0 |
| **Efor** | 2-3 saat |
| **Bağımlılık** | S1.2 (refactor sonrası daha kolay) |
| **Açıklama** | ubus thread ile ana döngü arasındaki shared state'e mutex veya `stdatomic.h` ekle. `g_last_metrics`, `g_persona_override`, `g_ubus_control` erişimlerini koru. |
| **Kabul Kriterleri** | ① Mutex/atomic kullanılıyor ② ThreadSanitizer ile data race yok ③ ubus çağrıları thread-safe |
| **Durum** | ⬜ Başlamadı |

---

#### S1.4 — Unit Test Altyapısı
| Alan | Detay |
|------|-------|
| **Ajan** | 🧪 **Quinn (QA)** → test planı, 💻 **James (Dev)** → implementasyon |
| **Öncelik** | 🔴 P0 |
| **Efor** | 4-6 saat |
| **Bağımlılık** | S1.2 (modüler yapı test edilebilirliği artırır) |
| **Açıklama** | Minimal test framework (CMake + CTest veya basit assert-based) kur. Kritik fonksiyonlar için unit test yaz. |
| **Test Hedefleri** | |

| Fonksiyon | Test Kapsamı |
|-----------|-------------|
| `ewma_update()` | Farklı α değerleriyle yakınsama |
| `control_decide()` | Congested/clear/outlier senaryoları |
| `persona_update()` | k-of-m voting, sticky hysteresis |
| `is_outlier()` | Eşik aşımı algılama |
| `config_load()` | Varsayılan değerler, sınır doğrulama |

| **Kabul Kriterleri** | ① `ctest` komutuyla testler çalışıyor ② Minimum 5 test fonksiyonu ③ %80+ kontrol mantığı kapsama |
| **Durum** | ⬜ Başlamadı |

---

### 🔴 Epic 2 — Akademik Rapor Uyumu

---

#### S2.1 — LaTeX Rapor Güncelleme
| Alan | Detay |
|------|-------|
| **Ajan** | 📊 **Mary (Analyst)** |
| **Öncelik** | 🔴 P0 |
| **Efor** | 2-3 saat |
| **Bağımlılık** | S1.1 (EWMA uygulandıktan sonra) |
| **Açıklama** | `mycelium_report.tex`'i kodla uyumlu hale getir. k-of-m voting'i "pragmatic simplification" olarak sun. EWMA uygulamasını dokümante et. |
| **Kabul Kriterleri** | ① Rapordaki her formülün kodda karşılığı tutarlı ② Uygulama seçimleri gerekçelendirilmiş |
| **Durum** | ⬜ Başlamadı |

---

#### S2.2 — Persona Sinyallerini Genişlet
| Alan | Detay |
|------|-------|
| **Ajan** | 💻 **James (Dev)** |
| **Öncelik** | 🟠 P1 |
| **Efor** | 4-6 saat |
| **Bağımlılık** | S1.1, S1.2 |
| **Açıklama** | Mevcut basit persona çıkarımına (RTT/jitter/tx-rx) en az 1 yeni sinyal ekle: paket boyutu histogramı veya DNS kategorisi. |
| **Kabul Kriterleri** | ① En az 1 ek sinyal kaynağı entegre ② Persona doğruluğu ölçülebilir ③ Log'da sinyal detayları görünüyor |
| **Durum** | ⬜ Başlamadı |

---

## Sprint 2: eBPF & Benchmark (9 Mar → 5 Nis)

### 🟠 Epic 3 — eBPF & Gelişmiş Telemetri

---

#### S3.1 — eBPF Map Okuma
| Alan | Detay |
|------|-------|
| **Ajan** | 🏗️ **Winston (Architect)** → tasarım, 💻 **James (Dev)** → uygulama |
| **Öncelik** | 🟠 P1 |
| **Efor** | 6-8 saat |
| **Bağımlılık** | S1.2 |
| **Açıklama** | Mevcut eBPF scaffold üzerine BPF map okuma ekle. `mycoflow.bpf.c` yazılacak (paket sayacı + byte sayacı), user-space'den map iteration ile veri çekilecek. |
| **Kabul Kriterleri** | ① `mycoflow.bpf.c` derleniyor ② Map'ten flow istatistikleri okunuyor ③ Docker sim'de çalışıyor |
| **Durum** | ⬜ Başlamadı |

---

#### S3.2 — Netlink İstatistikleri
| Alan | Detay |
|------|-------|
| **Ajan** | 💻 **James (Dev)** |
| **Öncelik** | 🟠 P1 |
| **Efor** | 3-4 saat |
| **Bağımlılık** | S1.2 |
| **Açıklama** | `/proc/net/dev` yerine Netlink socket ile kuyruk derinliği (backlog), düşen paket ve queue istatistiklerini oku. |
| **Kabul Kriterleri** | ① Backlog/drops metrikleri toplanıyor ② Log'da tin-bazlı istatistikler var |
| **Durum** | ⬜ Başlamadı |

---

#### S3.3 — Flow Tablosu ve LRU
| Alan | Detay |
|------|-------|
| **Ajan** | 💻 **James (Dev)** |
| **Öncelik** | 🟡 P2 |
| **Efor** | 3-4 saat |
| **Bağımlılık** | S3.1 |
| **Açıklama** | eBPF map'lerinden okunan flow'lar için user-space LRU tablosu oluştur. Kapasite sınırı (RAM bütçesi) uygula. |
| **Kabul Kriterleri** | ① LRU eviction çalışıyor ② RAM kullanımı <64 MB sınırında |
| **Durum** | ⬜ Başlamadı |

---

### 🟠 Epic 4 — Test & Benchmark

---

#### S4.1 — Entegrasyon Test Harness
| Alan | Detay |
|------|-------|
| **Ajan** | 🧪 **Quinn (QA)** |
| **Öncelik** | 🟠 P1 |
| **Efor** | 3-4 saat |
| **Bağımlılık** | S1.4 |
| **Açıklama** | Docker simülasyonunda mycoflowd'yi scriptli senaryolarla test eden shell harness yaz. |
| **Kabul Kriterleri** | ① `./run_tests.sh` komutuyla otomatik test ② Gaming/conferencing/mixed profiller test edildi ③ Sonuçlar JSON olarak kaydedildi |
| **Durum** | ⬜ Başlamadı |

---

#### S4.2 — Benchmark Suite
| Alan | Detay |
|------|-------|
| **Ajan** | 🧪 **Quinn (QA)** → plan, 💻 **James (Dev)** → implementasyon |
| **Öncelik** | 🟠 P1 |
| **Efor** | 6-8 saat |
| **Bağımlılık** | S4.1 + Router donanımı hazır olmalı |
| **Açıklama** | `flent` ve `iperf3` ile scriptli benchmark. Statik CAKE vs MycoFlow karşılaştırması. Her test ≥10 dakika, ≥5 tekrar. |
| **Baselines** | FIFO, SQM/CAKE static, MycoFlow adaptive |
| **Kabul Kriterleri** | ① Otomatik benchmark script ② CSV/JSON çıktılar ③ RTT/jitter/fairness karşılaştırma tabloları |
| **Durum** | ⬜ Başlamadı |

---

#### S4.3 — Stres/Kararlılık Testi
| Alan | Detay |
|------|-------|
| **Ajan** | 🧪 **Quinn (QA)** |
| **Öncelik** | 🟡 P2 |
| **Efor** | 2-3 saat |
| **Bağımlılık** | S4.1 |
| **Açıklama** | 24 saat çalışma testi, memory leak kontrolü (Valgrind/ASan), rollback stres testi. |
| **Kabul Kriterleri** | ① 24h çalışma sonrası crash yok ② Bellek sızıntısı yok ③ Safe-mode tetikleme ve kurtarma başarılı |
| **Durum** | ⬜ Başlamadı |

---

## Sprint 3: UI & Tez Teslimatı (6 Nis → 10 May)

### 🟡 Epic 5 — UI, Paketleme & Tez

---

#### S5.1 — LuCI Dashboard
| Alan | Detay |
|------|-------|
| **Ajan** | 🎨 **Sally (UX)** → wireframe, 💻 **James (Dev)** → uygulama |
| **Öncelik** | 🟡 P2 |
| **Efor** | 8-12 saat |
| **Bağımlılık** | S1.3 (ubus API hazır) |
| **Açıklama** | `luci-app-mycoflow` paketi. ubus `status` çağrısını görselleyen tek sayfa. |
| **Kabul Kriterleri** | ① LuCI'da menü girişi var ② Canlı metrikler güncelleniyor ③ Manuel override çalışıyor |
| **Durum** | ⬜ Başlamadı |

---

#### S5.2 — OpenWrt Paketleme (.ipk)
| Alan | Detay |
|------|-------|
| **Ajan** | 💻 **James (Dev)** |
| **Öncelik** | 🟡 P2 |
| **Efor** | 3-4 saat |
| **Bağımlılık** | S1.2, S5.1 |
| **Açıklama** | OpenWrt Makefile, init script, UCI defaults, `.ipk` oluşturma. |
| **Kabul Kriterleri** | ① `opkg install mycoflow_*.ipk` ile kurulabiliyor ② Service enable/start çalışıyor ③ Loglar görünüyor |
| **Durum** | ⬜ Başlamadı |

---

#### S5.3 — Tez Yazımı ve Sonuçlar
| Alan | Detay |
|------|-------|
| **Ajan** | 📊 **Mary (Analyst)** → veri analizi, 📋 **John (PM)** → yapı + timeline |
| **Öncelik** | 🟡 P2 |
| **Efor** | 10-15 saat |
| **Bağımlılık** | S4.2 (benchmark sonuçları) |
| **Açıklama** | Benchmark verilerinin istatistiksel analizi (t-test/ANOVA). Grafik üretimi. LaTeX raporuna sonuç bölümü ekleme. Sunum slaytları. |
| **Kabul Kriterleri** | ① Karşılaştırmalı grafikler hazır ② İstatistiksel anlamlılık gösterilmiş ③ Final PDF Raporu tamamlanmış |
| **Durum** | ⬜ Başlamadı |

---

## Ajan Yük Dağılımı Özeti

| Ajan | Rol | Atanan Story'ler | Toplam Efor |
|------|-----|------------------|-------------|
| 💻 **James (Dev)** | Full Stack Developer | S1.1, S1.2, S1.3, S1.4, S2.2, S3.1, S3.2, S3.3, S4.2, S5.1, S5.2 | ~45-60 saat |
| 🏗️ **Winston (Architect)** | Solution Architect | S1.2, S3.1 | ~4-6 saat |
| 🧪 **Quinn (QA)** | QA Test Architect | S1.4, S4.1, S4.2, S4.3 | ~12-16 saat |
| 📊 **Mary (Analyst)** | Business Analyst | S2.1, S5.3 | ~6-8 saat |
| 🎨 **Sally (UX)** | UX Expert | S5.1 | ~3-4 saat |
| 📋 **John (PM)** | Product Manager | S5.3 | ~3-4 saat |

---

## Doğrulama Planı

### Otomatik Testler
- `cd build && cmake .. && make` — Derleme doğrulaması
- `ctest --test-dir build` — Unit testler (S1.4 sonrası)
- `./scripts/run_tests.sh` — Entegrasyon testleri (S4.1 sonrası)
- `./scripts/benchmark.sh` — Benchmark suite (S4.2 sonrası)

### Manuel Doğrulama
- Docker sim'de `mycoflowd` çalıştırıp log'ları izleme
- Router donanımında (hazır olduğunda) SSH ile deployment testi
- LuCI arayüzünü tarayıcıdan görsel kontrol (S5.1 sonrası)

---

*Bu backlog, Mary'nin proje review raporuna (2026-02-16) dayanarak oluşturulmuştur.*
