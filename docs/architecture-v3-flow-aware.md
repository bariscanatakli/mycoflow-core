# MycoFlow v3 — Flow-Aware Architecture

**Status:** Onaylanmış tasarım — implementasyon Faz 3a'dan başlar.
**Tarih:** 2026-04-19
**Doğrulayan:** İkinci agent review'ı (ağ uzmanı + yazılım mimarı perspektifi) — tüm 10 karar onaylandı.

## 1. Hedef (tez diliyle)

> Kullanıcıdan hiçbir statik atama istemeden, her LAN cihazının o an aktif servislerini anlık tespit et; kullanıcının o cihaz için verdiği *tercih profiline* göre hangi servisi koruman gerektiğine karar ver; CAKE diffserv tin'lerine uygun DSCP işaretini akış başına uygula.

Kritik kısıtlar (taviz yok):
- **Dinamizm**: kullanıcı "bu cihaz gaming" demiyor, "bu cihazda gaming varsa koru" diyor. Persona hâlâ otomatik tespit.
- **Doğruluk**: aynı cihazdaki YouTube + CS2 + SSH aynı DSCP alamaz.
- **Flash safety**: state hâlâ RAM-only.
- **Kaynak**: 256 MB RAM, 2× A53. Kural sayısı O(servis) olmalı, O(akış) değil.

## 2. 4-Layer Model

```
┌─────────────────────────────────────────────────────────────┐
│ L4 — CAKE diffserv4  (kernel qdisc, egress pppoe-wan)       │
│      Voice / Video / BestEffort / Bulk tins                 │
└────────────────────────────▲────────────────────────────────┘
                             │ DSCP bits
┌────────────────────────────┴────────────────────────────────┐
│ L3 — DSCP Applier  (iptables mangle + CONNMARK, kernel)     │
│      ct mark → DSCP  (≤12 fixed mangle rules)               │
│      daemon updates ct marks via libnetfilter_conntrack     │
└────────────────────────────▲────────────────────────────────┘
                             │ ct mark value
┌────────────────────────────┴────────────────────────────────┐
│ L2 — Service→Mark Resolver  (daemon, userspace)             │
│      service_t + device_priority_profile → mark             │
│      profile = user's "I care about gaming on this device"  │
└────────────────────────────▲────────────────────────────────┘
                             │ service_t per flow
┌────────────────────────────┴────────────────────────────────┐
│ L1 — Flow Service Detector  (daemon, userspace)             │
│      port hint + DNS cache + behavior → service_t           │
│      runs per flow, not per device                          │
└─────────────────────────────────────────────────────────────┘
```

Device persona (`device->persona`) artık **türetilmiş bir rapor** — DSCP kararının kaynağı değil. Tez için LuCI'de gösterilir.

## 3. Service Taksonomisi

Mevcut 6 persona (VOIP/GAMING/VIDEO/STREAMING/BULK/TORRENT) **cihaz rolü** olarak kalır. Flow seviyesinde daha ince granülerite:

| `service_t` | Örnek | Default tin | Tespit |
|---|---|---|---|
| `SVC_GAME_RT` | CS2, Valorant, LoL in-game | Voice | port+DNS (riotgames/valvesoftware) |
| `SVC_GAME_LAUNCHER` | Steam indir, Epic | Bulk | DNS (steamcontent, epicgames-download) |
| `SVC_VOIP_CALL` | Discord voice, WhatsApp | Voice | port+DNS (discord.media, whatsapp) |
| `SVC_VIDEO_CONF` | Zoom, Meet, Teams | Video | DNS (zoom.us, meet.google.com) |
| `SVC_VIDEO_LIVE` | Twitch, YouTube Live | Video | DNS (twitch/youtube.com live) |
| `SVC_VIDEO_VOD` | YouTube, Netflix, Disney+ | **BestEffort** | DNS (*.googlevideo, *.nflxvideo) |
| `SVC_WEB_INTERACTIVE` | Web browsing, API calls | BestEffort | default 443/80 + small flows |
| `SVC_BULK_DL` | apt update, GitHub clone | Bulk | behavior (high rx, elephant) |
| `SVC_FILE_SYNC` | Dropbox, iCloud, OneDrive | Bulk | DNS (dropbox, icloud-content) |
| `SVC_TORRENT` | BitTorrent | Bulk | behavior (>100 flows no elephant) |
| `SVC_SYSTEM` | DNS, NTP, DHCP | BestEffort | port (53/123/67) |
| `SVC_UNKNOWN` | — | BestEffort | fallback |

