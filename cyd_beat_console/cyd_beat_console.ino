// CYD Beat Console: a touchscreen booth display for Beat Commander.
// Board: ESP32-2432S028R "Cheap Yellow Display" (ESP32, 2.8" 320x240 ILI9341, XPT2046 resistive touch).
//
// A Heltec V3 running heltec_v3_espnow_bridge listens to the beat link (916.0 MHz LoRa) and relays every
// beat here over ESP-NOW. The console shows the BPM huge, flashes in the conductor's colour on every beat,
// and tracks RSSI / SNR / losses. TAKE hands the beat to the booth: the console keeps time itself (tap tempo,
// +/- 0.1 BPM, 10 ms nudges) and the bridge transmits each beat on LoRa, so every follower keeps flashing.
//
// Touch: TAP = tap tempo (first tap = downbeat; tapping while following takes over).
//        -/+ = 0.1 BPM, hold for 1 BPM steps. Arrows = nudge the beat 10 ms earlier / later.
//        TAKE / RELEASE = take the beat from the conductor, or hand it back.
//        Hold the header 2 s = rotate 180 deg (remembered). Touch the splash at boot = calibrate touch.
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif (core 2.x or 3.x). Board "ESP32 Dev Module". No extra libraries.

#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "link.h"
#include "ui.h"

struct Snap { UiState s; uint32_t flashSeq, flashMs, nextExpect; bool flipReq; };   // UI snapshot of the beat engine
struct Cal { uint8_t swap; float ax, bx, ay, by; };                                // touch calibration

// ---------- options ----------
#define PANEL_ST7789   0     // 1 for the "2-USB" CYD variant with an ST7789 panel
#define PANEL_BGR      0     // default colour order (changeable on the device: display check)
#define PANEL_MIRROR   1     // default mirroring (changeable on the device: display check)
#define PANEL_INVERT   PANEL_ST7789   // set 1 if colours look like a photo negative
#define SPI_HZ         40000000
#define BACKLIGHT      255   // 0..255
#define LINK_AIR_MS    11    // during takeover, flash the console this long after sending: matches the followers
#define FLYWHEEL_MS    4000  // keep the beat going this long after the last packet (same as the followers)
#define DEBUG_TOUCH    0     // 1 = print raw touch values on serial

// ---------- CYD pins ----------
#define TFT_SCK 14
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_CS 15
#define TFT_DC 2
#define TFT_BL 21
#define TP_CLK 25
#define TP_MOSI 32
#define TP_MISO 39
#define TP_CS 33
#define TP_IRQ 36
#define LED_R 4
#define LED_G 16
#define LED_B 17

// ================= display =================
SPIClass tftSPI(HSPI);
SPISettings tftSet(SPI_HZ, MSBFIRST, SPI_MODE0);
bool flipped = false;
uint8_t panelMode = (PANEL_MIRROR ? 1 : 0) | (PANEL_BGR ? 2 : 0);   // bit0 mirror, bit1 BGR; saved as "pmode"
#define BOOT_BTN 0

