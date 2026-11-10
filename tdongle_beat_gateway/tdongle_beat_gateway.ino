// Beat Commander on the LilyGo T-Dongle S3 and T-Dongle C5: USB sticks with a 0.96" screen and an RGB LED.
//
//   T-Dongle S3  ->  Beat Gateway Dongle. Everything the XIAO Beat Gateway does: USB MIDI clock (24 per beat) +
//                    Start / Stop + a note per beat, the serial text feed for Beat Hub (Ableton Link, OSC, web) and
//                    TrackID, and the radio for the Tab5 ("F,..." takeover lines). Plugs straight into a laptop or
//                    the Pi, no cable.
//   T-Dongle C5  ->  Beat Light. The C5 has no USB MIDI, so: a pocket beat light for any USB charger or power bank,
//                    and the same serial text feed (Beat Hub works with it, minus MIDI clock).
//
// Both follow the beat link over ESP-NOW (channel 1). Screen pages: BEAT (BPM, status, beat boxes), STATS, and the
// visuals PULSE, TUNNEL, BARS, WARP, PLASMA. The LED flashes in the conductor's colour on every beat.
// Button (BOOT): press = next page, hold 1.5 s = AUTO (new visual every 8 bars), hold 4 s = display fix
// (colour order / flip / invert). While you hold, the screen says what letting go will do.
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Library: LovyanGFX.
//   S3: esp32 by Espressif 3.x, board "ESP32S3 Dev Module", Flash 16MB, USB Mode "USB-OTG (TinyUSB)",
//       USB CDC On Boot "Disabled" (this sketch makes its own USB serial next to the MIDI port).
//   C5: board package M5Stack 3.x (or esp32 3.3+), an ESP32-C5 board, USB CDC On Boot "Enabled". Build with
//       -DBOARD_TDONGLE_C5 or uncomment the line below.

// #define BOARD_TDONGLE_C5
#if !defined(BOARD_TDONGLE_C5) && !defined(BOARD_TDONGLE_S3)
#define BOARD_TDONGLE_S3
#endif

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "link.h"
#include "gateway.h"
#include "dongle_ui.h"

#define FW_VERSION "1.1"

#if defined(BOARD_TDONGLE_S3)
#if ARDUINO_USB_MODE
#error "Set Tools > USB Mode to USB-OTG (TinyUSB)"
#endif
#define HAS_MIDI 1
#include "USB.h"
#include "USBMIDI.h"
USBMIDI MIDI;
USBCDC USBSerial;
#define FEED USBSerial
#define P_MOSI 3
#define P_SCK 5
#define P_CS 4
#define P_DC 2
#define P_RST 1
#define P_BL 38
#define P_LED_DI 40
#define P_LED_CI 39
#define P_BTN 0
#define BOARD_TXT "T-Dongle S3"
#else
#define HAS_MIDI 0
#define FEED Serial
#define P_MOSI 2
#define P_SCK 6
#define P_CS 10
#define P_DC 3
#define P_RST 1
#define P_BL 0
#define P_LED_CI 4
#define P_LED_DI 5
#define P_BTN 28
#define P_SD_CS 23
#define BOARD_TXT "T-Dongle C5"
#endif

// How long after the real beat each kind of frame reaches us (the feed reports beat time = arrival - this).
#define LAT_FIRE_US  1000
#define LAT_LORA_US 10000
#define SRC_HOLD_US 2000000
#define FREE_BEATS  8

Preferences prefs;

