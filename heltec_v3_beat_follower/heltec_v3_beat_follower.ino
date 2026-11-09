// Beat Follower for Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262 + 0.96" SSD1306 OLED)
// Listens for Beat Commander (W10 conductor) packets on 916.0 MHz (SF7 / BW500) and flashes in sync.
// If packets drop out, a flywheel keeps flashing at the last tempo for a few seconds.
// The OLED is a beat screen (oled_viz.h): BEAT (big BPM), PULSE, BARS, TUNNEL, WARP, SCOPE and STATS.
//   PRG press = next screen,  PRG hold 1 s = AUTO (new visual every 8 bars). Saved.
// Built by Anubis Darkwatch with Claude. MIT license.
//
// Arduino IDE: board "ESP32S3 Dev Module" (esp32 core 3.x), USB CDC On Boot: Disabled, PSRAM: Disabled.
// Library: LovyanGFX. Optional WS2812 strip: Adafruit NeoPixel + set STRIP_LEN below.

#define FW_VERSION "1.1"
#include <SPI.h>
#include "oled_screen.h"

// ---------- options ----------
#define LINK_FREQ_HZ 916000000UL
#define FLYWHEEL_MS  4000      // keep flashing this long after the last packet
#define STRIP_PIN    47        // WS2812 data pin (header J3) if you add a strip
#define STRIP_LEN    0         // number of LEDs on the strip, 0 = no strip

// ---------- Heltec V3 pins ----------
#define L_NSS 8
#define L_SCK 9
#define L_MOSI 10
#define L_MISO 11
#define L_RST 12
#define L_BUSY 13
#define L_DIO1 14
#define LED_PIN 35             // white onboard LED

#if STRIP_LEN > 0
#include <Adafruit_NeoPixel.h>
Adafruit_NeoPixel strip(STRIP_LEN, STRIP_PIN, NEO_GRB + NEO_KHZ800);
#endif

struct __attribute__((packed)) BeatPacket { uint8_t m0, m1; uint8_t seq; uint16_t bpm10; uint8_t beat; uint8_t hue; };

// ================= SX1262 (RX) =================
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

bool loraInit() {
  pinMode(L_RST, OUTPUT); digitalWrite(L_RST, LOW); delay(10); digitalWrite(L_RST, HIGH); delay(20); waitBusy(1000);
  cmd({0x80, 0x00});                                   // standby RC
  if (readReg(0x0740) != 0x14) { Serial.println("SX1262 not responding"); return false; }
  cmd({0x97, 0x02, 0x00, 0x01, 0x40});                 // DIO3 -> TCXO 1.8 V, 5 ms
  cmd({0x89, 0x7F}); delay(10); waitBusy();            // calibrate all (needed after TCXO)
  cmd({0x96, 0x01});                                   // DC-DC
  cmd({0x9D, 0x01});                                   // DIO2 = RF switch
  cmd({0x8A, 0x01});                                   // LoRa
  cmd({0x98, 0xE1, 0xE9}); delay(5);                   // image cal 902-928
  uint32_t f = (uint32_t)(((uint64_t)LINK_FREQ_HZ << 25) / 32000000ULL);
  cmd({0x86, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f});
  cmd({0x8B, 0x07, 0x06, 0x01, 0x00});                 // SF7, BW500, CR4/5 (matches conductor)
  cmd({0x8C, 0x00, 0x08, 0x00, 0xFF, 0x01, 0x00});     // preamble 8, explicit, max len, CRC on, IQ normal
  cmd({0x8F, 0x00, 0x00});                             // buffer base
  writeReg(0x08AC, 0x96);                              // RX boosted gain
  cmd({0x08, 0x02, 0x42, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00}); // IRQ: RxDone|CrcErr|HdrErr, DIO1 = RxDone
  cmd({0x02, 0xFF, 0xFF});                             // clear IRQs
  cmd({0x82, 0xFF, 0xFF, 0xFF});                       // RX continuous
  Serial.println("Listening on 916.0 MHz, SF7/BW500");
  return true;
}

// returns payload length, or -1 if nothing / bad CRC
int loraRead(uint8_t* out, int maxLen, int& rssi, float& snr) {
  if (!digitalRead(L_DIO1)) return -1;
  uint8_t t[4] = {0x12, 0, 0, 0}, r[4]; loraXfer(t, 4, r);
  uint16_t irq = (r[2] << 8) | r[3];
  cmd({0x02, 0xFF, 0xFF});
  if (!(irq & 0x0002) || (irq & 0x0040)) return -1;
  uint8_t s[4] = {0x13, 0, 0, 0}, rs[4]; loraXfer(s, 4, rs);
  int len = rs[2], start = rs[3]; if (len > maxLen) len = maxLen;
  uint8_t b[3 + 32] = {0x1E, (uint8_t)start, 0}, rb[3 + 32]; loraXfer(b, 3 + len, rb);
  memcpy(out, rb + 3, len);
  uint8_t p[5] = {0x14, 0, 0, 0, 0}, rp[5]; loraXfer(p, 5, rp);
  rssi = -(int)rp[2] / 2; snr = (int8_t)rp[3] / 4.0f;
  return len;
}