void tftCmd(uint8_t c) { digitalWrite(TFT_DC, LOW); tftSPI.transfer(c); digitalWrite(TFT_DC, HIGH); }
void tftCmdData(uint8_t c, const uint8_t* d, int n) {
  tftSPI.beginTransaction(tftSet); digitalWrite(TFT_CS, LOW);
  tftCmd(c); for (int i = 0; i < n; i++) tftSPI.transfer(d[i]);
  digitalWrite(TFT_CS, HIGH); tftSPI.endTransaction();
}
uint8_t madctl() {
  uint8_t order = (panelMode & 2) ? 0x08 : 0x00;
#if PANEL_ST7789
  return (flipped ? 0xA0 : 0x60) | order;
#else
  uint8_t m = flipped ? 0xE0 : 0x20;                  // MV (landscape), MX|MY when rotated 180
  if (panelMode & 1) m ^= 0x40;                        // MX
  return m | order;
#endif
}
void tftRotate() { uint8_t m = madctl(); tftCmdData(0x36, &m, 1); }
void tftInit() {
  pinMode(TFT_CS, OUTPUT); digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT); digitalWrite(TFT_DC, HIGH);
  tftSPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tftCmdData(0x01, nullptr, 0); delay(150);            // software reset
  tftCmdData(0x11, nullptr, 0); delay(120);            // sleep out
  uint8_t px = 0x55; tftCmdData(0x3A, &px, 1);         // 16-bit colour
  tftRotate();
  tftCmdData(PANEL_INVERT ? 0x21 : 0x20, nullptr, 0);
  tftCmdData(0x13, nullptr, 0);                        // normal mode
  tftCmdData(0x29, nullptr, 0); delay(20);             // display on
}
void panelPushBand() {
  for (int i = 0; i < SCR_W * band.h; i++) { uint16_t c = band.px[i]; band.px[i] = (c >> 8) | (c << 8); }
  tftSPI.beginTransaction(tftSet); digitalWrite(TFT_CS, LOW);
  tftCmd(0x2A); tftSPI.transfer16(0); tftSPI.transfer16(SCR_W - 1);
  tftCmd(0x2B); tftSPI.transfer16(band.y0); tftSPI.transfer16(band.y0 + band.h - 1);
  tftCmd(0x2C);
  tftSPI.writeBytes((const uint8_t*)band.px, SCR_W * band.h * 2);
  digitalWrite(TFT_CS, HIGH); tftSPI.endTransaction();
}

// ================= touch (XPT2046, bit-banged) =================
Cal cal = {0, 320.0f / 3500, -200.0f * 320 / 3500, 240.0f / 3560, -240.0f * 240 / 3560};
Preferences prefs;

uint16_t tpRead(uint8_t c) {
  for (int i = 7; i >= 0; i--) { digitalWrite(TP_MOSI, (c >> i) & 1); digitalWrite(TP_CLK, HIGH); digitalWrite(TP_CLK, LOW); }
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) { digitalWrite(TP_CLK, HIGH); v = (v << 1) | digitalRead(TP_MISO); digitalWrite(TP_CLK, LOW); }
  return (v >> 3) & 0x0FFF;
}
// raw touch: a = X channel, b = Y channel. Returns false if not pressed.
bool tpRaw(int& a, int& b) {
  if (digitalRead(TP_IRQ)) return false;
  digitalWrite(TP_CS, LOW);
  int z = tpRead(0xB0) + 4095 - tpRead(0xC0);
  bool down = z > 400;
  int sa = 0, sb = 0;
  if (down) {
    tpRead(0x90);                                      // first sample is noisy: discard
    for (int i = 0; i < 4; i++) { sa += tpRead(0x90); sb += tpRead(0xD0); }
  }
  tpRead(0x80);                                        // power down, pen IRQ on
  digitalWrite(TP_CS, HIGH);
  a = sa / 4; b = sb / 4;
  return down;
}
inline void toLogical(int& x, int& y) { if (flipped) { x = SCR_W - 1 - x; y = SCR_H - 1 - y; } }
bool tpPoint(int& x, int& y) {
  int a, b; if (!tpRaw(a, b)) return false;
  int u = cal.swap ? b : a, v = cal.swap ? a : b;
  x = (int)(cal.ax * u + cal.bx); y = (int)(cal.ay * v + cal.by);
  x = constrain(x, 0, SCR_W - 1); y = constrain(y, 0, SCR_H - 1);
  toLogical(x, y);
#if DEBUG_TOUCH
  Serial.printf("# touch raw %d,%d -> %d,%d\n", a, b, x, y);
#endif
  return true;
}
void tpInit() {
  pinMode(TP_CLK, OUTPUT); pinMode(TP_MOSI, OUTPUT); pinMode(TP_CS, OUTPUT); digitalWrite(TP_CS, HIGH);
  pinMode(TP_MISO, INPUT); pinMode(TP_IRQ, INPUT);
  digitalWrite(TP_CS, LOW); tpRead(0x80); digitalWrite(TP_CS, HIGH);   // enable pen IRQ
}

