// LoRa <-> ESP-NOW bridge for the CYD Beat Console, on a Heltec WiFi LoRa 32 V3.
// Listens to the Beat Commander conductor (W10) on 916.0 MHz (SF7 / BW500) and relays every beat, with RSSI / SNR,
// to the console over ESP-NOW. When the console takes over, it sends beats here and this board
// transmits them on the beat link, so followers keep flashing from the booth.
// Needs no screen: a Heltec V3 with a dead OLED works fine. White LED: blink = beat heard, double = beat sent.
// With a working OLED it's a beat screen too (oled_viz.h): BEAT, PULSE, BARS, TUNNEL, WARP, SCOPE and STATS
// (heard / sent for the booth / booth gear seen). PRG press = next screen, hold 1 s = AUTO. Saved.
// Built by Anubis Darkwatch with Claude. MIT license.
//
// Arduino IDE 2.x, esp32 by Espressif 3.x. Board "ESP32S3 Dev Module",
// USB CDC On Boot: Disabled, PSRAM: Disabled. Library: LovyanGFX. Serial monitor 115200.

#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "link.h"
#include "oled_screen.h"

#define FW_VERSION "1.1"

// ---------- options ----------
#define LINK_FREQ_HZ   916000000UL
#define LINK_POWER_DBM 10          // console takeover TX power (the W10 uses 10 dBm)

// ---------- Heltec V3 pins ----------
#define L_NSS 8
#define L_SCK 9
#define L_MOSI 10
#define L_MISO 11
#define L_RST 12
#define L_BUSY 13
#define L_DIO1 14
#define LED_PIN 35

// ================= SX1262 =================
SPISettings loraSPI(8000000, MSBFIRST, SPI_MODE0);
void waitBusy(uint32_t ms = 300) { uint32_t t = millis(); while (digitalRead(L_BUSY) && millis() - t < ms) delayMicroseconds(20); }
void loraXfer(const uint8_t* d, int n, uint8_t* rx = nullptr) {
  waitBusy();
  SPI.beginTransaction(loraSPI); digitalWrite(L_NSS, LOW);
  for (int i = 0; i < n; i++) { uint8_t r = SPI.transfer(d[i]); if (rx) rx[i] = r; }
  digitalWrite(L_NSS, HIGH); SPI.endTransaction();
}
void cmd(std::initializer_list<uint8_t> l) { uint8_t b[16]; int n = 0; for (auto x : l) b[n++] = x; loraXfer(b, n); }
uint8_t readReg(uint16_t a) { uint8_t t[5] = {0x1D, (uint8_t)(a >> 8), (uint8_t)a, 0, 0}, r[5]; loraXfer(t, 5, r); return r[4]; }
void writeReg(uint16_t a, uint8_t v) { cmd({0x0D, (uint8_t)(a >> 8), (uint8_t)a, v}); }

void startRx() {
  cmd({0x8C, 0x00, 0x08, 0x00, 0xFF, 0x01, 0x00});     // preamble 8, explicit, max len, CRC on, IQ normal
  cmd({0x02, 0xFF, 0xFF});                             // clear IRQs
  cmd({0x82, 0xFF, 0xFF, 0xFF});                       // RX continuous
}

bool loraInit() {
  pinMode(L_RST, OUTPUT); digitalWrite(L_RST, LOW); delay(10); digitalWrite(L_RST, HIGH); delay(20); waitBusy(1000);
  cmd({0x80, 0x00});                                   // standby RC
  if (readReg(0x0740) != 0x14) { Serial.println("# SX1262 not responding"); return false; }
  cmd({0x97, 0x02, 0x00, 0x01, 0x40});                 // DIO3 -> TCXO 1.8 V, 5 ms
  cmd({0x89, 0x7F}); delay(10); waitBusy();            // calibrate all (needed after TCXO)
  cmd({0x96, 0x01});                                   // DC-DC
  cmd({0x9D, 0x01});                                   // DIO2 = RF switch
  cmd({0x8A, 0x01});                                   // LoRa
  cmd({0x98, 0xE1, 0xE9}); delay(5);                   // image cal 902-928
  uint32_t f = (uint32_t)(((uint64_t)LINK_FREQ_HZ << 25) / 32000000ULL);
  cmd({0x86, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f});
  cmd({0x95, 0x04, 0x07, 0x00, 0x01});                 // PA config (SX1262 high power)
  cmd({0x8E, (uint8_t)LINK_POWER_DBM, 0x04});          // TX power, 200 us ramp
  cmd({0x8B, 0x07, 0x06, 0x01, 0x00});                 // SF7, BW500, CR4/5 (matches the conductor)
  cmd({0x8F, 0x00, 0x00});                             // TX and RX buffer base 0
  writeReg(0x08AC, 0x96);                              // RX boosted gain
  cmd({0x08, 0x02, 0x43, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00}); // IRQ: TxDone|RxDone|CrcErr|HdrErr, DIO1 = TxDone|RxDone
  startRx();
  return true;
}

