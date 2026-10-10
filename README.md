# TV Warning - Peringatan Jarak Menonton TV untuk Anak

Proyek berbasis **ESP32** yang memantau jarak anak ke TV memakai sensor ultrasonik. Jika anak terlalu dekat, alat memberi peringatan suara bertahap. Jika tetap terlalu dekat selama 10 detik, **daya TV diputus lewat relay** dan orang tua menerima **notifikasi Telegram**. Begitu anak menjauh (> 200 cm), TV menyala kembali otomatis.

## Fitur

- Pengukuran jarak dengan sensor ultrasonik HC-SR04 (median 5 sampel agar stabil)
- 3 level peringatan dengan audio dari DFPlayer Mini
- Relay otomatis memutus dan menyambung daya TV
- Notifikasi dan perintah `/status` lewat bot Telegram
- WiFi auto-reconnect, dengan log diagnosa alasan putus
- Telegram berjalan di task terpisah (core 0) agar pembacaan sensor tidak terganggu
- Log serial berisi alasan reset (brownout, watchdog, dan lain-lain) dan jumlah boot

## Cara Kerja

| Level | Jarak | Aksi |
|-------|-------|------|
| 1 - Aman | > 200 cm | TV menyala, tanpa peringatan |
| 2 - Peringatan | 150 - 200 cm | Audio "Yuk, mundur sedikit." setiap 3 detik |
| 3 - Bahaya | < 150 cm | Audio "Ayo mundur! Kamu terlalu dekat." setiap 5 detik. Setelah 10 detik: **TV mati + notifikasi Telegram** |

Ambang batas dan jeda waktu dapat diubah di bagian `PARAMETER` pada kode.

## Komponen

- ESP32 (DevKit)
- Sensor ultrasonik HC-SR04
- Modul DFPlayer Mini + microSD (FAT32) + speaker
- Modul relay 1 channel
- Resistor: 1k dan 2k (pembagi tegangan ECHO), 1k (jalur TX ESP32 ke RX DFPlayer)
- Catu daya 5V yang cukup arus (disarankan terpisah dari USB jika audio sering brownout)

## Wiring

| Perangkat | Pin Perangkat | Pin ESP32 |
|-----------|---------------|-----------|
| HC-SR04 | TRIG | GPIO 18 |
| HC-SR04 | ECHO | GPIO 19 (**wajib** pembagi tegangan 5V ke 3.3V, mis. 1k + 2k) |
| DFPlayer | TX | GPIO 16 (RX2) |
| DFPlayer | RX | GPIO 17 (TX2), lewat resistor 1k |
| Relay | IN1 | GPIO 5 |
| Semua | VCC / GND | 5V / GND (ground disatukan) |

### Wiring relay

Level pin yang memutus daya TV diatur lewat `TV_CUT_LEVEL`:

| Modul relay | Kabel TV via | `TV_CUT_LEVEL` |
|-------------|--------------|----------------|
| Active-LOW | COM-NC | `LOW` (rekomendasi) |
| Active-HIGH | COM-NC | `HIGH` |
| Active-LOW | COM-NO | `HIGH` |
| Active-HIGH | COM-NO | `LOW` |

> ⚠️ **Peringatan keselamatan:** relay memutus listrik AC. Pasang hanya pada **satu kabel (fasa)** dari kabel TV, gunakan kotak/isolasi yang aman, dan jangan menyentuh bagian bertegangan saat tersambung. Jika ragu, minta bantuan teknisi listrik.

## Struktur SD Card

Format SD card ke **FAT32**, lalu buat folder dan file dengan nama persis seperti berikut:

```
/01/0001.mp3   -> "Yuk, mundur sedikit."          (Level 2)
/01/0002.mp3   -> "Ayo mundur! Kamu terlalu dekat." (Level 3)
```

Jika memakai nama 3 digit (`001.mp3`, `002.mp3`), ubah di kode:

```cpp
#define DF_NAME_4DIGIT  0
```

## Library yang Dibutuhkan

Instal lewat Library Manager Arduino IDE:

- `UniversalTelegramBot` (Brian Lough)
- `ArduinoJson` (dependensi UniversalTelegramBot)
- `DFRobotDFPlayerMini` (DFRobot)

Board: **ESP32** (Boards Manager: *esp32 by Espressif Systems*).

## Konfigurasi

1. Buat bot lewat [@BotFather](https://t.me/BotFather) dan salin token-nya.
2. Dapatkan `CHAT_ID` Anda (mis. lewat bot `@userinfobot`).
3. Isi kredensial di kode:

```cpp
const char* WIFI_SSID = "GANTI_DENGAN_NAMA_WIFI_ATAU_HOTSPOT_ANDA";   // harus 2.4 GHz
const char* WIFI_PASS = "GANTI_DENGAN_PASSWORD_WIFI_ANDA";
#define BOT_TOKEN "TOKEN_BOT_TELEGRAM"
#define CHAT_ID   "CHAT_ID_ANDA"
```

> 🔒 **Jangan commit kredensial asli ke GitHub.** Simpan di file `secrets.h` yang dimasukkan ke `.gitignore`, lalu `#include "secrets.h"` di sketch. Sediakan `secrets.example.h` sebagai contoh isi.

4. Upload sketch ke ESP32, buka Serial Monitor pada **115200 baud**.

## Perintah Telegram

| Perintah | Fungsi |
|----------|--------|
| `/status` | Menampilkan jarak, level, status sensor, audio, kekuatan WiFi, uptime, dan jumlah boot |
| `/start`, `/help` | Daftar perintah |

Bot hanya merespons `CHAT_ID` pemilik.

## Pemecahan Masalah

| Gejala | Kemungkinan penyebab |
|--------|----------------------|
| `TIDAK ADA ECHO` | Kabel TRIG/ECHO salah, sensor kurang daya, atau tidak ada objek di depan sensor |
| `ECHO NYANGKUT HIGH` | Pembagi tegangan ECHO salah atau sensor bermasalah |
| `[DF] GAGAL` | TX/RX tidak disilang, VCC bukan 5V, SD bukan FAT32, atau folder `01` tidak ada |
| `FILE TIDAK DITEMUKAN` | Nama file/folder di SD tidak persis atau format 3/4 digit tidak sesuai |
| WiFi tidak tersambung | Hotspot 5 GHz (ESP32 hanya 2.4 GHz), nama/password salah |
| Reset `BROWNOUT` | Catu daya lemah, gunakan sumber 5V yang lebih kuat |
| TV terbalik (mati saat aman) | Atur ulang `TV_CUT_LEVEL` sesuai tabel wiring relay |

## Struktur Repositori

```
esp32-tv-distance-guard/
├── README.md
└── TV_WARNING/
    └── TV_WARNING.ino
```

## Lisensi

Proyek ini dilisensikan di bawah [MIT License](LICENSE).