// wait for one press, return its averaged raw values
void calSample(int& a, int& b) {
  int ra, rb;
  while (tpRaw(ra, rb)) delay(10);                     // wait for release first
  delay(150);
  while (!tpRaw(ra, rb)) delay(5);
  long sa = 0, sb = 0; int n = 0; uint32_t t = millis();
  while (millis() - t < 400) { if (tpRaw(ra, rb)) { sa += ra; sb += rb; n++; } delay(8); }
  a = n ? sa / n : ra; b = n ? sb / n : rb;
  while (tpRaw(ra, rb)) delay(10);
}
void calibrate() {
  const int px[3][2] = {{20, 20}, {300, 20}, {20, 220}};
  int ra[3], rb[3];
  for (int i = 0; i < 3; i++) { drawCal(i, px[i][0], px[i][1]); calSample(ra[i], rb[i]); }
  int pp[3][2];                                        // targets in unrotated panel coordinates
  for (int i = 0; i < 3; i++) { pp[i][0] = px[i][0]; pp[i][1] = px[i][1]; toLogical(pp[i][0], pp[i][1]); }
  Cal c; c.swap = abs(rb[1] - rb[0]) > abs(ra[1] - ra[0]);
  int u0 = c.swap ? rb[0] : ra[0], u1 = c.swap ? rb[1] : ra[1];
  int v0 = c.swap ? ra[0] : rb[0], v2 = c.swap ? ra[2] : rb[2];
  if (abs(u1 - u0) < 300 || abs(v2 - v0) < 300) { Serial.println("# calibration rejected, keeping the old one"); return; }
  c.ax = (float)(pp[1][0] - pp[0][0]) / (u1 - u0); c.bx = pp[0][0] - c.ax * u0;
  c.ay = (float)(pp[2][1] - pp[0][1]) / (v2 - v0); c.by = pp[0][1] - c.ay * v0;
  cal = c; prefs.putBytes("cal2", &cal, sizeof(cal));
  Serial.printf("# calibration saved: swap %d  x = %.4f*u%+.1f  y = %.4f*v%+.1f\n", c.swap, c.ax, c.bx, c.ay, c.by);
}

// ================= RGB LED (active LOW) =================
uint8_t ledLast[3] = {1, 1, 1};
void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  if (r == ledLast[0] && g == ledLast[1] && b == ledLast[2]) return;
  ledLast[0] = r; ledLast[1] = g; ledLast[2] = b;
  analogWrite(LED_R, 255 - r); analogWrite(LED_G, 255 - g); analogWrite(LED_B, 255 - b);
}
void hue8(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6, q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}

// ================= beat engine (realtime task) =================
struct Core {
  uint8_t mode = M_FOLLOW;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, lastFireMs = 0;
  int rssi = 0; float snr = 0; int nudgeMs = 0;
  uint32_t bridgeMs = 0, conflictUntil = 0; bool radioOk = false, bridgeSeen = false;
  bool viaBooth = false;             // the beat comes from a booth controller (StickS3, Dial, Tab5...) not a LoRa conductor
  uint32_t flashSeq = 0, flashMs = 0; bool flashDown = false;
  int pressed = -1; bool flipReq = false;
  uint32_t tapTimes[5]; int tapCount = 0;
  float frac = 0;                    // sub-millisecond remainder of the beat period, so long runs don't drift
};
Core core;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t rxQ;
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static inline float periodMs(float bpm) { return 60000.0f / (bpm > 30 ? bpm : 120); }
// next whole-ms step of the beat grid, carrying the fraction (call with the lock held)
static uint32_t stepMs() { float a = core.frac + periodMs(core.bpm); uint32_t s = (uint32_t)a; core.frac = a - s; return s; }

// all below run inside the beat task with mux held
void beatAt(uint32_t t, bool down) { core.flashSeq++; core.flashMs = t; core.flashDown = down; core.lastBeatMs = t; }