volatile bool txBusy = false;
uint32_t txStart = 0;
uint8_t lastSeq = 0; bool heardAny = false;
uint32_t heard = 0, sent = 0;
uint32_t lastRxMs = 0, lastTxMs = 0; int lastRssi = 0; int8_t lastSnr4 = 0;
BeatPacket txPkt;

void sendBeat(const BeatPacket& p) {
  uint8_t buf[2 + sizeof(BeatPacket)] = {0x0E, 0x00};  // write buffer at offset 0
  memcpy(buf + 2, &p, sizeof(p));
  cmd({0x80, 0x01});                                   // standby XOSC (leaves RX)
  loraXfer(buf, sizeof(buf));
  cmd({0x8C, 0x00, 0x08, 0x00, (uint8_t)sizeof(p), 0x01, 0x00}); // preamble 8, explicit, len, CRC on
  cmd({0x02, 0xFF, 0xFF});
  cmd({0x83, 0x00, 0x00, 0x00});                       // TX, ~10 ms airtime, then back to RX on TxDone
  txBusy = true; txPkt = p; txStart = millis();
}

// ================= ESP-NOW =================
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t fireQ;

void espnowSend(const LinkFrame& f) { esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f)); }

// booth gear seen (distinct senders in the last 30 s), for the stats screen
struct Peer { uint8_t mac[6]; uint32_t ms; };
Peer peers[8];
void notePeer(const uint8_t* mac) {
  uint32_t now = millis(); int slot = -1, oldest = 0;
  for (int i = 0; i < 8; i++) {
    if (peers[i].ms && memcmp(peers[i].mac, mac, 6) == 0) { slot = i; break; }
    if (peers[i].ms < peers[oldest].ms) oldest = i;
  }
  if (slot < 0) { slot = oldest; memcpy(peers[slot].mac, mac, 6); }
  peers[slot].ms = now | 1;
}
uint32_t peerCount() { uint32_t n = 0, now = millis(); for (auto& p : peers) if (p.ms && now - p.ms < 30000) n++; return n; }

void onFrame(const uint8_t* mac, const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  if (mac) notePeer(mac);
  LinkFrame f; memcpy(&f, d, sizeof(f));
  if (f.type == LF_FIRE) xQueueSend(fireQ, &f, 0);
}

bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t* i, const uint8_t* d, int len) { onFrame(i ? i->src_addr : nullptr, d, len); });
#else
  esp_now_register_recv_cb([](const uint8_t* mac, const uint8_t* d, int len) { onFrame(mac, d, len); });
#endif
  return true;
}

// ================= LED =================
uint32_t ledOffAt = 0, ledBlink2 = 0;
void led(uint32_t ms) { digitalWrite(LED_PIN, HIGH); ledOffAt = millis() + ms; }

bool radioOk = false;

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("# Beat Console bridge (Heltec V3) | LoRa 916.0 MHz SF7/BW500 <-> ESP-NOW ch " + String(LINK_ESPNOW_CH));
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  pinMode(L_NSS, OUTPUT); digitalWrite(L_NSS, HIGH);
  pinMode(L_BUSY, INPUT); pinMode(L_DIO1, INPUT);
  Serial.printf("# OLED %s\n", screenBegin(true) ? "found: beat screen on" : "not found (fine, it runs without)");
  fireQ = xQueueCreate(4, sizeof(LinkFrame));
  SPI.begin(L_SCK, L_MISO, L_MOSI, L_NSS);
  radioOk = loraInit();
  if (!espnowInit()) Serial.println("# ESP-NOW init failed");
  Serial.printf("# radio %s | MAC %s\n", radioOk ? "OK" : "FAILED", WiFi.macAddress().c_str());
  Serial.println("# dir,seq,bpm,beat,hue,rssi,snr");
  if (!radioOk) { while (1) { led(60); delay(120); digitalWrite(LED_PIN, LOW); delay(120); } }
}

