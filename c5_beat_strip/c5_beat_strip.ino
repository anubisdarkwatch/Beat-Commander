// Beat Strip: an ESP32-C5 board drives a WS2812B / SK6812 RGB LED strip on the beat (Beat Commander).
// It follows the beat link over ESP-NOW (channel 1) and lights the strip in the conductor's colour, with a bigger
// hit on beat 1. Patterns: PULSE, CHASE, BURST, BAR (1-2-3-4 sections), SPARKLE, RAINBOW, or AUTO (new one every
// 8 bars). No beat: slow amber breathing. The onboard LED flashes too.
// It also prints the Beat Gateway's serial feed, so Beat Hub can use it for Ableton Link and OSC (no MIDI clock).
//
// Wiring (no soldering if your board has header pins): strip 5V -> 5V / VBUS, GND -> GND, DIN -> the data pin
// (default GPIO1; on a XIAO that's D0). Short strips run off USB; brightness is capped to a USB-safe current.
// Longer strips: their own 5V supply, with its GND joined to the board's GND.
//
// BOOT button: press = next pattern (the first LEDs show its number), hold 1.5 s = AUTO on (green) / off (red),
// hold 4 s = brightness 25 / 50 / 75 / 100 %.
// Serial (any baud, Flash Station's console works): ?  leds <n>  pin <gpio>  bright <1-100>  limit <mA>
//   pattern <name|0-5>  auto on|off  test. All saved.
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, board package M5Stack 3.x (or esp32 by Espressif 3.3+), any ESP32-C5 board (e.g. "M5StampC5"),
// USB CDC On Boot: Enabled. No extra libraries. Build with -DBOARD_XIAO_C5 for the XIAO, nothing for the DevKits.

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <Preferences.h>
#include "link.h"
#include "gateway.h"
#include "strip.h"

#define FW_VERSION "1.0"
#define P_BTN 28                 // BOOT on the XIAO, Espressif DevKitC-1 and Waveshare C5 boards
#define P_ONBOARD 27             // XIAO: yellow user LED (active low). DevKits: WS2812 RGB LED.
#ifdef BOARD_XIAO_C5
#define BOARD_TXT "XIAO ESP32-C5"
#else
#define BOARD_TXT "ESP32-C5 DevKit"
#endif

#define LAT_FIRE_US  1000
#define LAT_LORA_US 10000
#define SRC_HOLD_US 2000000
#define FREE_BEATS  8

Preferences prefs;
uint16_t nLeds = 60; uint8_t dataPin = 1, brightPct = 50, pattern = SP_PULSE; uint16_t limitMa = 900; bool autoMode = false;

// ================= output to both USB serial and the UART (DevKits have a second USB port on the UART) =================
void outf(const char* fmt, ...) {
  char b[160]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
  Serial.print(b); Serial0.print(b);
}

// ================= strip output (RMT, 10 MHz ticks) =================
static rmt_data_t sym[STRIP_MAX * 24];
static uint8_t frame[STRIP_MAX * 3];
bool rmtOk = false;
void stripPinInit() {
  rmtOk = rmtInit(dataPin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  if (!rmtOk) outf("# strip: can't use GPIO%u\n", dataPin);
}
void stripShow(const uint8_t* rgb, int n) {
  if (!rmtOk) return;
  int k = 0;
  for (int i = 0; i < n; i++) {
    uint8_t c[3] = {rgb[i * 3 + 1], rgb[i * 3], rgb[i * 3 + 2]};   // the strip wants G, R, B
    for (int j = 0; j < 3; j++) for (int bit = 7; bit >= 0; bit--) {
      bool one = (c[j] >> bit) & 1;
      sym[k].level0 = 1; sym[k].duration0 = one ? 8 : 4;           // 0.8 / 0.4 us high
      sym[k].level1 = 0; sym[k].duration1 = one ? 4 : 8;           // 0.4 / 0.8 us low
      k++;
    }
  }
  rmtWrite(dataPin, sym, k, 20);
}
// brightness, then the power limit: ~20 mA per channel at full, ~1 mA idle per LED
int lastMa = 0;
void applyLevel(uint8_t* rgb, int n) {
  uint32_t sum = 0;
  for (int i = 0; i < n * 3; i++) { rgb[i] = (uint16_t)rgb[i] * brightPct / 100; sum += rgb[i]; }
  uint32_t ma = sum * 20 / 255 + n;
  lastMa = ma;
  if (ma > limitMa && sum) {
    uint32_t scale = (uint32_t)(limitMa > (uint32_t)n ? limitMa - n : 1) * 255 / 20 * 256 / sum;   // 8.8 fixed
    for (int i = 0; i < n * 3; i++) rgb[i] = rgb[i] * scale >> 8;
    lastMa = limitMa;
  }
}

// ================= onboard LED =================
void onboard(uint8_t r, uint8_t g, uint8_t b) {
#ifdef BOARD_XIAO_C5
  int v = (r > g ? (r > b ? r : b) : (g > b ? g : b));
  analogWrite(P_ONBOARD, 255 - v);                                   // single colour, active low
#else
  rgbLedWrite(P_ONBOARD, r / 4, g / 4, b / 4);                       // WS2812: a quarter is plenty
#endif
}

// ================= ESP-NOW =================
QueueHandle_t evQ;
volatile uint32_t framesIn = 0;
void onFrame(const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  framesIn++;
  Ev e; memcpy(&e.f, d, sizeof(LinkFrame)); e.t = esp_timer_get_time();
  xQueueSend(evQ, &e, 0);
}
bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) { onFrame(d, len); });
  return true;
}