void takeOver(uint32_t now) {
  if (core.mode == M_TAKE) return;
  core.mode = M_TAKE; core.fires = core.acks = 0; core.nudgeMs = 0; core.tapCount = 0;
  if (core.bpm < 60 || core.bpm > 200) core.bpm = 120;
  float p = periodMs(core.bpm);
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {     // seamless: continue the current beat grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
}
void release(uint32_t now) {
  if (core.mode != M_TAKE) return;
  core.mode = M_FOLLOW; core.gotAny = true;
  core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;  // HOLD: flywheel on until the W10 is back
  core.conflictUntil = 0;
}
void tap(uint32_t now) {
  takeOver(now);
  if (core.tapCount && now - core.tapTimes[core.tapCount - 1] > 2000) core.tapCount = 0;
  if (core.tapCount == 5) { memmove(core.tapTimes, core.tapTimes + 1, 4 * sizeof(uint32_t)); core.tapCount = 4; }
  core.tapTimes[core.tapCount++] = now;
  if (core.tapCount >= 2) {
    float avg = (float)(core.tapTimes[core.tapCount - 1] - core.tapTimes[0]) / (core.tapCount - 1);
    core.bpm = constrain(60000.0f / avg, 60.0f, 200.0f);
  }
  if (core.tapCount == 1) core.beatPos = 3;               // first tap = downbeat
  core.nextBeat = now;                                     // fire on the tap
  core.nudgeMs = 0;
}

void onFrame(const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  LinkFrame f; memcpy(&f, d, sizeof(f));
  if (f.type != LF_FIRE) xQueueSend(rxQ, &f, 0);
}

void handleFrame(const LinkFrame& f, uint32_t now) {
  core.bridgeMs = now; core.bridgeSeen = true;
  if (f.type == LF_HELLO) { core.radioOk = f.flags & 1; return; }
  if (f.type == LF_ACK) { core.acks++; return; }
  if (f.type != LF_RELAY) return;
  core.radioOk = true;
  if (core.gotAny) { uint8_t gap = f.seq - core.lastSeq; if (gap > 1 && gap < 64) core.lost += gap - 1; }
  bool echo = f.flags & LINK_FLAG_ECHO;                // a booth controller mirroring the bridge's confirmation
  if (core.gotAny && f.seq == core.lastSeq && now - core.lastRxMs < 250) return;   // same beat twice
  core.lastSeq = f.seq; core.rx++;
  if (!echo) { core.rssi = f.rssi; core.snr = f.snr4 / 4.0f; }
  core.viaBooth = echo;
  if (core.mode == M_TAKE) { core.conflictUntil = now + 1500; return; }   // someone else is transmitting
  core.gotAny = true; core.lastRxMs = now;
  core.bpm = f.bpm10 / 10.0f; core.beatPos = f.beat & 3; core.hue = f.hue;
  core.flyNext = now + (uint32_t)periodMs(core.bpm) + 40;
  beatAt(now, core.beatPos == 0);
}

// called with the lock held; the touch panel was read just before, outside the lock
void touchStep(uint32_t now, bool down, int x, int y) {
  static bool was = false; static uint32_t downAt = 0, nextRep = 0; static int downBtn = -1; static bool headHold = false;
  if (down && !was) {
    downAt = now; headHold = y < H_HEAD; downBtn = hitButton(x, y);
    UiState tmp; tmp.mode = core.mode;
    if (downBtn >= 0 && !btnEnabled(tmp, downBtn)) downBtn = -1;
    core.pressed = downBtn; nextRep = now + 500;
    switch (downBtn) {
      case B_TAP:   tap(now); break;
      case B_MINUS: core.bpm = constrain(core.bpm - 0.1f, 60.0f, 200.0f); break;
      case B_PLUS:  core.bpm = constrain(core.bpm + 0.1f, 60.0f, 200.0f); break;
      case B_LEFT:  core.nextBeat -= 10; core.nudgeMs -= 10; break;
      case B_RIGHT: core.nextBeat += 10; core.nudgeMs += 10; break;
      case B_TAKE:  if (core.mode == M_TAKE) release(now); else takeOver(now); break;
    }
  } else if (down && was) {
    if ((downBtn == B_MINUS || downBtn == B_PLUS) && (int32_t)(now - nextRep) >= 0) {
      core.bpm = constrain(roundf(core.bpm) + (downBtn == B_PLUS ? 1.0f : -1.0f), 60.0f, 200.0f);
      nextRep = now + 150;
    }
    if (headHold && y < H_HEAD + 20 && now - downAt > 2000) { core.flipReq = true; headHold = false; }
  } else if (!down && was) { core.pressed = -1; headHold = false; }
  was = down;
}

void beatTask(void*) {
  uint8_t lr = 0, lg = 0, lb = 0;
  uint32_t lastTouch = 0;
  for (;;) {
    uint32_t now = millis();
    LinkFrame in[8]; int nIn = 0;
    while (nIn < 8 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;
    bool tDown = false, tRead = false; int tx = 0, ty = 0;
    if (now - lastTouch >= 4) { lastTouch = now; tRead = true; tDown = tpPoint(tx, ty); }
    bool fire = false; LinkFrame fr;

    portENTER_CRITICAL(&mux);
    for (int i = 0; i < nIn; i++) handleFrame(in[i], now);
    if (tRead) touchStep(now, tDown, tx, ty);
    if (core.mode == M_TAKE) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        float p = periodMs(core.bpm);
        if (now - core.nextBeat > p) core.nextBeat = now;   // fell far behind: resync
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;            // new colour every bar, like the W10
        core.nextBeat += stepMs();
        core.fires++; core.lastFireMs = now;
        fr = linkFrame(LF_FIRE);
        fr.bpm10 = (uint16_t)(core.bpm * 10 + 0.5f); fr.beat = core.beatPos; fr.hue = core.hue;
        fire = true;
        beatAt(now + LINK_AIR_MS, core.beatPos == 0);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS &&
               (int32_t)(now - core.flyNext) >= 0) {   // packet missed: keep the beat going
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0);
      core.flyNext += stepMs();
    }
    // LED level from the latest flash
    uint32_t fm = core.flashMs; bool fd = core.flashDown; uint8_t h = core.hue;
    portEXIT_CRITICAL(&mux);
    if (fire) esp_now_send(BCAST, (const uint8_t*)&fr, sizeof(fr));

    int lv = 0;
    if (fm && (int32_t)(now - fm) >= 0) { lv = 255 - (int)(now - fm) * 2; if (lv < 0) lv = 0; if (!fd) lv = lv * 55 / 100; }
    hue8(h, lr, lg, lb);
    ledSet(lr * lv / 255, lg * lv / 255, lb * lv / 255);
    vTaskDelay(1);
  }
}

