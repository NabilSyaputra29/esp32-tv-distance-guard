#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <HardwareSerial.h>
#include <esp_system.h>
#include "DFRobotDFPlayerMini.h"

// ================= FORMAT NAMA FILE DI SD CARD =================
// 1 = nama 4 digit: /01/0001.mp3 dan /01/0002.mp3  (pakai playLargeFolder)
// 0 = nama 3 digit: /01/001.mp3  dan /01/002.mp3   (pakai playFolder)
#define DF_NAME_4DIGIT  1

// ================= PIN =================
#define TRIG_PIN   18
#define ECHO_PIN   19   // WAJIB pembagi tegangan 5V -> 3.3V (mis. 1k + 2k)
#define DF_RX_PIN  16   // <- TX DFPlayer
#define DF_TX_PIN  17   // -> RX DFPlayer (lewat resistor 1k)
#define RELAY_PIN  5    // IN1 relay

// Level pin yang MEMUTUS daya TV.
// Modul relay active-LOW  + kabel TV lewat COM-NC -> LOW  (rekomendasi)
// Modul relay active-HIGH + kabel TV lewat COM-NC -> HIGH
// Modul relay active-LOW  + kabel TV lewat COM-NO -> HIGH
// Modul relay active-HIGH + kabel TV lewat COM-NO -> LOW
#define TV_CUT_LEVEL   LOW
#define TV_POWER_LEVEL (TV_CUT_LEVEL == LOW ? HIGH : LOW)

// ================= KREDENSIAL (ISI SENDIRI, JANGAN DIBAGIKAN) =================
const char* WIFI_SSID = "GANTI_DENGAN_NAMA_WIFI_ATAU_HOTSPOT_ANDA";                 // harus persis sama dengan nama hotspot
const char* WIFI_PASS = "GANTI_DENGAN_PASSWORD_WIFI_ANDA";
#define BOT_TOKEN "ISI_TOKEN_BOT_TELEGRAM_ANDA"            // token baru dari @BotFather
#define CHAT_ID   "ISI_CHAT_ID_TELEGRAM_ANDA"

// ================= PARAMETER =================
const float DIST_SAFE   = 200.0;   // > 200 cm  = Level 1 (aman)
const float DIST_DANGER = 150.0;   // < 150 cm  = Level 3 (bahaya)
const unsigned long READ_INTERVAL_MS   = 100;
const unsigned long LVL2_AUDIO_GAP_MS  = 3000;
const unsigned long LVL3_AUDIO_GAP_MS  = 5000;
const unsigned long LVL3_NOTIF_MS      = 10000;
const unsigned long PRINT_INTERVAL_MS  = 500;
const unsigned long WIFI_RETRY_MS      = 15000;   // jeda antar percobaan sambung ulang
const unsigned long TG_POLL_MS         = 3000;
const unsigned long DF_FIRST_DELAY_MS  = 4000;
const unsigned long DF_RETRY_MS        = 20000;
const uint8_t DF_VOLUME = 30;                     // 0-30
const int N_SAMPLES = 5;

// ================= OBJEK =================
HardwareSerial dfSerial(2);
DFRobotDFPlayerMini myDFPlayer;
WiFiClientSecure secureClient;
UniversalTelegramBot bot(BOT_TOKEN, secureClient);

// ================= STATE =================
enum State { ST_SAFE, ST_WARN, ST_DANGER };
volatile State state = ST_SAFE;
State lastPrintedState = ST_DANGER;

RTC_DATA_ATTR uint32_t bootCount = 0;

bool dfReady = false;
bool dfSerialStarted = false;
bool dfTriedOnce = false;
bool notifSent = false;
volatile bool telegramPending = false;
volatile bool bootMsgPending = true;
String bootMsg = "";

float samples[N_SAMPLES];
int sampleIdx = 0;
volatile float distance = 400.0;

unsigned long lastRead = 0, lastPrint = 0, lastWifiTry = 0, lastDfTry = 0;
unsigned long lastLvl2Audio = 0, lastLvl3Audio = 0, lvl3Start = 0;

// ================= RELAY =================
bool tvIsOn = true;