**Onaylanmış karar 1**: VOD → BestEffort. YouTube'un 30s buffer'ı var; Video tin'i real-time conf/live için boş tutulur. Bufferbloat'un altın kuralı: "sadece gerçekten gecikmeye duyarlı olanlar öncelik alır".

**Onaylanmış karar 2**: `SVC_WEB_INTERACTIVE` default BestEffort. Kullanıcı `boost_interactive=1` ile cihaz başına Video tin'e yükseltebilir.

## 4. Device Priority Profile

Cihaz başına kullanıcı tercihi. **Servis ataması değil, öncelik sırası.**

```
# /etc/config/mycoflow
config device
    option mac 'aa:bb:cc:dd:ee:ff'
    option label 'barış-pc'
    option profile 'gaming'
    # list boost 'SVC_VOIP_CALL'   # opsiyonel override
```

### Profil tanımları (global, default 4 profil)

```
config profile 'gaming'
    option winner_priority 'SVC_GAME_RT,SVC_VOIP_CALL,SVC_VIDEO_CONF'
    list service_dscp 'SVC_GAME_RT=EF'
    list service_dscp 'SVC_VOIP_CALL=CS4'
    list service_dscp 'SVC_VIDEO_CONF=CS3'
    list service_dscp 'SVC_VIDEO_VOD=0'
    list service_dscp 'SVC_BULK_DL=CS1'

config profile 'remote_work'
    option winner_priority 'SVC_VIDEO_CONF,SVC_VOIP_CALL,SVC_GAME_RT'
    list service_dscp 'SVC_VIDEO_CONF=EF'
    list service_dscp 'SVC_VOIP_CALL=CS4'
    list service_dscp 'SVC_GAME_RT=CS3'

config profile 'family_media'
    option winner_priority 'SVC_VIDEO_VOD,SVC_VIDEO_LIVE,SVC_GAME_RT'
    list service_dscp 'SVC_VIDEO_VOD=CS3'
    list service_dscp 'SVC_GAME_RT=CS4'

config profile 'auto'
    option winner_priority 'SVC_VOIP_CALL,SVC_GAME_RT,SVC_VIDEO_CONF,SVC_VIDEO_LIVE,SVC_VIDEO_VOD,SVC_WEB_INTERACTIVE,SVC_BULK_DL'
```

**Onaylanmış karar**: `auto` default priority sırası VOIP > GAME_RT > VIDEO_CONF > VIDEO_LIVE > VIDEO_VOD > WEB_INTERACTIVE > BULK — ağ dünyasındaki ideal sıralama.

### Device persona derivation

```
active_services = {svc | any flow in last 5s has this service}
for svc in profile.winner_priority:
    if svc in active_services:
        device.persona = service_to_persona(svc)
        break
else:
    device.persona = PERSONA_UNKNOWN
```

**Kritik davranış**: `profile=gaming` cihazda Steam + CS2 aynı anda → device.persona = GAMING (rapor). Ama DSCP marking per-flow: Steam = CS1 (Bulk), CS2 = EF (Voice). İkisi ayrı tin'e gider.

## 5. Flow Service Detector (L1)

### Girdi
Her aktif flow (5-tuple from conntrack):
- src_ip, src_port, dst_ip, dst_port, proto
- bytes_fwd, bytes_rev
- packets_fwd, packets_rev
- age_sec, last_seen

### 3 sinyal kaynağı (ağırlıklı oy)

**(a) DNS hint (güçlü sinyal, ağırlık 0.6)**
- DNS cache genişlet (15 → 60+ domain): Gaming (steam/riot/ea/activision/battle.net/xbox/playstation/epic/ubi/roblox), VOD (googlevideo/nflxvideo/dssedge/hbomax/primevideo/hulu), Live (twitch/tiktok), Conf (zoom/meet/teams/whereby/webex), VoIP (discord.media/whatsapp/telegram/signal), Sync (dropbox/icloud/onedrive/drive.google), Bulk (github/docker/pypi/npm/ubuntu/debian).

**(b) Port hint (orta sinyal, ağırlık 0.3)**
- `myco_hint.c` mevcut; `port_to_service()` eklenir.

**(c) Behavior (zayıf sinyal, ağırlık 0.1)**
- avg_pkt_size < 200B + bidir UDP → GAME_RT/VOIP
- rx_bps > 5Mbps + elephant + TCP → BULK_DL
- rx_bps > 1Mbps + udp_ratio > 0.8 → VIDEO_VOD (QUIC)
- flows>100 + !elephant → TORRENT (device-level)

