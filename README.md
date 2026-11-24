# Moonlight T4

Moonlight oyun yayını için **Tizen 4.0 (2018) Samsung TV** portu. İlk hedef UE55NU8000.

Resmî Tizen portu ([moonlight-tizen](https://github.com/brightcraft/moonlight-tizen)) en az Tizen 5.5 (2020+) istiyor. Görüntüyü düşük gecikmeyle TV'nin donanım çözücüsüne gönderen Samsung API'si Tizen 4.0'da yok. Bu proje sorunu bir **köprü** programıyla çözüyor:

```
[PC: Sunshine] ──Moonlight protokolü──▶ [Köprü (PC'de)] ──WebSocket──▶ [TV: Moonlight T4]
                                             ▲                              │
                                             └──── kumanda / klavye / gamepad ┘
```

- **Köprü** (`bridge/`, C): Moonlight'ın resmî protokol kütüphanesi [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) ile Sunshine'a bağlanır.
  - Görüntü: H.264 yeniden kodlanmaz, yalnızca fMP4 parçalarına sarılır. Bu yüzden ek gecikme çok az.
  - Ses: Opus çözülüp PCM olarak gönderilir.
  - Girdi: TV'den gelen tuş, fare ve gamepad olayları Sunshine'a iletilir.
- **TV uygulaması** (`tizen-app/`, HTML/JS): Görüntüyü MSE ile, sesi Web Audio ile çalar ve oynatma konumunu canlı uca yakın tutar.

> Durum: **erken sürüm (0.1)**. Köprü ve uygulama, test modunda Chromium üzerinde uçtan uca test edildi. Gerçek bir Sunshine ve gerçek TV ile henüz denenmedi.

## 1. Köprüyü kurma

Köprüyü Sunshine'ın çalıştığı bilgisayarda çalıştırmak en kolayı. Aynı ağdaki başka bir makinede (Linux kutusu, Raspberry Pi) de çalışabilir.

### Windows
GitHub'daki **Actions → build** sayfasından son çalışmanın `moonlight-bridge-windows-x64` çıktısını indirin. Komut isteminde çalıştırın:

```bat
moonlight-bridge.exe
```

Windows güvenlik duvarı sorarsa **özel ağ** için izin verin. TV, 8090 numaralı TCP portuna bağlanır.

### Linux (kaynaktan derleme)
```bash
sudo apt install build-essential cmake pkg-config libssl-dev libopus-dev
```
```bash
git clone --recursive https://github.com/vigetsu/moonlight-tizen4
```
```bash
cd moonlight-tizen4 && cmake -S bridge -B bridge/build && cmake --build bridge/build -j
```
```bash
./bridge/build/moonlight-bridge --host 127.0.0.1
```

Seçenekler:

| Seçenek | Açıklama |
|---|---|
| `--host ADRES` | Sunshine'ın adresi (varsayılan `127.0.0.1`) |
| `--port N` | TV uygulamasının bağlanacağı port (varsayılan `8090`) |
| `--test DOSYA` | Sunshine olmadan test: bir H.264 dosyasını döngüde yayınlar |

Eşleştirme sertifikası `~/.config/moonlight-tizen4/` (Windows'ta `%APPDATA%\moonlight-tizen4\`) içinde saklanır.

## 2. TV uygulamasını yükleme

Samsung TV'ler yalnızca imzalı uygulama kabul eder. İmzayı ücretsiz Samsung sertifikasıyla Tizen Studio üretir.

1. **TV'de geliştirici modunu açın:** Apps (Uygulamalar) ekranında kumandadan `1 2 3 4 5` tuşlayın. **Developer mode**'u **On** yapın, bilgisayarın IP adresini girin ve TV'yi kapatıp açın.
2. **[Tizen Studio](https://developer.tizen.org/development/tizen-studio/download)** kurun. Package Manager'dan **Samsung TV Extensions** ve **TV 4.0** profilini ekleyin.
3. **Sertifika:** *Tools → Certificate Manager → +* yolunu izleyin, **Samsung** türünü ve **TV** cihazını seçin, Samsung hesabıyla giriş yapın. Distributor sertifikası oluştururken TV'nin **DUID**'ini ekleyin. TV bağlıyken Device Manager bunu otomatik doldurur.
4. **TV'yi bağlayın:** *Tools → Device Manager → Remote Device Manager → +* ile TV'nin IP adresini girip bağlanın.
5. **Projeyi açın:** *File → Import → Tizen → Tizen Project* yolundan `tizen-app` klasörünü seçin, profil olarak `tv-samsung 4.0` (Web) seçin.
6. Projeye sağ tıklayın ve **Run As → Tizen Web Application** deyin. Uygulama TV'ye kurulup açılır.

Komut satırını tercih ederseniz (Tizen Studio CLI):
```bash
tizen package -t wgt -s SERTIFIKA_PROFILI -- tizen-app
```
```bash
sdb connect TV_IP && tizen install -n tizen-app/MoonlightT4.wgt -t TV_MODEL_ADI
```

## 3. Kullanım

1. Bilgisayarda **Sunshine** ve **köprü** çalışıyor olsun.
2. TV'de **Moonlight T4**'ü açın ve köprünün çalıştığı bilgisayarın IP adresini girin.
3. İlk seferde **Eşleştir** deyin. Ekrandaki 4 haneli PIN'i Sunshine web arayüzünde (`https://localhost:47990`) **PIN** sekmesine girin.
4. Listeden bir uygulama (ör. *Desktop*) seçin.

### Yayın sırasında kumanda

| Tuş | Görev |
|---|---|
| Ok tuşları / OK | PC'de ok tuşları / Enter (gamepad modunda D-pad / A) |
| GERİ (kısa) | Esc (gamepad modunda B) |
| GERİ (uzun) veya KIRMIZI | Yayın menüsü (devam, bağlantıyı kes, oyunu kapat) |
| YEŞİL | İstatistikleri göster/gizle |
| MAVİ | Kumandayı klavye ↔ gamepad modu arasında değiştir |

USB/Bluetooth **klavye, fare ve gamepad** TV'ye takıldığında doğrudan PC'ye iletilir.

### Ayarlar
Çözünürlük (720p–4K), kare hızı (30/60), bit hızı (5–50 Mbps) ve "sesi PC'de de çal" ayarları var. **1080p60 / 20 Mbps** iyi bir başlangıç. Görüntü takılıyorsa bit hızını düşürün. Mümkünse TV'yi **kabloyla** bağlayın.

## Sorun giderme

- **"Köprüye bağlanılamadı":** Köprü çalışıyor mu? TV'nin tarayıcısından `http://PC_IP:8090` adresini açın, "köprüsü çalışıyor" yazmalı. Yazmıyorsa güvenlik duvarını kontrol edin.
- **"Sunshine'a ulaşılamadı":** Sunshine çalışıyor mu? Köprü başka bir makinedeyse `--host` ile Sunshine'ın IP adresini verin.
- **Gecikme yüksek:** YEŞİL tuşla istatistikleri açın. *Tampon* değeri sürekli yüksekse TV'nin MSE oynatıcısı tampon tutuyor demektir. Bu değerleri bildirin, ayarlanabilir.
- **Sunshine olmadan TV testi:** Önce test videosunu üretin:
  ```bash
  sh tools/make-test-video.sh 1920x1080 60
  ```
  Sonra köprüyü test modunda başlatın:
  ```bash
  ./bridge/build/moonlight-bridge --test testdata/test-1920x1080-60.h264
  ```
  TV'de renkli bir test görüntüsü ve kısa bip sesleri gelmeli.

## Geliştirme

- Köprünün protokolü: TV'den JSON metin mesajları gelir (`hello`, `pair`, `apps`, `start`, `stop`, `quit`, `idr` ve girdi mesajları). Köprü JSON durum mesajlarıyla ve ilk baytı tür bildiren ikili mesajlarla yanıt verir (`1` = fMP4 init, `2` = fMP4 kare, `3` = PCM ses). Ayrıntılar için `bridge/src/main.c` ve `bridge/src/stream.h` dosyalarına bakın.
- Uygulama, Tizen 4.0'ın eski Chromium motoru için ES2015 ile yazıldı: `async/await`, `?.`, flex `gap` gibi özellikler kullanılmıyor. Normal bir tarayıcıda da açılıp test edilebilir.
- Otomatik test:
  ```bash
  node tools/smoke-test.mjs bridge/build/moonlight-bridge testdata/test-1280x720-60.h264
  ```

## Lisans

GPLv3. moonlight-common-c ile aynı lisans. "Moonlight" adı moonlight-stream projesine aittir. Bu proje resmî değildir.
