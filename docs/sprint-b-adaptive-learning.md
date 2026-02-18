# Sprint B — Gerçek Adaptasyon: Misel Gibi Öğrenen MycoFlow

> **Tarih:** 18 Şubat 2026
> **Temel:** [Flash Safety Analizi](2026-02-16_project_review.md) — tüm öğrenme RAM'de, flash yazma sıfır
> **Hedef:** Reaktif controller'dan gerçek adaptif sisteme geçiş

---

## Neden Bu Sprint

Mevcut sistem **reaktif** — anlık ölçüme sabit kurallarla tepki veriyor.
Bu sprint sonunda sistem **adaptif** olacak — geçmiş deneyimden öğrenerek kendi parametrelerini ayarlayacak.

### Flash/RAM/CPU Kanıtı (tez sunumu için)

| Kaynak | Mevcut | Sprint B eklentisi | Router limiti |
|--------|--------|-------------------|---------------|
| Flash yazma | 0 | **0** | ~100k cycle/block |
| RAM ek yükü | 12.3 KB | **+336 byte** | 256 MB |
| CPU ek yükü | ~0 | **%0.000006** | 20% hedef |

Tüm öğrenme RAM'de yaşar. Reboot'ta sıfırlanır — kasıtlı tasarım.

---

## Hikayeler

### S6.0 — `tc qdisc replace` → `tc qdisc change` (Kritik Düzeltme)

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_act.c:29` |
| **Sorun** | `replace` her seferinde qdisc'i sıfırlar — kuyruk boşalır, CAKE iç durumu kaybolur |
| **Çözüm** | İlk kurulumda `replace`, sonrasında `change` |
| **Kabul** | `tc qdisc show dev <iface>` ile aynı qdisc handle'ı korunuyor |
| **Efor** | 30 dakika |

---

### S6.1 — Kayan Baseline (Sliding Baseline)

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_sense.c`, `src/myco_types.h` |
| **Sorun** | Baseline boot'ta bir kez alınıyor, gece/gündüz farkı, ISP değişimi yok sayılıyor |
| **Mekanizma** | Her `BASELINE_UPDATE_INTERVAL` (60) döngüde: `baseline = 0.99 * baseline + 0.01 * metrics` |
| **Yeni alan** | `myco_config_t`'ye `baseline_decay` (default: 0.01) |
| **Kabul** | Log'da `baseline_rtt` zamanla kayıyor görünmeli |
| **Efor** | 1-2 saat |

---

### S6.2 — Adaptif Congestion Eşiği

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_control.c` |
| **Sorun** | Eşikler hardcode: `rtt_delta > 20ms`, `jitter_delta > 10ms` |
| **Mekanizma** | `threshold_rtt = max(8.0, baseline.rtt_ms * 0.30)` |
| **Ek sinyal** | `qdisc_backlog > 0` → congestion kanıtı (RTT probe'dan daha hızlı) |
| **Sonuç** | Fiber hat (5ms RTT): eşik ~1.5ms; ADSL (40ms RTT): eşik ~12ms |
| **Kabul** | UCI'dan `rtt_margin_factor` (default: 0.30) override edilebilmeli |
| **Efor** | 2-3 saat |

---

### S6.3 — Aksiyon Etkisi Ölçümü (Action Feedback Ring)

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_control.c`, `src/myco_types.h` |
| **Sorun** | Bandwidth düşürülüyor ama RTT gerçekten düştü mü? Sistem bilmiyor |
| **Mekanizma** | 8 elemanlı halka tampon: `{ts, bw_before, bw_after, rtt_before}`, 3 döngü sonra `rtt_after` doldurulur |
| **Adaptasyon** | Son 4 aksiyonun %50'si etkisizse → `bandwidth_step` yarıya indirilir |
| **RAM** | 8 × 28 byte = 224 byte |
| **Kabul** | Log'da `step_adapted: 2000→1000 kbit` mesajı görünmeli |
| **Efor** | 3-4 saat |