// ================= state =================
float bpm = 0; int beatPos = 0; uint8_t hue = 0, lastSeq = 0;
uint32_t lastRx = 0, nextFly = 0, flashAt = 0, pkts = 0, lost = 0;
int lastRssi = 0; float lastSnr = 0; bool flashing = false, gotAny = false, flyBeat = false;

void hueToRgb(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6;
  uint8_t q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}

void flashOn(bool downbeat) {
  screenBeat((uint16_t)(bpm * 10 + 0.5f), beatPos, millis());   // the screen task flashes the OLED
  analogWrite(LED_PIN, downbeat ? 255 : 120);
#if STRIP_LEN > 0
  uint8_t r, g, b; hueToRgb(hue, r, g, b);
  float k = downbeat ? 1.0f : 0.55f;
  for (int i = 0; i < STRIP_LEN; i++) strip.setPixelColor(i, r * k, g * k, b * k);
  strip.show();
#endif
  flashing = true; flashAt = millis();
}
void flashDecay() {
  if (!flashing) return;
  uint32_t dt = millis() - flashAt;
  int lv = 255 - (int)dt * 2; if (lv < 0) lv = 0;
  analogWrite(LED_PIN, lv * (beatPos == 0 ? 1 : 0.5));
#if STRIP_LEN > 0
  uint8_t r, g, b; hueToRgb(hue, r, g, b); float k = lv / 255.0f * (beatPos == 0 ? 1.0f : 0.55f);
  for (int i = 0; i < STRIP_LEN; i++) strip.setPixelColor(i, r * k, g * k, b * k);
  strip.show();
#endif
  if (dt > 130) flashing = false;
}

void updateScreen(uint32_t now) {
  uint32_t since = now - lastRx;
  screenLink(!gotAny ? OL_WAIT : since < 1500 ? OL_LIVE : since < FLYWHEEL_MS ? OL_HOLD : OL_WAIT);
  screenStats(gotAny ? lastRssi : 0, (int8_t)(lastSnr * 4), pkts, lost, 0, 0);
}

void setup() {
  Serial.begin(115200); delay(300);
  pinMode(LED_PIN, OUTPUT); analogWrite(LED_PIN, 0);
  pinMode(L_NSS, OUTPUT); digitalWrite(L_NSS, HIGH);
  pinMode(L_BUSY, INPUT); pinMode(L_DIO1, INPUT);
  Serial.printf("Beat Follower v%s | OLED %s\n", FW_VERSION, screenBegin(false) ? "found" : "not found");
  SPI.begin(L_SCK, L_MISO, L_MOSI, L_NSS);
#if STRIP_LEN > 0
  strip.begin(); strip.show();
#endif
  if (!loraInit()) { while (1) { analogWrite(LED_PIN, 200); delay(120); analogWrite(LED_PIN, 0); delay(120); } }
}

void loop() {
  uint8_t buf[32]; int rssi; float snr;
  int n = loraRead(buf, sizeof(buf), rssi, snr);
  uint32_t now = millis();
  if (n == (int)sizeof(BeatPacket) && buf[0] == 'B' && buf[1] == 'T') {
    BeatPacket p; memcpy(&p, buf, sizeof(p));
    if (gotAny) { uint8_t gap = p.seq - lastSeq; if (gap > 1 && gap < 64) lost += gap - 1; }
    lastSeq = p.seq; gotAny = true; pkts++;
    bpm = p.bpm10 / 10.0f; beatPos = p.beat & 3; hue = p.hue;
    lastRssi = rssi; lastSnr = snr; lastRx = now;
    flashOn(beatPos == 0);
    if (bpm > 30) nextFly = now + (uint32_t)(60000.0f / bpm) + 40; // flywheel waits a bit past the expected packet
  } else if (gotAny && bpm > 30 && now - lastRx < FLYWHEEL_MS && (int32_t)(now - nextFly) >= 0) {
    // packet missed: keep the beat going on our own
    uint32_t period = (uint32_t)(60000.0f / bpm);
    beatPos = (beatPos + 1) & 3;
    flashOn(beatPos == 0);
    nextFly += period;
  }
  flashDecay();
  updateScreen(now);
  screenButton();
  static uint32_t lastLog = 0;
  if (now - lastLog > 2000) {
    lastLog = now;
    Serial.printf("bpm %.1f  beat %d  rssi %d  snr %.1f  pkts %lu  lost %lu  %s\n", bpm, beatPos, lastRssi, lastSnr,
                  (unsigned long)pkts, (unsigned long)lost, gotAny ? (now - lastRx < 1500 ? "live" : "hold") : "waiting");
  }
}
