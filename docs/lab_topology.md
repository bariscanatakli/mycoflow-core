# Gelişmiş Simülasyon Topolojisi (Host → OpenWrt → VM’ler)

Bu doküman, host makinenin internetini OpenWrt konteynerine WAN olarak verip, OpenWrt’nin ürettiği LAN ağını küçük VM’lere dağıtarak gerçekçi test yapılmasını anlatır. Bu kurulum ileriki fazlar için planlanmıştır.

## Amaç
- Host internetini OpenWrt üzerinden yönlendirmek
- OpenWrt üzerinde NAT/DHCP/firewall testleri yapmak
- VM’leri OpenWrt’nin LAN’ına bağlayıp gerçekçi trafik simülasyonu üretmek

## Önerilen Mimari
- Host: İnternete çıkışı sağlayan fiziksel NIC (ör. eth0/wlan0)
- OpenWrt (Docker): WAN ve LAN için ayrı sanal arayüzler
- VM’ler: OpenWrt’nin LAN bridge’ine bağlı sanal NIC’ler

## Yüksek Seviye Ağ Akışı
1. Host internet → OpenWrt WAN arayüzü
2. OpenWrt NAT → LAN arayüzü
3. LAN bridge → VM’ler (DHCP ile IP alır)

## Gereken Bileşenler
- Host üzerinde bridge ve tap/veth arayüzleri
- OpenWrt konteynerine iki ayrı arayüz (WAN/LAN) verilmesi
- VM’lerin host üzerindeki LAN bridge’ine bağlanması
- OpenWrt üzerinde DHCP + NAT konfigürasyonu

## Konsept Kurulum Adımları (Özet)
1. Host bridge oluştur (LAN için)
2. OpenWrt konteynerine WAN ve LAN arayüzü ekle
3. VM’leri host bridge’e bağla
4. OpenWrt’de WAN/LAN arayüzlerini eşleştir
5. DHCP/NAT ve firewall ayarlarını doğrula

## Dikkat Edilmesi Gerekenler
- Host tarafında iptables/nftables kuralları
- Docker’ın default network izolasyonu (bridge/host mode seçimi)
- OpenWrt uhttpd/LuCI erişimi ve güvenlik kısıtları
- VM’lerin MAC/IP çakışmaları ve DHCP sınırları

## Kullanım Alanları
- QoS/CAKE testleri
- Real-time trafik testleri (VoIP, streaming, bulk download)
- MycoFlow aktüasyon etkilerini gözlemleme

## Not
Bu yapı Phase 2 kapsamı dışında tutulmuştur ve ileriki fazlarda uygulanacaktır. İhtiyaç oluştuğunda bu doküman detaylandırılacaktır.