### Karar

```
score[svc] = 0.6 * (dns_hint == svc) + 0.3 * (port_hint == svc) + 0.1 * (behavior == svc)
if max(score) >= 0.3: return argmax(score)
return SVC_UNKNOWN
```

**Onaylanmış karar — DoH/DoT**: DNS şifrelenirse port+behavior'a düşer; LuCI'de "DNS encrypted — classification degraded" banner. DPI denenmez (CPU bütçesi yok).

## 6. Kernel DSCP Application (L3)

**Onaylanmış karar**: CONNMARK-based, not per-flow iptables.

### Kural yapısı (daemon boot)

```
iptables -t mangle -N mycoflow_dscp
iptables -t mangle -A mycoflow_dscp -m connmark --mark 1 -j DSCP --set-dscp-class EF
iptables -t mangle -A mycoflow_dscp -m connmark --mark 2 -j DSCP --set-dscp-class CS4
# ... 12 mark total
iptables -t mangle -A FORWARD -j mycoflow_dscp
iptables -t mangle -A OUTPUT  -j mycoflow_dscp
```

### Flow → mark (daemon runtime)

libnetfilter_conntrack ile kernel'e yaz (fork yok):
```c
nfct_set_attr_u32(ct, ATTR_MARK, mark);
nfct_query(h, NFCT_Q_UPDATE, ct);
```

### Mark değerleri

```
MARK_UNKNOWN         = 0    (default, no DSCP change)
MARK_GAME_RT         = 1    → EF
MARK_VOIP_CALL       = 2    → CS4
MARK_VIDEO_CONF      = 3    → CS3
MARK_VIDEO_LIVE      = 4    → CS3
MARK_VIDEO_VOD       = 5    → 0  (profile-dependent)
MARK_WEB_INTERACTIVE = 6    → 0
MARK_BULK_DL         = 7    → CS1
MARK_FILE_SYNC       = 8    → CS1
MARK_TORRENT         = 9    → CS1
MARK_GAME_LAUNCHER   = 10   → CS1
MARK_SYSTEM          = 11   → 0
```