void tvOn() {
  if (tvIsOn) return;
  digitalWrite(RELAY_PIN, TV_POWER_LEVEL);
  tvIsOn = true;
  Serial.println("[Relay] TV/lampu NYALA (daya tersambung).");
}

void tvOff() {
  if (!tvIsOn) return;
  digitalWrite(RELAY_PIN, TV_CUT_LEVEL);
  tvIsOn = false;
  Serial.println("[Relay] TV/lampu MATI (daya diputus).");
}

// ================= UTIL =================
const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "Power-on";
    case ESP_RST_EXT:       return "Tombol EN";
    case ESP_RST_SW:        return "Software restart";
    case ESP_RST_PANIC:     return "CRASH program (panic)";
    case ESP_RST_INT_WDT:   return "Watchdog interrupt";
    case ESP_RST_TASK_WDT:  return "Watchdog task";
    case ESP_RST_WDT:       return "Watchdog lain";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (catu daya lemah)";
    default:                return "Tidak diketahui";
  }
}

const char* stateName(State s) {
  switch (s) {
    case ST_SAFE:   return "LEVEL 1 (AMAN)";
    case ST_WARN:   return "LEVEL 2 (PERINGATAN)";
    case ST_DANGER: return "LEVEL 3 (BAHAYA)";
  }
  return "?";
}

// ================= SENSOR =================
// 0 = echo normal, 1 = tidak ada echo, 2 = ECHO tertahan HIGH sebelum trigger
volatile uint8_t sensorFlag = 1;

volatile unsigned long rawLast = 0;
volatile unsigned long readCount = 0;
unsigned long rawRing[20];
int rawRingIdx = 0;

unsigned long rawVariation() {
  unsigned long mn = 0xFFFFFFFF, mx = 0;
  int cnt = 0;
  for (int i = 0; i < 20; i++) {
    if (rawRing[i] == 0) continue;
    cnt++;
    if (rawRing[i] < mn) mn = rawRing[i];
    if (rawRing[i] > mx) mx = rawRing[i];
  }
  return cnt ? (mx - mn) : 0;
}

const char* sensorStatusName() {
  switch (sensorFlag) {
    case 0:  return "ECHO OK";
    case 1:  return "TIDAK ADA ECHO";
    default: return "ECHO NYANGKUT HIGH";
  }
}

float readDistanceOnce() {
  bool stuckHigh = (digitalRead(ECHO_PIN) == HIGH);

  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  rawLast = duration;
  rawRing[rawRingIdx] = duration;
  rawRingIdx = (rawRingIdx + 1) % 20;
  readCount++;
  if (duration == 0) {
    sensorFlag = stuckHigh ? 2 : 1;
    return 400.0;
  }
  sensorFlag = 0;
  return (duration * 0.0343f) / 2.0f;
}

float median(float *src, int n) {
  float a[N_SAMPLES];
  for (int i = 0; i < n; i++) a[i] = src[i];
  for (int i = 1; i < n; i++) {
    float k = a[i]; int j = i - 1;
    while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; }
    a[j + 1] = k;
  }
  return a[n / 2];
}

// ================= DFPLAYER =================
void printDFError() {
  if (myDFPlayer.available()) {
    uint8_t type = myDFPlayer.readType();
    int val = myDFPlayer.read();
    Serial.printf("[DF] Pesan modul: type=%d value=%d\n", type, val);
  } else {
    Serial.println("[DF] Tidak ada respon dari modul.");
  }
}

bool tryInitDF() {
  Serial.println("[DF] Menyambung DFPlayer...");
  Serial.flush();

  if (!dfSerialStarted) {
    dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
    dfSerialStarted = true;
    delay(200);
  }
  while (dfSerial.available()) dfSerial.read();

  bool ok = false;
  if (myDFPlayer.begin(dfSerial, true, false)) {
    Serial.println("[DF] Terhubung (ACK).");
    ok = true;
  } else {
    printDFError();
    myDFPlayer.begin(dfSerial, false, false);          // modul clone tanpa ACK
    myDFPlayer.setTimeOut(700);
    delay(200);
    int n = myDFPlayer.readFileCounts();
    if (n > 0) {
      Serial.printf("[DF] Terhubung (tanpa ACK), %d file.\n", n);
      ok = true;
    }
  }

  if (!ok) {
    Serial.println("[DF] GAGAL. Cek TX/RX disilang, VCC 5V, GND, SD FAT32.");
    return false;
  }

  myDFPlayer.setTimeOut(500);
  myDFPlayer.volume(DF_VOLUME);
  delay(100);

  int totalFiles = myDFPlayer.readFileCounts();
  delay(50);
  int folder01 = myDFPlayer.readFileCountsInFolder(1);
  Serial.printf("[DF] File di SD: %d | File di folder 01: %d\n", totalFiles, folder01);
  if (folder01 < 2) {
    Serial.println("[DF] PERINGATAN: folder 01 harus berisi 001.mp3 dan 002.mp3 (nama harus persis).");
  }
  return true;
}