// ================= display =================
class LGFX_Dongle : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
public:
  void setup(bool bgr, bool invert) {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST; b.spi_mode = 0; b.freq_write = 27000000; b.freq_read = 16000000;
    b.spi_3wire = true; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = P_SCK; b.pin_mosi = P_MOSI; b.pin_miso = -1; b.pin_dc = P_DC;
    _bus.config(b); _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = P_CS; p.pin_rst = P_RST; p.pin_busy = -1;
    p.panel_width = 80; p.panel_height = 160; p.offset_x = 26; p.offset_y = 1;   // the 0.96" "green tab 160x80" glass
    p.readable = false; p.invert = invert; p.rgb_order = bgr; p.bus_shared = false;
    _panel.config(p);
    auto l = _light.config();
    l.pin_bl = P_BL; l.invert = true; l.freq = 12000; l.pwm_channel = 7;          // backlight is active low on both
    _light.config(l); _panel.setLight(&_light);
    setPanel(&_panel);
  }
};
LGFX_Dongle tft;
LGFX_Sprite cv(&tft);
uint8_t panelMode = 0;             // bit0 colour order, bit1 turn 180, bit2 invert off
volatile bool panelChanged = false;
void applyPanel() {
  tft.setup(panelMode & 1, !(panelMode & 4));
  tft.init();
  tft.setRotation(panelMode & 2 ? 3 : 1);
  tft.setBrightness(200);
}

// ================= APA102 LED =================
void apaByte(uint8_t b) { for (int i = 7; i >= 0; i--) { digitalWrite(P_LED_DI, (b >> i) & 1); digitalWrite(P_LED_CI, 1); digitalWrite(P_LED_CI, 0); } }
void apaSet(uint8_t r, uint8_t g, uint8_t b, uint8_t bright) {   // bright 0..31
  for (int i = 0; i < 4; i++) apaByte(0x00);
  apaByte(0xE0 | (bright & 31)); apaByte(b); apaByte(g); apaByte(r);
  for (int i = 0; i < 4; i++) apaByte(0xFF);
}
int64_t ledAt = 0; int ledLen = 0; uint8_t ledR, ledG, ledB, ledPeak; int ledLast = -1;
void led(int ms, uint8_t h, bool down) {
  int r, g, b; vhue(h, r, g, b); ledR = r; ledG = g; ledB = b;
  ledPeak = down ? 24 : 10; ledLen = ms; ledAt = esp_timer_get_time(); ledLast = -1;
}
void ledStep(int64_t now) {                      // fade the beat flash out
  if (!ledLen) return;
  int dt = (int)((now - ledAt) / 1000);
  int lvl = dt >= ledLen ? 0 : ledPeak - ledPeak * dt / ledLen;
  if (lvl == ledLast) return;
  ledLast = lvl;
  if (lvl) apaSet(ledR, ledG, ledB, lvl); else { apaSet(0, 0, 0, 0); ledLen = 0; }
}

// ================= ESP-NOW in =================
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
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // gentle on weak USB ports; plenty for a bridge in the same booth
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) { onFrame(d, len); });
#else
  esp_now_register_recv_cb([](const uint8_t*, const uint8_t* d, int len) { onFrame(d, len); });
#endif
  return true;
}

// ================= screen state (loop -> display task) =================
DView dv = {};
portMUX_TYPE dvMux = portMUX_INITIALIZER_UNLOCKED;   // ints / pointers only inside

// ================= beat state =================
const char* srcName(Src s) { return s == GS_AIR ? "AIR" : s == GS_BOOTH ? "BOOTH" : "-"; }
Src src = GS_NONE;
int64_t srcLastUs = 0, lastBeatUs = 0;
bool lastReal = false;
uint32_t periodUs = 500000;
uint16_t bpm10 = 0;
uint8_t beatNo = 0, hue = 0;
int freeBeats = 0;
uint32_t beatsReal = 0;
int16_t rssi = 0;
int64_t bridgeHelloUs = 0; bool bridgeRadio = false; uint32_t bridgeHeard = 0;
bool clockRun = false;

#if HAS_MIDI
bool cfgNotes = true, cfgTransport = true; uint8_t cfgChannel = 16;
int tickIdx = 24; int64_t anchorUs = 0, noteOffUs = 0; uint8_t noteOn = 0;
void midiByte(uint8_t b) { midiEventPacket_t p = {0x0F, b, 0, 0}; MIDI.writePacket(&p); }
#endif