---

### S6.4 — Flow Table → Persona Sinyali

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_persona.c`, `src/myco_flow.h` |
| **Sorun** | `flow_table` toplanıyor, `persona_update()`'e hiç girmiyor |
| **Yeni sinyal 1** | `active_flows < 5` → interactive (gaming: az bağlantı, yüksek paket hızı) |
| **Yeni sinyal 2** | Elephant flow: tek bir flow toplam byte'ın %60'ını taşıyorsa → BULK |
| **Yeni sinyal 3** | `ebpf_rx_pkts` delta hızı yüksekse (>1000 pkt/s) → interactive (UDP flood = gaming/VoIP) |
| **Kabul** | Persona testi: `iperf3 -P 1` (elephant) → BULK, `ping flood` → INTERACTIVE |
| **Efor** | 3-4 saat |

---

### S6.5 — Çoklu-Ping Jitter & Loss

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_sense.c`, `src/myco_types.h` |
| **Sorun** | Jitter = `fabs(rtt_now - rtt_prev)` — tek paket, kaba delta |
| **Mekanizma** | `ping -c 3 -W 1` ile 3 paket: gerçek jitter = stddev(rtt[0..2]) |
| **Yeni alan** | `metrics_t.probe_loss_pct` (float, 0.0–100.0) |
| **CAKE etkisi** | `probe_loss_pct > 2.0` → congestion sinyali (kuyruk dolu, paket düşüyor) |
| **Efor** | 2-3 saat |

---

### S6.6 — CAKE Tin Yönetimi (Persona → Diffserv)

| Alan | Detay |
|------|-------|
| **Dosya** | `src/myco_act.c` |
| **Sorun** | Persona bilgisi sadece bandwidth adımını etkiliyor; CAKE tin'leri hiç yönetilmiyor |
| **Mekanizma** | `tc qdisc change dev <iface> root cake bandwidth <N>kbit diffserv4` |
| **INTERACTIVE** | `tc filter add ... action skbedit priority 0` (VOICE tin) |
| **BULK** | `tc filter add ... action skbedit priority 3` (BULK tin) |
| **Kabul** | `tc -s qdisc show` ile tin istatistikleri farklı dağılım göstermeli |
| **Efor** | 4-5 saat |

---

## Hikaye Bağımlılıkları

```
S6.0 (tc fix) ──────────────────────────────── S6.6 (tin mgmt)
                                                      ↑
S6.1 (sliding baseline) ── S6.2 (adaptif eşik) ──────┤
                                    ↑                  │
S6.3 (aksiyon feedback) ───────────┘                  │
                                                       │
S6.4 (flow → persona) ─────────────────────────────────┘
S6.5 (çoklu-ping) ─────── S6.2 (loss sinyali olarak)
```

**Minimum viable set (bufferbloat için yeterli):** S6.0 + S6.1 + S6.2 + S6.4
**Tam misel adaptasyonu:** + S6.3 + S6.5 + S6.6

---

## Tez İçin Değer

| Katkı | Neden Güçlü |
|-------|-------------|
| Kayan baseline | "Sistem ortamı öğreniyor" — statik sistemle temel fark |
| Adaptif eşik | Hat tipine göre otokalibre: fiber vs ADSL |
| Aksiyon feedback | Kapalı-döngü öğrenme kanıtı |
| Flash = 0 yazma | "Constrained hardware uyumlu, chip ömrünü tüketmez" |
| Persona + flow | 5 sinyal: RTT, jitter, tx/rx, paket boyutu, flow sayısı, elephant flow, eBPF hızı |

---

*Sprint B, mevcut tüm test altyapısı (4/4 unit test, Docker integration) üzerine inşa eder.*
*Yeni unit testler: `test_control.c`'ye adaptif eşik ve aksiyon feedback senaryoları eklenecek.*