**Onaylanmış karar — Profile switch**: kompakt mark havuzu + profile değişince mangle rebuild. Profile switch nadir olay, rebuild ucuz. fwmark havuzu patlamaz (routing/firewall mark'ları ile çakışmaz).

## 7. Daemon State Model

```c
typedef struct flow_service {
    uint32_t  src_ip, dst_ip;
    uint16_t  src_port, dst_port;
    uint8_t   proto;
    service_t service;
    uint8_t   ct_mark;
    double    detected_at;
    double    last_confirmed;
    uint8_t   service_stable;   // 2/3 window
} flow_service_t;

typedef struct flow_service_table {
    flow_service_t entries[MAX_FLOWS];   // 1024
    int count;
    // LRU eviction
} flow_service_table_t;

// Device:
typedef struct device_entry {
    ...
    uint8_t   priority_profile;
    uint32_t  active_services_bitmap;
    persona_t persona;
} device_entry_t;
```

**Onaylanmış karar — MAX_FLOWS=1024 + LRU**: torrent saniyede binlerce flow kurabilir; LRU eviction crash-proof.

**RAM bütçesi**: 1024 × 40 = ~40KB + 32 device × 8 = 256B → **~40KB** toplam.

## 8. Döngü Akışı (her 500ms)

```
1. Conntrack snapshot al           (mevcut, myco_flow.c)
2. Per-flow: service detect        (YENİ — 3-sinyal)
3. Yeni/değişen service'ler için   (YENİ)
     nfct_query(UPDATE, mark=N)
4. Per-device aggregate            (mevcut)
5. Device persona = winner_priority(active_services, profile)   (DEĞİŞTİ)
6. CAKE bw adapt                   (mevcut)
7. State dump (/tmp)               (mevcut)
```

## 9. UI Model (LuCI)

### Device list

| IP | MAC | Hostname | Active Services | Persona | Profile | Edit |
|---|---|---|---|---|---|---|
| 10.10.1.172 | aa:bb | barış-pc | 🎮 GAME_RT, 📺 VOD, 📦 BULK | GAMING | gaming | ✏️ |

### Device edit modal

```
┌─── 10.10.1.172 (barış-pc) ──────┐
│ Label: [barış-pc____________]    │
│ Priority Profile:  [gaming  ▼]   │
│   auto / gaming / remote_work /  │
│   family_media / custom          │
│                                  │
│ Winner Priority Order:           │
│   [dropdown list 1: SVC_GAME_RT] │
│   [dropdown list 2: SVC_VOIP]    │
│   [dropdown list 3: SVC_CONF]    │
│   ...                            │
│                                  │
│ [Save] [Cancel]                  │
└──────────────────────────────────┘
```

**Onaylanmış karar — UX**: Phase 1'de select dropdown (stabil), drag-drop Phase 6'ya ertelendi. LuCI JS framework testi sonra.

### OUI hint

Yeni cihaz bağlandığında MAC OUI → üretici tahmini → **öneri** (banner), zorla atama yok. Apple/Android random MAC yaygın — hard assignment zararlı olur.

## 10. Kenar Durumlar

| Senaryo | Davranış |
|---|---|
| DNS cache miss (ilk 5s) | Port+behavior fallback; DNS gelirse mark update |
| Daemon restart | CT mark kernel'de kalır; daemon first cycle'da re-classify |
| Profile change during session | Mangle rebuild; mark değişmez, map değişir → 500ms sonra etkili |
| Flow 5-tuple çakışması (NAT) | conntrack zone=0 yeterli |
| IPv6 | v1.0 kapsam dışı, Phase 3.5'e ertelendi |
| DoH/DoT | Banner + graceful degradation |
| `nfct_query` unavailable | OpenWrt `libnetfilter-conntrack` paketi; boot check |
| Safe-mode: outlier | DSCP apply suspend (mevcut) |
| MAX_FLOWS aşımı | LRU eviction (last_seen en eski) |

**Onaylanmış karar — IPv6 deferred**: scope creep engelleme. CAKE fiziksel iface üzerinde zaten IPv6 trafiğine çalışıyor (marking olmadan da).

## 11. Migration Map

| Dosya | Değişim |
|---|---|
| `myco_persona.c` | Aynen kalır; artık device.persona'nın kaynağı değil |
| `myco_device.c/h` | `hint_votes` → `active_services` bitmap; profile-driven persona |
| `myco_hint.c` | `port_to_service()` eklenir |
| `myco_dns.c` | Catalog genişler (15→60+); `domain_to_service()` |
| `myco_act.c` | CONNMARK mangle; profile-driven rebuild |
| `myco_config.c/h` | `config device` + `config profile` parse |
| `myco_flow.c` | `flow_service_table_t` |
| **yeni** `myco_service.c/h` | service_t + 3-signal detection |
| **yeni** `myco_profile.c/h` | Profile table + winner_priority resolver |
| `luci-app-mycoflow` | Device edit + service bars |

Mevcut 19 persona + 4 device + 7 diğer test kırılmamalı. `test_service`, `test_profile`, `test_ctmark` eklenecek.

## 12. Test Stratejisi

### Unit
- `test_service`: her service_t × 3-sinyal matrix
- `test_profile`: winner_priority algoritması, tie-break, custom
- `test_ctmark`: mark→DSCP map, profile switch rebuild

### Integration (QEMU)
- Tek netns, 5 eşzamanlı app: CS2 + Zoom + YouTube + Steam + SSH
- DSCP histogram her servis için
- `profile=gaming` vs `profile=remote_work` delta

### Real device
- `realworld-bench.sh` multi-app genişletme
- v2 vs v3 karşılaştırma (gaming latency Δ)
- Tez grafiği: 5-app altında CS2 ping jitter

## 13. Roadmap

| Faz | İçerik | Süre |
|---|---|---|
| **3a** | service_t taksonomi + `myco_service.c` skeleton | 0.5g |
| **3b** | DNS catalog genişletme (60+ domain) | 0.5g |
| **3c** | Port hint → service mapping | 0.3g |
| **3d** | Behavior-based service inference | 0.5g |
| **3e** | CONNMARK-based mangle + libnfct | 1g |
| **3f** | Flow→service→ct mark apply loop | 1g |
| **4a** | `config device` + `config profile` UCI | 0.5g |
| **4b** | winner_priority persona derivation | 0.5g |
| **4c** | Profile switch → mangle rebuild | 0.3g |
| **5a** | Per-flow RTT tracking | 1g |
| **5b** | Misclassification auto-correction | 1g |
| **6a** | LuCI device modal + service bars | 1.5g |
| **6b** | Multi-app benchmark + tez grafikleri | 1g |

**Toplam**: ~10 gün (UI + daemon paralel).

**Onaylanmış karar — Faz sırası**: Flow (3) → Profile (4) → Feedback (5) → UI+Bench (6).