// ================= UI (Arduino loop, core 1) =================
void snapshot(Snap& n, uint32_t now) {
  portENTER_CRITICAL(&mux);
  UiState& s = n.s;
  s.mode = core.mode;
  s.bridgeOk = core.bridgeSeen && now - core.bridgeMs < 3000;
  s.radioOk = core.radioOk;
  s.conflict = core.mode == M_TAKE && (int32_t)(core.conflictUntil - now) > 0;
  if (core.mode == M_TAKE) s.link = LS_TAKE;
  else if (!core.gotAny) s.link = LS_WAIT;
  else if (now - core.lastRxMs < 1500) s.link = LS_LIVE;
  else if (now - core.lastRxMs < FLYWHEEL_MS) s.link = LS_HOLD;
  else s.link = LS_WAIT;
  s.bpm = core.bpm; s.beatPos = core.beatPos; s.hue = core.hue;
  s.rssi = core.rssi; s.snr = core.snr; s.rx = core.rx; s.lost = core.lost; s.viaBooth = core.viaBooth;
  s.sent = core.acks;
  uint32_t pend = (core.fires > core.acks && now - core.lastFireMs < 80) ? 1 : 0;
  s.fail = core.fires - core.acks - pend;
  s.nudgeMs = core.nudgeMs; s.pressed = core.pressed; s.downbeat = core.flashDown;
  n.flashSeq = core.flashSeq; n.flashMs = core.flashMs;
  n.nextExpect = core.mode == M_TAKE ? core.nextBeat + LINK_AIR_MS : core.lastBeatMs + (uint32_t)periodMs(core.bpm);
  n.flipReq = core.flipReq; core.flipReq = false;
  portEXIT_CRITICAL(&mux);
}