void loop() {
  uint32_t now = millis();

  // --- console wants a beat on the air ---
  LinkFrame f;
  if (!txBusy && xQueueReceive(fireQ, &f, 0) == pdTRUE) {
    BeatPacket p = {'B', 'T', (uint8_t)(heardAny ? lastSeq + 1 : sent), f.bpm10, (uint8_t)(f.beat & 3), f.hue};
    lastSeq = p.seq; heardAny = true;                  // keep the followers' seq continuous: no false "lost" counts
    sendBeat(p);
  }

  // --- radio events ---
  if (digitalRead(L_DIO1)) {
    uint8_t t[4] = {0x12, 0, 0, 0}, r[4]; loraXfer(t, 4, r);
    uint16_t irq = (r[2] << 8) | r[3];
    cmd({0x02, 0xFF, 0xFF});
    if (irq & 0x0001) {                                // TxDone
      txBusy = false; sent++; startRx();
      LinkFrame a = linkFrame(LF_ACK);
      a.seq = txPkt.seq; a.bpm10 = txPkt.bpm10; a.beat = txPkt.beat; a.hue = txPkt.hue; a.count = sent;
      espnowSend(a);
      led(25); ledBlink2 = now + 60;
      screenBeat(txPkt.bpm10, txPkt.beat, now); lastTxMs = now;
      Serial.printf("TX,%u,%.1f,%u,%u,,\n", txPkt.seq, txPkt.bpm10 / 10.0f, txPkt.beat, txPkt.hue);
    } else if ((irq & 0x0002) && !(irq & 0x0040)) {    // RxDone, CRC good
      uint8_t s[4] = {0x13, 0, 0, 0}, rs[4]; loraXfer(s, 4, rs);
      int len = rs[2], start = rs[3];
      uint8_t b[3 + 16] = {0x1E, (uint8_t)start, 0}, rb[3 + 16];
      if (len == (int)sizeof(BeatPacket)) {
        loraXfer(b, 3 + len, rb);
        uint8_t p4[5] = {0x14, 0, 0, 0, 0}, rp[5]; loraXfer(p4, 5, rp);
        BeatPacket p; memcpy(&p, rb + 3, sizeof(p));
        if (p.m0 == 'B' && p.m1 == 'T') {
          int rssi = -(int)rp[2] / 2; int8_t snr4 = (int8_t)rp[3];
          LinkFrame rf = linkFrame(LF_RELAY);
          rf.seq = p.seq; rf.bpm10 = p.bpm10; rf.beat = p.beat; rf.hue = p.hue; rf.rssi = rssi; rf.snr4 = snr4;
          espnowSend(rf);
          lastSeq = p.seq; heardAny = true; heard++;
          led(p.beat == 0 ? 60 : 25);
          screenBeat(p.bpm10, p.beat, now); lastRxMs = now; lastRssi = rssi; lastSnr4 = snr4;
          Serial.printf("RX,%u,%.1f,%u,%u,%d,%.1f\n", p.seq, p.bpm10 / 10.0f, p.beat, p.hue, rssi, snr4 / 4.0f);
        }
      }
    }
  }

  // --- heartbeat ---
  static uint32_t lastHello = 0;
  if (now - lastHello >= 1000) {
    lastHello = now;
    LinkFrame h = linkFrame(LF_HELLO);
    h.flags = radioOk ? 1 : 0; h.count = heard;
    espnowSend(h);
  }
  if (txBusy && now - txStart > 200) { txBusy = false; startRx(); }   // TxDone never came: recover

  // --- screen ---
  if (screenOk) {
    static uint32_t lastScr = 0;
    if (now - lastScr > 50) {
      lastScr = now;
      uint32_t last = lastTxMs > lastRxMs ? lastTxMs : lastRxMs, age = now - last;
      screenLink(!last ? OL_WAIT : age < 1500 ? (lastTxMs > lastRxMs ? OL_BOOTH : OL_LIVE) : age < 4000 ? OL_HOLD : OL_WAIT);
      screenStats(lastRssi, lastSnr4, heard, 0, sent, peerCount());
    }
    screenButton();
  }

  // --- LED ---
  if (ledOffAt && (int32_t)(now - ledOffAt) >= 0) { digitalWrite(LED_PIN, LOW); ledOffAt = 0; }
  if (ledBlink2 && (int32_t)(now - ledBlink2) >= 0) { ledBlink2 = 0; led(25); }
}