void stopClock(const char* why) {
#if HAS_MIDI
  if (clockRun && cfgTransport) midiByte(0xFC);
  tickIdx = 24;
#endif
  if (clockRun && why) FEED.printf("# clock stopped (%s)\n", why);
  clockRun = false;
}

void beat(int64_t tb, uint8_t no, bool real) {
#if HAS_MIDI
  if (clockRun) while (tickIdx < 24) { midiByte(0xF8); tickIdx++; }   // every beat gets exactly 24 ticks
  if (!clockRun && no == 0) { clockRun = true; if (cfgTransport) midiByte(0xFA); FEED.println("# clock running"); }
  if (clockRun) { anchorUs = tb; midiByte(0xF8); tickIdx = 1; }
  if (cfgNotes) {
    if (noteOn) MIDI.noteOff(noteOn, 0, cfgChannel);
    noteOn = no == 0 ? 36 : 37;
    MIDI.noteOn(noteOn, no == 0 ? 127 : 100, cfgChannel);
    noteOffUs = esp_timer_get_time() + 40000;
  }
#else
  if (!clockRun && no == 0) clockRun = true;      // "running" = locked on a bar (shown, no MIDI here)
#endif
  led(no == 0 ? 220 : 120, hue, no == 0);
  lastBeatUs = tb; lastReal = real; beatNo = no;
  uint32_t ms = millis() - (uint32_t)((esp_timer_get_time() - tb) / 1000);
  portENTER_CRITICAL(&dvMux);
  dv.beatMs = ms; dv.beatId++; dv.beatPos = no; dv.hue = hue; dv.bpm10 = bpm10;
  if (no == 0) {
    dv.bars++;
    if (dv.autoMode && dv.page >= P_PULSE && dv.bars % 8 == 0) { dv.page = dv.page + 1 < P_COUNT ? dv.page + 1 : P_PULSE; dv.labelUntil = ms + 1500; }
  }
  portEXIT_CRITICAL(&dvMux);
}