bool linkInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) { onFrame(d, len); });
#else
  esp_now_register_recv_cb([](const uint8_t*, const uint8_t* d, int len) { onFrame(d, len); });
#endif
  return true;
}

// ---- display check: pick mirroring + colour order on the device (one .bin fits every CYD) ----
void drawDisplayCheck() {
  paint(0, SCR_H, [] {
    fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
    textC(ORB_14, "DISPLAY CHECK", SCR_W / 2, 18, C_AMB);
    text(STM_16, "<< THIS SIDE IS LEFT", 12, 50, C_WHITE);
    static const char* name[3] = {"AMBER", "RED", "BLUE"};
    const uint16_t col[3] = {C_AMB, rgb(255, 0, 0), rgb(0, 60, 255)};
    for (int i = 0; i < 3; i++) {
      int x = 14 + i * 100;
      fillRect(x, 82, 92, 60, col[i]);
      textC(STM_16, name[i], x + 46, 150, C_WHITE);
    }
    char m[24]; snprintf(m, sizeof(m), "setting %d of 4", panelMode + 1);
    textC(STM_12, m, SCR_W / 2, 182, C_GREY);
    textC(STM_12, "text reads right + colours match?", SCR_W / 2, 202, C_GRN);
    textC(STM_12, "no: tap BOOT    yes: hold BOOT 2 s", SCR_W / 2, 220, C_GRN);
  });
}
void displayCheck() {
  Serial.println("# display check: tap BOOT = next setting, hold BOOT 2 s = save");
  while (digitalRead(BOOT_BTN) == LOW) delay(10);          // wait for release of the press that opened it
  drawDisplayCheck();
  while (true) {
    if (digitalRead(BOOT_BTN) == LOW) {
      uint32_t t0 = millis();
      while (digitalRead(BOOT_BTN) == LOW && millis() - t0 < 2000) delay(10);
      if (millis() - t0 >= 2000) {                         // hold = save
        prefs.putUChar("pmode", panelMode);
        Serial.printf("# display setting %d saved\n", panelMode + 1);
        paint(0, SCR_H, [] { fillRect(0, 0, SCR_W, SCR_H, C_BLACK); textC(ORB_14, "SAVED", SCR_W / 2, 110, C_GRN); });
        while (digitalRead(BOOT_BTN) == LOW) delay(10);
        delay(600);
        return;
      }
      if (millis() - t0 > 40) {                            // tap = next setting
        panelMode = (panelMode + 1) & 3; tftRotate(); drawDisplayCheck();
        Serial.printf("# display setting %d\n", panelMode + 1);
      }
    }
    delay(10);
  }
}

void setup() {
  Serial.begin(115200); delay(200);
  Serial.println("# CYD Beat Console " FW_VERSION);
  pinMode(LED_R, OUTPUT); pinMode(LED_G, OUTPUT); pinMode(LED_B, OUTPUT); ledSet(0, 0, 0);
  pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, LOW);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  prefs.begin("beatcon", false);
  flipped = prefs.getBool("flip", false);
  bool havePanel = prefs.isKey("pmode");
  if (havePanel) panelMode = prefs.getUChar("pmode", panelMode) & 3;
  bool haveCal = prefs.getBytesLength("cal2") == sizeof(Cal);
  if (haveCal) prefs.getBytes("cal2", &cal, sizeof(cal));
  tftInit(); tpInit();
  paint(0, SCR_H, [] { fillRect(0, 0, SCR_W, SCR_H, C_BLACK); });
  analogWrite(TFT_BL, BACKLIGHT);

  if (!havePanel) displayCheck();                      // first boot: make sure the panel looks right
  for (int i = 1; i <= 12; i++) drawSplash(i / 12.0f);  // CRT power-on
  uint32_t t = millis(); bool doCal = false; int x, y;
  while (millis() - t < 1500) {
    if (tpPoint(x, y)) { doCal = true; break; }
    if (digitalRead(BOOT_BTN) == LOW) { displayCheck(); for (int i = 1; i <= 12; i++) drawSplash(i / 12.0f); t = millis(); }
    delay(10);
  }
  if (doCal || !haveCal) calibrate();               // first boot always calibrates

  rxQ = xQueueCreate(16, sizeof(LinkFrame));
  if (!linkInit()) Serial.println("# ESP-NOW init failed");
  Serial.printf("# ESP-NOW ch %d, net 0x%02X | MAC %s\n", LINK_ESPNOW_CH, LINK_NET_ID, WiFi.macAddress().c_str());
  xTaskCreatePinnedToCore(beatTask, "beat", 4096, nullptr, 5, nullptr, 0);
  UiState s0; drawAll(s0);
}