void playAudio(uint8_t track) {
  if (!dfReady) {
    Serial.println("[Audio] DFPlayer belum siap, suara dilewati.");
    return;
  }
#if DF_NAME_4DIGIT
  myDFPlayer.playLargeFolder(1, track);   // /01/0001.mp3 , /01/0002.mp3
#else
  myDFPlayer.playFolder(1, track);        // /01/001.mp3 , /01/002.mp3
#endif
}

void checkDFMessages() {
  if (!dfReady) return;
  while (myDFPlayer.available()) {
    uint8_t type = myDFPlayer.readType();
    int value = myDFPlayer.read();
    if (type == DFPlayerPlayFinished) {
      Serial.printf("[DF] Selesai memutar track %d\n", value);
    } else if (type == DFPlayerError) {
      Serial.printf("[DF] ERROR modul (kode %d): ", value);
      switch (value) {
        case 1: Serial.println("modul sibuk / SD Card tidak terbaca"); break;
        case 2: Serial.println("modul sleep"); break;
        case 3: Serial.println("stack salah"); break;
        case 4: Serial.println("checksum salah (kabel/noise)"); break;
        case 5: Serial.println("nomor file di luar jangkauan"); break;
        case 6: Serial.println("FILE TIDAK DITEMUKAN -> cek nama file di folder 01"); break;
        case 7: Serial.println("sedang iklan"); break;
        default: Serial.println("tidak diketahui"); break;
      }
    }
  }
}

// ================= WIFI =================
const char* wifiStatusName(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "idle";
    case WL_NO_SSID_AVAIL:   return "SSID TIDAK DITEMUKAN (cek nama / band 2.4GHz)";
    case WL_SCAN_COMPLETED:  return "scan selesai";
    case WL_CONNECTED:       return "tersambung";
    case WL_CONNECT_FAILED:  return "GAGAL (cek password / WPA2)";
    case WL_CONNECTION_LOST: return "koneksi hilang";
    case WL_DISCONNECTED:    return "terputus";
    default:                 return "tidak diketahui";
  }
}

// Menampilkan alasan putus dari ESP32 -> paling akurat untuk diagnosa
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("[WiFi] Terhubung ke AP, menunggu IP...");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("[WiFi] SIAP. IP: ");
      Serial.println(WiFi.localIP());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] Putus. Kode alasan: %d ", info.wifi_sta_disconnected.reason);
      switch (info.wifi_sta_disconnected.reason) {
        case 2:  case 15: case 202: Serial.println("(password salah / keamanan tidak cocok, coba WPA2)"); break;
        case 201:                   Serial.println("(SSID tidak ditemukan: nama salah / hotspot 5GHz / hotspot mati)"); break;
        case 205:                   Serial.println("(gagal konek ke AP)"); break;
        case 200: case 4: case 8:   Serial.println("(sinyal hilang / AP memutus)"); break;
        default:                    Serial.println(); break;
      }
      break;
    default:
      break;
  }
}

// Scan sekali saat boot: pastikan hotspot terlihat oleh ESP32
void scanNetworksOnce() {
  Serial.println("[WiFi] Scan jaringan...");
  int n = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < n; i++) {
    bool match = (WiFi.SSID(i) == String(WIFI_SSID));
    if (match) found = true;
    Serial.printf("  %s[%s] ch%d RSSI %d dBm\n", match ? ">> " : "   ",
                  WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  }
  if (n <= 0)      Serial.println("[WiFi] Tidak ada jaringan terdeteksi.");
  else if (!found) Serial.printf("[WiFi] Hotspot \"%s\" TIDAK terlihat. Cek nama persis & band 2.4GHz.\n", WIFI_SSID);
  else             Serial.println("[WiFi] Hotspot terlihat, mencoba menyambung.");
  WiFi.scanDelete();
}

void handleWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWifiTry >= WIFI_RETRY_MS) {
    lastWifiTry = millis();
    Serial.printf("[WiFi] Menyambung... status: %s\n", wifiStatusName(WiFi.status()));
    // Tanpa WiFi.disconnect(): memutus tiap kali justru menggagalkan proses konek yang sedang berjalan
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ================= TELEGRAM =================
String buildStatus() {
  unsigned long sec = millis() / 1000;
  String msg = "📊 Status Sistem\n";
  msg += "Jarak: " + String((float)distance, 1) + " cm\n";
  msg += "Level: " + String(stateName(state)) + "\n";
  msg += "Sensor: " + String(sensorStatusName()) + "\n";
  msg += "Audio: " + String(dfReady ? "siap" : "TIDAK TERHUBUNG") + "\n";
  msg += "WiFi: " + String(WiFi.RSSI()) + " dBm\n";
  msg += "Aktif: " + String(sec / 3600) + "j " + String((sec % 3600) / 60) + "m " + String(sec % 60) + "d\n";
  msg += "Jumlah boot: " + String(bootCount);
  return msg;
}

void handleCommand(String text) {
  text.trim();
  text.toLowerCase();
  int at = text.indexOf('@');
  if (at > 0) text = text.substring(0, at);

  if (text == "/status") {
    bot.sendMessage(CHAT_ID, buildStatus(), "");
  } else if (text == "/start" || text == "/help") {
    bot.sendMessage(CHAT_ID, "Perintah:\n/status - lihat jarak dan kondisi sistem", "");
  }
}

void pollTelegram() {
  int n = bot.getUpdates(bot.last_message_received + 1);
  while (n) {
    for (int i = 0; i < n; i++) {
      if (String(bot.messages[i].chat_id) != String(CHAT_ID)) continue;   // hanya pemilik
      handleCommand(bot.messages[i].text);
    }
    n = bot.getUpdates(bot.last_message_received + 1);
  }
}

void telegramTask(void *param) {
  bool flushed = false;
  unsigned long lastPoll = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {

      if (!flushed) {                       // buang pesan lama
        int n = bot.getUpdates(bot.last_message_received + 1);
        while (n) n = bot.getUpdates(bot.last_message_received + 1);
        flushed = true;
      }

      if (bootMsgPending) {
        String m = bootMsg + "\nAudio: " + String(dfReady ? "siap" : "belum terhubung");
        if (bot.sendMessage(CHAT_ID, m, "")) bootMsgPending = false;
        else vTaskDelay(5000 / portTICK_PERIOD_MS);
      }

      if (telegramPending) {
        String msg = "⚠️ *Peringatan*\nJarak menonton kurang dari 150 cm.\nTV telah dimatikan.";
        if (bot.sendMessage(CHAT_ID, msg, "Markdown")) {
          telegramPending = false;
          Serial.println("[Telegram] Notifikasi terkirim.");
        } else {
          Serial.println("[Telegram] Gagal kirim, coba lagi 5 detik.");
          vTaskDelay(5000 / portTICK_PERIOD_MS);
        }
      }

      if (millis() - lastPoll >= TG_POLL_MS) {
        lastPoll = millis();
        pollTelegram();
      }
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// ================= LOGIKA UTAMA =================
void enterDanger(unsigned long now) {
  state = ST_DANGER;
  lvl3Start = now;
  lastLvl3Audio = now;
  playAudio(2);
  Serial.println("[Audio] Level 3: \"Ayo mundur! Kamu terlalu dekat.\"");
}

void enterWarn(unsigned long now) {
  state = ST_WARN;
  lastLvl2Audio = now - LVL2_AUDIO_GAP_MS;   // bunyi langsung saat masuk
}

void updateLogic(float d) {
  unsigned long now = millis();

  // Anak menjauh -> reset semuanya
  if (d > DIST_SAFE) {
    state = ST_SAFE;
    notifSent = false;
    tvOn();                                         // jarak > 200 cm -> TV langsung nyala
    return;
  }

  switch (state) {

    case ST_SAFE:
      if (d < DIST_DANGER) enterDanger(now);
      else                 enterWarn(now);
      break;

    case ST_WARN:
      if (d < DIST_DANGER) {
        enterDanger(now);
      } else if (now - lastLvl2Audio >= LVL2_AUDIO_GAP_MS) {
        lastLvl2Audio = now;
        playAudio(1);
        Serial.println("[Audio] Level 2: \"Yuk, mundur sedikit.\"");
      }
      break;

    case ST_DANGER:
      if (d >= DIST_DANGER) {
        enterWarn(now);
        break;
      }
      if (now - lastLvl3Audio >= LVL3_AUDIO_GAP_MS) {
        lastLvl3Audio = now;
        playAudio(2);
        Serial.println("[Audio] Level 3 (ulang).");
      }
      // 10 detik bertahan -> matikan TV, lalu kirim Telegram (sekali per kejadian)
      if (!notifSent && now - lvl3Start >= LVL3_NOTIF_MS) {
        notifSent = true;
        tvOff();
        telegramPending = true;
        Serial.println("[Telegram] Bertahan <150 cm selama 10 detik -> TV mati, kirim notifikasi.");
      }
      break;
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(300);

  bootCount++;
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.println();
  Serial.println("==============================");
  Serial.printf("Boot ke-%u | Alasan reset: %s\n", bootCount, resetReasonName(rr));
  Serial.println("==============================");
  Serial.flush();

  digitalWrite(RELAY_PIN, TV_POWER_LEVEL);   // set level dulu agar relay tidak "klik" saat boot
  pinMode(RELAY_PIN, OUTPUT);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  for (int i = 0; i < N_SAMPLES; i++) samples[i] = 400.0;

  // ---- WiFi ----
  WiFi.persistent(false);             // jangan tulis kredensial ke flash tiap begin()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);               // modem-sleep dimatikan: koneksi hotspot HP lebih stabil
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWifiEvent);
  // Daya pancar dibiarkan default (maksimum). Baris WIFI_POWER_11dBm dihapus
  // karena membuat sinyal ke hotspot terlalu lemah.
  scanNetworksOnce();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiTry = millis();

  secureClient.setInsecure();

  bootMsg = "🔄 Sistem menyala (boot ke-" + String(bootCount) + ")\nAlasan reset: " +
            String(resetReasonName(rr));

  // Prioritas 0 (sama dengan idle task) supaya proses TLS yang berat tidak
  // membuat idle task core 0 kelaparan -> penyebab "Watchdog task".
  xTaskCreatePinnedToCore(telegramTask, "telegram", 16384, NULL, 0, NULL, 0);

  Serial.println("--- Sistem Siap (WiFi & DFPlayer menyusul di background) ---");
}

// ================= LOOP =================
void loop() {
  unsigned long now = millis();

  handleWifi();

  // DFPlayer diinisialisasi belakangan, hanya saat kondisi aman
  if (!dfReady && now >= DF_FIRST_DELAY_MS &&
      (WiFi.status() == WL_CONNECTED || now > 15000) &&
      (!dfTriedOnce || now - lastDfTry >= DF_RETRY_MS)) {
    dfTriedOnce = true;
    lastDfTry = now;
    dfReady = tryInitDF();
  }

  checkDFMessages();

  if (now - lastRead >= READ_INTERVAL_MS) {
    lastRead = now;
    samples[sampleIdx] = readDistanceOnce();
    sampleIdx = (sampleIdx + 1) % N_SAMPLES;
    distance = median(samples, N_SAMPLES);
    updateLogic(distance);
  }

  if (now - lastPrint >= PRINT_INTERVAL_MS || state != lastPrintedState) {
    lastPrint = now;
    lastPrintedState = state;
    Serial.printf("Jarak: %.1f cm [%s raw=%luus var=%luus n=%lu] | %s | WiFi: %s | Audio: %s | TV: %s\n",
                  (float)distance, sensorStatusName(), rawLast, rawVariation(), readCount,
                  stateName(state),
                  WiFi.status() == WL_CONNECTED ? "OK" : "putus",
                  dfReady ? "OK" : "x",
                  tvIsOn ? "ON" : "OFF");
  }
}