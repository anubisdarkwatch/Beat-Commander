// Beat Commander beat badge for the Piglet (Seeed XIAO ESP32-C5 + 128x64 SSD1306 OLED + one button).
// Follows the beat link over ESP-NOW (channel 1, via the Heltec bridge) and shows the BPM, the bar and every beat;
// beat 1 flashes the whole screen. A stats page shows the bridge's signal where you stand, for checking coverage.
// It can also take command of the beat in a pinch.
//
// Button (the Piglet's button, GPIO0)
//   press              following: switch page (BEAT / STATS)     in TAKE: tap tempo
//   hold 2 s           take over / release (a bar under the header fills while you hold)
//
// Built by Anubis Darkwatch with Claude. MIT license. Replaces the Piglet wardriver firmware; reflash Piglet to go back
// (your SD card and wardriver.cfg are not touched).
// Arduino IDE 2.x, board package "M5Stack" 3.3.x, board "M5StampC5" (same ESP32-C5 chip as the XIAO C5).
// Library: M5GFX.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <M5GFX.h>
#include <lgfx/v1/panel/Panel_SSD1306.hpp>
#include "link.h"
#include "ui.h"

#define FW_VERSION   "1.0"
#define PIN_SDA      23
#define PIN_SCL      24
#define PIN_BTN      0      // shared with BOOT: ignored for the first 1.5 s after power-up
#define LINK_AIR_MS  11
#define FLYWHEEL_MS  4000
#define HOLD_MS      2000

// ================= OLED =================
class LGFX_OLED : public lgfx::LGFX_Device {
  lgfx::Panel_SSD1306 _panel; lgfx::Bus_I2C _bus;
public:
  void setup(uint8_t addr) {
    auto b = _bus.config(); b.i2c_port = 0; b.freq_write = 400000; b.freq_read = 400000;
    b.pin_sda = PIN_SDA; b.pin_scl = PIN_SCL; b.i2c_addr = addr; _bus.config(b); _panel.setBus(&_bus);
    auto p = _panel.config(); p.panel_width = 128; p.panel_height = 64; p.memory_width = 128; p.memory_height = 64;
    _panel.config(p); setPanel(&_panel);
  }
};
LGFX_OLED oled;
LGFX_Sprite cv(&oled);

// ================= ESP-NOW =================
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
struct Rx { LinkFrame f; int8_t sig; };
QueueHandle_t rxQ;
bool radioUp = false;

bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
  esp_now_register_recv_cb([](const esp_now_recv_info_t* info, const uint8_t* d, int len) {
    if (!linkValid(d, len)) return;
    Rx r; memcpy(&r.f, d, sizeof(LinkFrame)); r.sig = info && info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    xQueueSend(rxQ, &r, 0);
  });
  return true;
}

// ================= state =================
struct Core {
  bool take = false;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, bars = 0;
  int rssi = 0, sig = 0; uint32_t bridgeMs = 0, conflictUntil = 0; bool bridgeSeen = false;
  uint32_t flashMs = 0; bool flashDown = false, viaBooth = false;
  uint32_t tapTimes[5]; int tapCount = 0; float frac = 0;
  const char* toast = ""; uint32_t toastUntil = 0;
};
Core core;
SemaphoreHandle_t lock;
enum BtnCmd : uint8_t { B_SHORT, B_LONG };
QueueHandle_t btnQ;
uint8_t page = PG_BEAT;

static inline float periodMs(float bpm) { return 60000.0f / (bpm > 30 ? bpm : 120); }
static uint32_t stepMs() { float a = core.frac + periodMs(core.bpm); uint32_t s = (uint32_t)a; core.frac = a - s; return s; }
void beatAt(uint32_t t, bool down, bool booth) { core.flashMs = t; core.flashDown = down; core.lastBeatMs = t; core.viaBooth = booth; if (down) core.bars++; }
void toast(const char* m, uint32_t now) { core.toast = m; core.toastUntil = now + 1200; }