// ================= beat state (same logic as the Beat Gateway) =================
const char* srcName(Src s) { return s == GS_AIR ? "AIR" : s == GS_BOOTH ? "BOOTH" : "-"; }
Src src = GS_NONE;
int64_t srcLastUs = 0, lastBeatUs = 0;
bool lastReal = false;
uint32_t periodUs = 500000;
uint16_t bpm10 = 0;
uint8_t beatNo = 0, hue = 160;
int freeBeats = 0;
uint32_t beatsReal = 0;
int16_t rssi = 0;
int64_t bridgeHelloUs = 0; bool bridgeRadio = false;
SState ss = {};

void beat(int64_t tb, uint8_t no, bool real) {
  lastBeatUs = tb; lastReal = real; beatNo = no;
  ss.beatMs = millis() - (uint32_t)((esp_timer_get_time() - tb) / 1000);
  ss.beatId++; ss.beatPos = no; ss.hue = hue; ss.bpm10 = bpm10;
  if (no == 0) { ss.bars++; if (autoMode && ss.bars % 8 == 0) ss.pattern = (ss.pattern + 1) % SP_COUNT; }
}
void raw(const LinkFrame& f) {
  switch (f.type) {
    case LF_RELAY: outf("R,%u,%.1f,%u,%u,%d,%.1f,%u\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, f.rssi, f.snr4 / 4.0f, f.flags & LINK_FLAG_ECHO ? 1 : 0); break;
    case LF_ACK:   outf("A,%u,%.1f,%u,%u,%lu\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, (unsigned long)f.count); break;
    case LF_FIRE:  outf("F,%.1f,%u,%u\n", f.bpm10 / 10.0f, f.beat & 3, f.hue); break;
    case LF_HELLO: outf("H,%u,%lu\n", f.flags & 1, (unsigned long)f.count); break;
  }
}
void handle(const Ev& e) {
  const LinkFrame& f = e.f;
  raw(f);
  if (f.type == LF_HELLO) { bridgeHelloUs = e.t; bridgeRadio = f.flags & 1; return; }
  if (f.type != LF_RELAY && f.type != LF_ACK && f.type != LF_FIRE) return;
  if (f.bpm10 < 300 || f.bpm10 > 3000) return;
  bool echo = f.flags & LINK_FLAG_ECHO;
  Src s = (f.type == LF_RELAY && !echo) ? GS_AIR : GS_BOOTH;
  if (src != GS_NONE && s != src && e.t - srcLastUs < SRC_HOLD_US) return;
  if (s != src) outf("# source %s\n", srcName(s));
  src = s; srcLastUs = e.t;
  int64_t lat = f.type == LF_FIRE ? LAT_FIRE_US : LAT_LORA_US + (echo ? 1000 : 0);
  int64_t tb = e.t - lat;
  uint32_t per = 600000000UL / f.bpm10;
  bool resync = false;
  if (lastBeatUs) {
    int64_t gap = tb - lastBeatUs;
    if (lastReal && gap < (int64_t)per * 45 / 100) return;            // same beat again (FIRE, ACK, echo)
    resync = !lastReal && gap < (int64_t)per / 2;
  }
  periodUs = per; bpm10 = f.bpm10; hue = f.hue; freeBeats = 0; beatsReal++;
  if (f.type == LF_RELAY && !echo) rssi = f.rssi;
  if (resync) { lastBeatUs = tb; lastReal = true; beatNo = f.beat & 3; }
  else beat(tb, f.beat & 3, true);
  outf("B,%u,%.1f,%s,%lld,%u,%u,%d\n", (f.beat & 3) + 1, f.bpm10 / 10.0f, srcName(s),
       (long long)(esp_timer_get_time() - tb), f.seq, f.hue, s == GS_AIR ? f.rssi : 0);
}

// ================= settings / commands =================
void saveAll() {
  prefs.putUShort("leds", nLeds); prefs.putUChar("pin", dataPin); prefs.putUChar("bright", brightPct);
  prefs.putUShort("limit", limitMa); prefs.putUChar("pattern", ss.pattern); prefs.putBool("auto", autoMode);
}
void status() {
  int64_t now = esp_timer_get_time();
  const char* st = src == GS_NONE ? "WAIT" : freeBeats ? "FREE" : "LOCK";
  const char* br = bridgeHelloUs && now - bridgeHelloUs < 3000000 ? (bridgeRadio ? "OK" : "NORADIO") : "-";
  outf("S,%s,%.1f,%s,%lu,STOP,%s,%lu\n", st, bpm10 / 10.0f, srcName(src), (unsigned long)beatsReal, br, (unsigned long)framesIn);
}
void settings() {
  outf("# Beat Strip v%s (%s) | %u LEDs on GPIO%u | bright %u%% | limit %u mA (now ~%d) | pattern %s%s\n",
       FW_VERSION, BOARD_TXT, nLeds, dataPin, brightPct, limitMa, lastMa, SP_NAME[ss.pattern], autoMode ? " (auto)" : "");
}
uint32_t testUntil = 0;
void command(String c) {
  c.trim(); c.toLowerCase();
  int sp = c.indexOf(' '); String k = sp > 0 ? c.substring(0, sp) : c, v = sp > 0 ? c.substring(sp + 1) : "";
  v.trim();
  if (k == "?" || k == "status") { status(); settings(); return; }
  if (k == "leds") { int n = v.toInt(); if (n >= 1 && n <= STRIP_MAX) { memset(frame, 0, sizeof(frame)); stripShow(frame, nLeds); nLeds = n; } }
  else if (k == "pin") { int p = v.toInt(); if (p >= 0 && p <= 28 && p != P_BTN && p != P_ONBOARD) { rmtDeinit(dataPin); dataPin = p; stripPinInit(); } }
  else if (k == "bright") { int b = v.toInt(); if (b >= 1 && b <= 100) brightPct = b; }
  else if (k == "limit") { int m = v.toInt(); if (m >= 100 && m <= 10000) limitMa = m; }
  else if (k == "pattern") {
    for (int i = 0; i < SP_COUNT; i++) if (v == SP_NAME[i]) ss.pattern = i;
    if (v.length() == 1 && v[0] >= '0' && v[0] < '0' + SP_COUNT) ss.pattern = v[0] - '0';
  }
  else if (k == "auto") autoMode = v == "on" || v == "1";
  else if (k == "test") { testUntil = millis() + 3000; outf("# test: red, green, blue, white along the strip\n"); return; }
  else { outf("# commands: ?  leds <1-%d>  pin <gpio>  bright <1-100>  limit <mA>  pattern <%s|%s|%s|%s|%s|%s>  auto on|off  test\n",
              STRIP_MAX, SP_NAME[0], SP_NAME[1], SP_NAME[2], SP_NAME[3], SP_NAME[4], SP_NAME[5]); return; }
  saveAll(); settings();
}

// ================= button =================
// press = next pattern, hold 1.5 s = AUTO, hold 4 s = brightness step. Feedback shows on the strip for a moment.
uint32_t fbUntil = 0; uint8_t fbKind = 0, fbVal = 0;               // 1 = pattern number, 2 = auto on/off, 3 = brightness
void button(uint32_t now) {
  static bool armed = false, down = false; static uint32_t upSince = 0, t0 = 0;
  bool d = digitalRead(P_BTN) == LOW;
  if (!d) { if (!upSince) upSince = now | 1; if (now - upSince > 500) armed = true; } else upSince = 0;
  if (!armed) return;
  if (d && !down) { down = true; t0 = now; }
  else if (!d && down) {
    down = false; uint32_t held = now - t0;
    if (held >= 4000) { brightPct = brightPct >= 100 ? 25 : brightPct + 25; fbKind = 3; }
    else if (held >= 1500) { autoMode = !autoMode; fbKind = 2; fbVal = autoMode; }
    else if (held > 30) { ss.pattern = (ss.pattern + 1) % SP_COUNT; fbKind = 1; fbVal = ss.pattern; }
    else return;
    fbUntil = now + 700; saveAll(); settings();
  }
}

// ================= setup / loop =================
void setup() {
  Serial.begin(115200); Serial.setTxTimeoutMs(0);
  Serial0.begin(115200);
  pinMode(P_BTN, INPUT_PULLUP);
#ifdef BOARD_XIAO_C5
  pinMode(P_ONBOARD, OUTPUT); analogWrite(P_ONBOARD, 255);
#endif
  prefs.begin("beatstrip", false);
  nLeds = prefs.getUShort("leds", 60); if (nLeds < 1 || nLeds > STRIP_MAX) nLeds = 60;
  dataPin = prefs.getUChar("pin", 1);
  brightPct = prefs.getUChar("bright", 50); if (brightPct < 1 || brightPct > 100) brightPct = 50;
  limitMa = prefs.getUShort("limit", 900);
  ss.pattern = prefs.getUChar("pattern", SP_PULSE) % SP_COUNT;
  autoMode = prefs.getBool("auto", false);
  ss.n = nLeds; ss.hue = 160;
  stripPinInit();
  evQ = xQueueCreate(16, sizeof(Ev));
  bool ok = espnowInit();
  delay(300);
  outf("# Beat Gateway v%s (Beat Strip, %s) | ESP-NOW ch %d %s\n", FW_VERSION, BOARD_TXT, LINK_ESPNOW_CH, ok ? "listening" : "FAILED");
  settings();
}

void loop() {
  Ev e;
  while (xQueueReceive(evQ, &e, 0) == pdTRUE) handle(e);
  int64_t now = esp_timer_get_time();
  if (src != GS_NONE && lastBeatUs && now > lastBeatUs + periodUs + periodUs / 4) {   // link quiet: keep going, then stop
    if (freeBeats < FREE_BEATS) { freeBeats++; beat(lastBeatUs + periodUs, (beatNo + 1) & 3, false); }
    else { src = GS_NONE; lastBeatUs = 0; bpm10 = 0; freeBeats = 0; outf("S,WAIT,0.0,-,0,STOP,-\n"); }
  }
  uint32_t ms = millis();
  button(ms);

  static uint32_t lastFrame = 0;
  if (ms - lastFrame >= 16) {                                         // ~60 fps
    lastFrame = ms;
    ss.n = nLeds; ss.live = src != GS_NONE;
    stripRender(frame, ss, ms);
    if ((int32_t)(testUntil - ms) > 0) {                              // test: one colour per quarter of the strip
      for (int i = 0; i < nLeds; i++) { int q = i * 4 / nLeds; frame[i * 3] = q == 0 || q == 3 ? 255 : 0; frame[i * 3 + 1] = q == 1 || q == 3 ? 255 : 0; frame[i * 3 + 2] = q >= 2 ? 255 : 0; }
    } else if ((int32_t)(fbUntil - ms) > 0) {                         // button feedback
      memset(frame, 0, nLeds * 3);
      if (fbKind == 1) for (int i = 0; i <= fbVal && i * 2 < nLeds; i++) { frame[i * 6] = 255; frame[i * 6 + 1] = 255; frame[i * 6 + 2] = 255; }
      else if (fbKind == 2) for (int i = 0; i < nLeds; i++) frame[i * 3 + (fbVal ? 1 : 0)] = 200;
      else for (int i = 0; i < nLeds; i++) frame[i * 3] = frame[i * 3 + 1] = frame[i * 3 + 2] = 255;
    }
    applyLevel(frame, nLeds);
    stripShow(frame, nLeds);
    // onboard LED follows the beat
    if (ss.live) { float t = sage(ss, ms); float v = expf(-t * 5); int r, g, b; float fr, fg, fb; shue(ss.hue, fr, fg, fb);
                   r = fr * v * 255; g = fg * v * 255; b = fb * v * 255; onboard(r, g, b); }
    else { int v = ms % 2000 < 60 ? 120 : 0; onboard(v, v * 7 / 10, 0); }        // waiting: amber blip every 2 s
  }

  static int64_t lastStatus = 0;
  if (now - lastStatus >= 1000000) { lastStatus = now; status(); }

  static String line;
  for (Stream* s : {(Stream*)&Serial, (Stream*)&Serial0}) {
    while (s->available()) {
      char c = s->read();
      if (c == '\n' || c == '\r') { if (line.length()) command(line); line = ""; }
      else if (line.length() < 40) line += c;
    }
  }
}