void loop() {
  static UiState last; static bool first = true;
  static uint32_t seenSeq = 0, animStart = 0, lastStats = 0; static bool animating = false;
  uint32_t now = millis();
  Snap n; snapshot(n, now);
  UiState& s = n.s;

  if (n.flipReq) {
    flipped = !flipped; prefs.putBool("flip", flipped); tftRotate(); first = true;
  }
  // flash animation
  if (n.flashSeq != seenSeq && (int32_t)(now - n.flashMs) >= 0) { seenSeq = n.flashSeq; animStart = n.flashMs; animating = true; }
  s.flash = 0;
  if (animating) { int dt = now - animStart; s.flash = dt >= 150 ? 0 : 1.0f - dt / 150.0f; }

  bool full = first || s.link != last.link || s.mode != last.mode || s.bridgeOk != last.bridgeOk || s.viaBooth != last.viaBooth ||
              ((s.link == LS_WAIT) && s.radioOk != last.radioOk);
  if (full) {
    drawAll(s); first = false; lastStats = now;
  } else {
    // the stage first: it carries the beat
    bool stageDirty = animating || s.conflict != last.conflict || (int)(s.bpm * 10) != (int)(last.bpm * 10) || s.hue != last.hue;
    if (stageDirty) paint(Y_STAGE, H_STAGE, [&] { drawStage(s); });
    if (animating && s.flash <= 0) animating = false;
    if (s.beatPos != last.beatPos) paint(Y_BAR, H_BAR, [&] { drawBar(s); });
    // other regions only when no beat is about to land
    bool quiet = (int32_t)(n.nextExpect - millis()) > 25 || (int32_t)(n.nextExpect - millis()) < -150;
    if (quiet) {
      if (s.pressed != last.pressed) paint(Y_BTNS, H_BTNS, [&] { drawButtons(s); });
      if (now - lastStats > 250 && (s.rx != last.rx || s.lost != last.lost || s.sent != last.sent || s.fail != last.fail ||
                                    s.nudgeMs != last.nudgeMs || s.rssi != last.rssi)) {
        paint(Y_STATS, H_STATS, [&] { drawStats(s); }); lastStats = now;
      } else { s.rx = last.rx; s.lost = last.lost; s.sent = last.sent; s.fail = last.fail; s.nudgeMs = last.nudgeMs; s.rssi = last.rssi; s.snr = last.snr; }
    } else { s.pressed = last.pressed; s.rx = last.rx; s.lost = last.lost; s.sent = last.sent; s.fail = last.fail;
             s.nudgeMs = last.nudgeMs; s.rssi = last.rssi; s.snr = last.snr; }
  }
  last = s;

  static uint32_t lastLog = 0;
  if (now - lastLog > 2000) {
    lastLog = now;
    static const char* LS[] = {"wait", "live", "hold", "take"};
    Serial.printf("bpm %.1f beat %d %s bridge %s rssi %d snr %.1f rx %lu lost %lu sent %lu fail %lu\n", s.bpm, s.beatPos,
                  LS[s.link], s.bridgeOk ? "ok" : "MISSING", s.rssi, s.snr, (unsigned long)s.rx, (unsigned long)s.lost,
                  (unsigned long)s.sent, (unsigned long)s.fail);
  }
  delay(animating ? 1 : 5);
}