void takeOver(uint32_t now) {
  core.take = true; core.fires = core.acks = 0; core.tapCount = 0;
  if (core.bpm < 60 || core.bpm > 200) core.bpm = 120;
  float p = periodMs(core.bpm);
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {          // continue the current beat grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
  toast("TAKEOVER", now);
}
void release(uint32_t now) {
  core.take = false; core.gotAny = true; core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;
  core.conflictUntil = 0; toast("RELEASED", now);
}
void tapTempo(uint32_t now) {
  if (core.tapCount && now - core.tapTimes[core.tapCount - 1] > 2000) core.tapCount = 0;
  if (core.tapCount == 5) { memmove(core.tapTimes, core.tapTimes + 1, 4 * sizeof(uint32_t)); core.tapCount = 4; }
  core.tapTimes[core.tapCount++] = now;
  if (core.tapCount >= 2) core.bpm = constrain(60000.0f * (core.tapCount - 1) / (core.tapTimes[core.tapCount - 1] - core.tapTimes[0]), 60.0f, 200.0f);
  if (core.tapCount == 1) core.beatPos = 3;
  core.nextBeat = now;
}

bool handleFrame(const Rx& r, uint32_t now) {   // true = mirror this ACK for consoles
  const LinkFrame& f = r.f;
  bool echo = f.flags & LINK_FLAG_ECHO;
  if (f.type == LF_FIRE) { if (core.take) core.conflictUntil = now + 1500; return false; }
  if (!echo && r.sig) core.sig = r.sig;          // signal from the bridge (echoes come from other controllers)
  core.bridgeMs = now; core.bridgeSeen = true;
  if (f.type == LF_HELLO) return false;
  if (core.take) {
    if (f.type == LF_ACK) { core.acks++; return true; }
    if (f.type == LF_RELAY && !echo) core.conflictUntil = now + 1500;
    return false;
  }
  if (f.type != LF_RELAY && f.type != LF_ACK) return false;
  if (core.gotAny && f.seq == core.lastSeq && now - core.lastRxMs < 250) return false;
  bool booth = f.type == LF_ACK || echo;
  if (!booth) core.rssi = f.rssi;
  if (core.gotAny) { uint8_t gap = f.seq - core.lastSeq; if (gap > 1 && gap < 64) core.lost += gap - 1; }
  core.lastSeq = f.seq; core.rx++; core.gotAny = true; core.lastRxMs = now;
  core.bpm = f.bpm10 / 10.0f; core.beatPos = f.beat & 3; core.hue = f.hue;
  core.flyNext = now + (uint32_t)periodMs(core.bpm) + 40; core.frac = 0;
  beatAt(now, core.beatPos == 0, booth);
  return false;
}

void controlTask(void*) {
  for (;;) {
    uint32_t now = millis();
    Rx in[6]; int nIn = 0;
    while (nIn < 6 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;
    uint8_t btn[4]; int nBtn = 0;
    while (nBtn < 4 && xQueueReceive(btnQ, &btn[nBtn], 0) == pdTRUE) nBtn++;
    LinkFrame out[3]; int nOut = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < nIn; i++)
      if (handleFrame(in[i], now) && nOut < 3) {
        LinkFrame e = in[i].f; e.type = LF_RELAY; e.flags = LINK_FLAG_ECHO; e.rssi = 0; e.snr4 = 0; e.count = 0; out[nOut++] = e;
      }
    for (int i = 0; i < nBtn; i++) {
      if (btn[i] == B_LONG) { if (core.take) release(now); else takeOver(now); }
      else if (core.take) tapTempo(now);
      else page = (page + 1) % PG_COUNT;
    }
    if (core.take) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        if (now - core.nextBeat > periodMs(core.bpm)) core.nextBeat = now;
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;
        core.nextBeat += stepMs(); core.fires++;
        LinkFrame fr = linkFrame(LF_FIRE); fr.bpm10 = (uint16_t)(core.bpm * 10 + 0.5f); fr.beat = core.beatPos; fr.hue = core.hue;
        if (nOut < 3) out[nOut++] = fr;
        beatAt(now + LINK_AIR_MS, core.beatPos == 0, true);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS && (int32_t)(now - core.flyNext) >= 0) {
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0, core.viaBooth);
      core.flyNext += stepMs();
    }
    xSemaphoreGive(lock);
    for (int i = 0; i < nOut; i++) esp_now_send(BCAST, (const uint8_t*)&out[i], sizeof(LinkFrame));
    vTaskDelay(1);
  }
}

// ================= button =================
uint32_t settleUntil = 0, downSince = 0; bool down = false, longSent = false;
uint32_t pollButton(uint32_t now) {      // returns how long it's been held (for the progress bar)
  if ((int32_t)(now - settleUntil) < 0) return 0;
  bool pressed = digitalRead(PIN_BTN) == LOW;
  if (pressed && !down) { down = true; downSince = now; longSent = false; }
  if (pressed && down && !longSent && now - downSince >= HOLD_MS) { uint8_t c = B_LONG; xQueueSend(btnQ, &c, 0); longSent = true; }
  if (!pressed && down) {
    down = false;
    if (!longSent && now - downSince > 25) { uint8_t c = B_SHORT; xQueueSend(btnQ, &c, 0); }
  }
  return down && !longSent ? now - downSince : 0;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  settleUntil = millis() + 1500;
  oled.setup(0x3C);
  if (!oled.init()) { oled.setup(0x3D); oled.init(); }
  oled.setRotation(0);
  initFonts();
  cv.setColorDepth(1);
  cv.createSprite(SW, SH);
  lock = xSemaphoreCreateMutex();
  rxQ = xQueueCreate(16, sizeof(Rx));
  btnQ = xQueueCreate(4, 1);
  radioUp = espnowInit();
  Serial.printf("# Beat Commander badge (Piglet C5) v%s | ESP-NOW %s\n", FW_VERSION, radioUp ? "up" : "FAILED");
  xTaskCreate(controlTask, "control", 4096, nullptr, 5, nullptr);
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  uint32_t held = pollButton(now);
  if (now - lastDraw < 30) { delay(2); return; }
  lastDraw = now;
  View v = {};
  xSemaphoreTake(lock, portMAX_DELAY);
  if (!radioUp) v.link = LS_NORADIO;
  else if (core.take) v.link = LS_TAKE;
  else if (!core.gotAny) v.link = LS_WAIT;
  else if (now - core.lastRxMs < 1500) v.link = LS_LIVE;
  else if (now - core.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
  else v.link = LS_WAIT;
  v.page = page; v.viaBooth = core.viaBooth; v.conflict = core.take && (int32_t)(core.conflictUntil - now) > 0;
  v.bridgeOk = core.bridgeSeen && now - core.bridgeMs < 3000;
  v.bpm = core.bpm; v.beatPos = core.beatPos; v.bars = core.bars;
  v.sig = v.bridgeOk ? core.sig : 0; v.rssi = core.rssi; v.rx = core.rx; v.lost = core.lost; v.sent = core.acks;
  v.flashMs = core.flashMs; v.flashDown = core.flashDown; v.holdMs = held;
  v.toast = (int32_t)(core.toastUntil - now) > 0 ? core.toast : "";
  xSemaphoreGive(lock);
  draw(cv, v, now);
  cv.pushSprite(0, 0);
}