// ================= sending (takeover from the host, e.g. the Tab5) =================
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int64_t lastFireUs = 0;
void fire(uint16_t b10, uint8_t no, uint8_t h) {
  LinkFrame f = linkFrame(LF_FIRE);
  f.bpm10 = b10; f.beat = no & 3; f.hue = h;
  esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f));
  lastFireUs = esp_timer_get_time();
}
void raw(const LinkFrame& f) {
  switch (f.type) {
    case LF_RELAY: FEED.printf("R,%u,%.1f,%u,%u,%d,%.1f,%u\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, f.rssi, f.snr4 / 4.0f, f.flags & LINK_FLAG_ECHO ? 1 : 0); break;
    case LF_ACK:   FEED.printf("A,%u,%.1f,%u,%u,%lu\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, (unsigned long)f.count); break;
    case LF_FIRE:  FEED.printf("F,%.1f,%u,%u\n", f.bpm10 / 10.0f, f.beat & 3, f.hue); break;
    case LF_HELLO: FEED.printf("H,%u,%lu\n", f.flags & 1, (unsigned long)f.count); break;
  }
}

void handle(const Ev& e) {
  const LinkFrame& f = e.f;
  raw(f);
  if (f.type == LF_ACK && lastFireUs && e.t - lastFireUs < 150000) {   // the bridge sent our beat: mirror it for consoles
    LinkFrame ef = f; ef.type = LF_RELAY; ef.flags = LINK_FLAG_ECHO; ef.rssi = 0; ef.snr4 = 0; ef.count = 0;
    esp_now_send(BCAST, (const uint8_t*)&ef, sizeof(ef));
  }
  if (f.type == LF_HELLO) { bridgeHelloUs = e.t; bridgeRadio = f.flags & 1; bridgeHeard = f.count; return; }
  if (f.type != LF_RELAY && f.type != LF_ACK && f.type != LF_FIRE) return;
  if (f.bpm10 < 300 || f.bpm10 > 3000) return;

  bool echo = f.flags & LINK_FLAG_ECHO;
  Src s = (f.type == LF_RELAY && !echo) ? GS_AIR : GS_BOOTH;
  if (src != GS_NONE && s != src && e.t - srcLastUs < SRC_HOLD_US) return;   // two sources at once: keep ours
  if (s != src) FEED.printf("# source %s\n", srcName(s));
  src = s; srcLastUs = e.t;

  int64_t lat = f.type == LF_FIRE ? LAT_FIRE_US : LAT_LORA_US + (echo ? 1000 : 0);
  int64_t tb = e.t - lat;
  uint32_t per = 600000000UL / f.bpm10;
  bool resync = false;
  if (lastBeatUs) {
    int64_t gap = tb - lastBeatUs;
    if (lastReal && gap < (int64_t)per * 45 / 100) return;          // same beat again (FIRE, then ACK, then echo)
    resync = !lastReal && gap < (int64_t)per / 2;                   // a late beat we already free-ran: just re-align
  }
  periodUs = per; bpm10 = f.bpm10; hue = f.hue; freeBeats = 0; beatsReal++;
  if (f.type == LF_RELAY && !echo) rssi = f.rssi;
  if (resync) {
    lastBeatUs = tb; lastReal = true; beatNo = f.beat & 3;
#if HAS_MIDI
    if (clockRun) anchorUs = tb;
#endif
  } else beat(tb, f.beat & 3, true);

  // B,<beat 1-4>,<bpm>,<source>,<us since the beat>,<seq>,<hue>,<rssi or 0>
  FEED.printf("B,%u,%.1f,%s,%lld,%u,%u,%d\n", (f.beat & 3) + 1, f.bpm10 / 10.0f, srcName(s),
              (long long)(esp_timer_get_time() - tb), f.seq, f.hue, s == GS_AIR ? f.rssi : 0);
}

// ================= serial commands =================
void status() {
  int64_t now = esp_timer_get_time();
  const char* st = src == GS_NONE ? "WAIT" : freeBeats ? "FREE" : "LOCK";
  const char* br = bridgeHelloUs && now - bridgeHelloUs < 3000000 ? (bridgeRadio ? "OK" : "NORADIO") : "-";
  FEED.printf("S,%s,%.1f,%s,%lu,%s,%s,%lu\n", st, bpm10 / 10.0f, srcName(src), (unsigned long)beatsReal,
              clockRun ? "RUN" : "STOP", br, (unsigned long)framesIn);
}
void command(String c) {
  c.trim();
  if (c.startsWith("F,")) {                                     // F,<bpm x10>,<beat 0-3>,<hue>: put a beat on the link
    int a = c.indexOf(',', 2), b = c.indexOf(',', a + 1);
    long b10 = c.substring(2, a).toInt();
    if (a > 0 && b > 0 && b10 >= 300 && b10 <= 3000) fire(b10, c.substring(a + 1, b).toInt(), c.substring(b + 1).toInt());
    return;
  }
  c.toLowerCase();
  if (c == "?" || c == "status") {
    status();
    FEED.printf("# Beat Gateway v%s (%s) | net 0x%02X ch %d | MAC %s\n", FW_VERSION, BOARD_TXT, LINK_NET_ID, LINK_ESPNOW_CH, WiFi.macAddress().c_str());
#if HAS_MIDI
  } else if (c.startsWith("notes ")) { cfgNotes = c.endsWith("1") || c.endsWith("on"); prefs.putBool("notes", cfgNotes); FEED.printf("# notes %d\n", cfgNotes); }
  else if (c.startsWith("transport ")) { cfgTransport = c.endsWith("1") || c.endsWith("on"); prefs.putBool("transport", cfgTransport); FEED.printf("# transport %d\n", cfgTransport); }
  else if (c.startsWith("channel ")) { int ch = c.substring(8).toInt(); if (ch >= 1 && ch <= 16) { cfgChannel = ch; prefs.putUChar("ch", cfgChannel); } FEED.printf("# channel %u\n", cfgChannel); }
  else if (c.length()) FEED.println("# commands: ?  notes on|off  transport on|off  channel 1-16  F,<bpm x10>,<beat 0-3>,<hue>");
#else
  } else if (c.length()) FEED.println("# commands: ?  F,<bpm x10>,<beat 0-3>,<hue>");
#endif
}

// ================= button =================
// press = next page, hold 1.5 s = AUTO, hold 4 s = next display setting. Counts only once BOOT has read released.
const char* btnHint = nullptr;
void button(uint32_t now) {
  static bool armed = false, down = false; static uint32_t upSince = 0, t0 = 0;
  bool d = digitalRead(P_BTN) == LOW;
  if (!d) { if (!upSince) upSince = now | 1; if (now - upSince > 500) armed = true; } else upSince = 0;
  if (!armed) return;
  if (d && !down) { down = true; t0 = now; }
  uint32_t held = now - t0;
  if (d && down) {
    btnHint = held >= 4000 ? "LET GO: DISPLAY" : held >= 1500 ? (dv.autoMode ? "LET GO: AUTO OFF" : "LET GO: AUTO ON") : nullptr;
  } else if (!d && down) {
    down = false; btnHint = nullptr;
    portENTER_CRITICAL(&dvMux);
    if (held >= 4000) { panelMode = (panelMode + 1) & 7; panelChanged = true; }
    else if (held >= 1500) { dv.autoMode = !dv.autoMode; if (dv.autoMode && dv.page < P_PULSE) dv.page = P_PULSE; dv.labelUntil = now + 1500; }
    else if (held > 30) { dv.page = (dv.page + 1) % P_COUNT; dv.labelUntil = now + 1500; }
    uint8_t pg = dv.page; bool au = dv.autoMode;
    portEXIT_CRITICAL(&dvMux);
    prefs.putUChar("page", pg); prefs.putBool("auto", au); prefs.putUChar("panel", panelMode);
  }
}

// ================= display task =================
void displayTask(void*) {
  for (;;) {
    if (panelChanged) { panelChanged = false; applyPanel(); }
    uint32_t now = millis();
    DView v;
    portENTER_CRITICAL(&dvMux); v = dv; portEXIT_CRITICAL(&dvMux);
    if (v.st == D_WAIT) {                                    // idle: the visuals run a 120 BPM demo
      v.bpm10 = 0; v.beatMs = now / 500 * 500; v.beatId = now / 500; v.beatPos = (now / 500) & 3; v.hue = 160 + now / 2000 * 24;
    }
    dongleDraw(cv, v, now);
    cv.pushSprite(0, 0);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ================= setup / loop =================
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

void setup() {
  pinMode(P_LED_DI, OUTPUT); pinMode(P_LED_CI, OUTPUT); apaSet(0, 0, 0, 0);
  pinMode(P_BTN, INPUT_PULLUP);
#ifdef P_SD_CS
  pinMode(P_SD_CS, OUTPUT); digitalWrite(P_SD_CS, HIGH);   // SD card shares the display's SPI pins
#endif
  prefs.begin("beatdongle", false);
  panelMode = prefs.getUChar("panel", 0) & 7;
  dv.page = prefs.getUChar("page", P_BEAT) % P_COUNT;
  dv.autoMode = prefs.getBool("auto", false);
  dv.gateway = HAS_MIDI;
  dv.labelUntil = 2500;
#if HAS_MIDI
  cfgNotes = prefs.getBool("notes", true); cfgTransport = prefs.getBool("transport", true); cfgChannel = prefs.getUChar("ch", 16);
#endif

  applyPanel();
  dongleInitFonts();
  cv.setColorDepth(16);
  cv.createSprite(DW, DH);
  cv.fillScreen(D_BG);
  cv.setFont(&vfLbl); cv.setTextDatum(middle_center); cv.setTextColor(D_AMB);
  cv.drawString("BEAT COMMANDER", DW / 2, DH / 2 - 8);
  cv.setFont(&dfMono); cv.setTextColor(D_AMB_D);
  cv.drawString(HAS_MIDI ? "Beat Gateway v" FW_VERSION : "Beat Light v" FW_VERSION, DW / 2, DH / 2 + 12);
  cv.pushSprite(0, 0);
  for (int i = 0; i <= 24; i += 3) { apaSet(255, 176, 0, i); delay(25); }   // amber hello
  for (int i = 24; i >= 0; i -= 3) { apaSet(255, 176, 0, i); delay(25); }

  evQ = xQueueCreate(16, sizeof(Ev));
#if HAS_MIDI
  MIDI.begin();
  USBSerial.setTxTimeoutMs(0);
  USBSerial.begin();
  USB.productName("Beat Gateway");
  USB.manufacturerName("Anubis Darkwatch");
  USB.begin();
#else
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);
#endif
  delay(1500);                          // let the host finish with USB before the radio's start-up current
  tft.setBrightness(40);                // and keep the backlight low while Wi-Fi calibrates
  bool ok = espnowInit();
  delay(150);
  tft.setBrightness(200);
  FEED.printf("# Beat Gateway v%s (%s) | ESP-NOW ch %d %s\n", FW_VERSION, BOARD_TXT, LINK_ESPNOW_CH, ok ? "listening" : "FAILED");
#if CONFIG_FREERTOS_UNICORE
  xTaskCreate(displayTask, "disp", 8192, nullptr, 1, nullptr);
#else
  xTaskCreatePinnedToCore(displayTask, "disp", 8192, nullptr, 1, nullptr, 0);   // core 0: the MIDI clock stays on core 1
#endif
}

void loop() {
  Ev e;
  while (xQueueReceive(evQ, &e, 0) == pdTRUE) handle(e);
  int64_t now = esp_timer_get_time();
#if HAS_MIDI
  while (clockRun && tickIdx < 24 && now >= anchorUs + (int64_t)periodUs * tickIdx / 24) { midiByte(0xF8); tickIdx++; }
#endif
  // link went quiet: keep the beat going for a bit, then stop
  if (src != GS_NONE && lastBeatUs && now > lastBeatUs + periodUs + periodUs / 4) {
    if (freeBeats < FREE_BEATS) {
      freeBeats++;
      beat(lastBeatUs + periodUs, (beatNo + 1) & 3, false);
      if (freeBeats == 1) FEED.println("# link quiet, free-running");
    } else {
      stopClock("beat lost");
      src = GS_NONE; lastBeatUs = 0; bpm10 = 0; freeBeats = 0;
      FEED.println("S,WAIT,0.0,-,0,STOP,-");
    }
  }
#if HAS_MIDI
  if (noteOn && now >= noteOffUs) { MIDI.noteOff(noteOn, 0, cfgChannel); noteOn = 0; }
#endif
  ledStep(now);

  uint32_t ms = millis();
  button(ms);
  static int64_t lastStatus = 0, lastIdle = 0;
  if (now - lastStatus >= 1000000) { lastStatus = now; status(); }
  if (src == GS_NONE && now - lastIdle >= 2000000) { lastIdle = now; led(60, 24, false); }   // waiting: a dim amber blip

  // screen state
  portENTER_CRITICAL(&dvMux);
  dv.st = src == GS_NONE ? D_WAIT : freeBeats ? D_FREE : D_LOCK;
  dv.src = src == GS_AIR ? DS_AIR : src == GS_BOOTH ? DS_BOOTH : DS_NONE;
  dv.clockRun = clockRun; dv.rssi = rssi; dv.beats = beatsReal; dv.frames = framesIn;
  dv.bridge = bridgeHelloUs && now - bridgeHelloUs < 3000000 ? (bridgeRadio ? 1 : 2) : 0;
  dv.hint = btnHint;
  portEXIT_CRITICAL(&dvMux);

  static String line;
  while (FEED.available()) {
    char c = FEED.read();
    if (c == '\n' || c == '\r') { if (line.length()) command(line); line = ""; }
    else if (line.length() < 40) line += c;
  }
}
