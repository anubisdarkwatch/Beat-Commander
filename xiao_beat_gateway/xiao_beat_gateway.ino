// Beat Gateway on a Seeed Studio XIAO ESP32-S3.
// Listens to the Beat Conductor link (ESP-NOW channel 1) and hands every beat to whatever it is plugged into:
//   - USB MIDI clock (24 per beat) + Start on the downbeat / Stop when the beat is gone + a note per beat
//     -> Ableton, Traktor, Rekordbox, Resolume, drum machines... no driver, no app needed.
//   - USB serial text lines (one per beat) -> the Pi hub script (Ableton Link + OSC + web/JSON) and TrackID.
//   - v1.1: it is also a radio for whatever it's plugged into. The Tab5 Command Center sends "F,<bpm10>,<beat>,<hue>"
//     to put a beat on the link (takeover), and reads raw R/A/F/H lines for every frame heard.
// It follows whatever is on the beat link: the W10 conductor (heard by the Heltec bridge) or a booth controller
// (StickS3 / M5Dial / CYD) during a takeover. Between beats it free-runs, so the MIDI clock never stutters.
// LED: amber flash on every beat, longer on beat 1; a short blip every 2 s while waiting for a beat.
// Built by Anubis Darkwatch with Claude. MIT license.
//
// Arduino IDE 2.x, esp32 by Espressif 3.x. Board "XIAO_ESP32S3", USB Mode: USB-OTG (TinyUSB),
// USB CDC On Boot: Disabled (this sketch makes its own USB serial next to the MIDI port). No extra libraries.
// Flashing again later: if the flasher can't connect, hold BOOT, tap RESET, let go of BOOT, then flash.

#if ARDUINO_USB_MODE
#error "Set Tools > USB Mode to USB-OTG (TinyUSB)"
#endif

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <Preferences.h>
#include "USB.h"
#include "USBMIDI.h"
#include "link.h"
#include "gateway.h"

#define FW_VERSION "1.3"
#define LED_PIN 21                  // XIAO user LED, active LOW

// How long after the real beat each kind of frame reaches us (the gateway reports beat time = arrival - this).
#define LAT_FIRE_US  1000           // controller -> ESP-NOW (no LoRa in between)
#define LAT_LORA_US 10000           // on LoRa first (~9 ms airtime at SF7/BW500), then ESP-NOW from the bridge
#define SRC_HOLD_US 2000000         // stay on one source until it has been quiet this long
#define FREE_BEATS  8               // beats to keep the clock going after the link goes quiet

USBMIDI MIDI;
USBCDC USBSerial;
Preferences prefs;

// ---------------- settings (saved) ----------------
bool cfgNotes = true;               // MIDI note per beat (ch 16: 36 on beat 1, 37 on 2-4)
bool cfgTransport = true;           // MIDI Start on the first downbeat, Stop when the beat is lost
uint8_t cfgChannel = 16;

// ---------------- ESP-NOW in ----------------
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
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // v1.2: gentle on weak USB ports (Tab5); plenty for a bridge in the same booth
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);        // v1.3: radio always listening (modem sleep can drop ESP-NOW broadcasts)
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

// ---------------- beat state ----------------
const char* srcName(Src s) { return s == SRC_AIR ? "AIR" : s == SRC_BOOTH ? "BOOTH" : "-"; }

Src src = SRC_NONE;
int64_t srcLastUs = 0;
int64_t lastBeatUs = 0;             // estimated time of the last beat (real or free-run)
bool lastReal = false;
uint32_t periodUs = 500000;
uint16_t bpm10 = 0;
uint8_t beatNo = 0;
uint8_t hue = 0;
int freeBeats = 0;
uint32_t beatsReal = 0;
int16_t rssi = 0;

int64_t bridgeHelloUs = 0;
bool bridgeRadio = false;
uint32_t bridgeHeard = 0;

// MIDI clock
bool clockRun = false;
int tickIdx = 24;
int64_t anchorUs = 0;
int64_t noteOffUs = 0;
uint8_t noteOn = 0;

// LED
int64_t ledOffUs = 0;

void midiByte(uint8_t b) {             // single-byte real-time message (clock / start / stop)
  midiEventPacket_t p = {0x0F, b, 0, 0};
  MIDI.writePacket(&p);
}

void stopClock(const char* why) {
  if (clockRun && cfgTransport) midiByte(0xFC);
  clockRun = false; tickIdx = 24;
  if (why) USBSerial.printf("# clock stopped (%s)\n", why);
}

void led(int ms) { digitalWrite(LED_PIN, LOW); ledOffUs = esp_timer_get_time() + ms * 1000LL; }

void beat(int64_t tb, uint8_t no, bool real) {
  // finish the previous beat's clock ticks if this beat came a little early, so every beat gets exactly 24
  if (clockRun) while (tickIdx < 24) { midiByte(0xF8); tickIdx++; }
  if (!clockRun && no == 0) {
    clockRun = true;
    if (cfgTransport) midiByte(0xFA);
    USBSerial.println("# clock running");
  }
  if (clockRun) { anchorUs = tb; midiByte(0xF8); tickIdx = 1; }

  if (cfgNotes) {
    if (noteOn) MIDI.noteOff(noteOn, 0, cfgChannel);
    noteOn = no == 0 ? 36 : 37;
    MIDI.noteOn(noteOn, no == 0 ? 127 : 100, cfgChannel);
    noteOffUs = esp_timer_get_time() + 40000;
  }
  led(no == 0 ? 70 : 25);
  lastBeatUs = tb; lastReal = real; beatNo = no;
}

// ---------------- sending (takeover from the host) ----------------
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int64_t lastFireUs = 0;
uint32_t fired = 0;

void fire(uint16_t b10, uint8_t no, uint8_t h) {
  LinkFrame f = linkFrame(LF_FIRE);
  f.bpm10 = b10; f.beat = no & 3; f.hue = h;
  esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f));
  lastFireUs = esp_timer_get_time(); fired++;
}

// raw line for every frame, so a host can run its own logic (the Tab5 does)
void raw(const LinkFrame& f) {
  switch (f.type) {
    case LF_RELAY: USBSerial.printf("R,%u,%.1f,%u,%u,%d,%.1f,%u\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, f.rssi, f.snr4 / 4.0f, f.flags & LINK_FLAG_ECHO ? 1 : 0); break;
    case LF_ACK:   USBSerial.printf("A,%u,%.1f,%u,%u,%lu\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, (unsigned long)f.count); break;
    case LF_FIRE:  USBSerial.printf("F,%.1f,%u,%u\n", f.bpm10 / 10.0f, f.beat & 3, f.hue); break;
    case LF_HELLO: USBSerial.printf("H,%u,%lu\n", f.flags & 1, (unsigned long)f.count); break;
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
  if (f.bpm10 < 300 || f.bpm10 > 3000) return;                 // 30-300 BPM only

  bool echo = f.flags & LINK_FLAG_ECHO;
  Src s = (f.type == LF_RELAY && !echo) ? SRC_AIR : SRC_BOOTH;
  if (src != SRC_NONE && s != src && e.t - srcLastUs < SRC_HOLD_US) return;   // two sources at once: keep ours
  if (s != src) USBSerial.printf("# source %s\n", srcName(s));
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
  if (resync) { lastBeatUs = tb; lastReal = true; beatNo = f.beat & 3; if (clockRun) anchorUs = tb; }
  else beat(tb, f.beat & 3, true);

  // B,<beat 1-4>,<bpm>,<source>,<us since the beat>,<seq>,<hue>,<rssi or 0>
  USBSerial.printf("B,%u,%.1f,%s,%lld,%u,%u,%d\n", (f.beat & 3) + 1, f.bpm10 / 10.0f, srcName(s),
                   (long long)(esp_timer_get_time() - tb), f.seq, f.hue, s == SRC_AIR ? f.rssi : 0);
}

// ---------------- serial commands ----------------
void status() {
  int64_t now = esp_timer_get_time();
  const char* st = src == SRC_NONE ? "WAIT" : freeBeats ? "FREE" : "LOCK";
  const char* br = bridgeHelloUs && now - bridgeHelloUs < 3000000 ? (bridgeRadio ? "OK" : "NORADIO") : "-";
  USBSerial.printf("S,%s,%.1f,%s,%lu,%s,%s,%lu\n", st, bpm10 / 10.0f, srcName(src), (unsigned long)beatsReal,
                   clockRun ? "RUN" : "STOP", br, (unsigned long)framesIn);
}

void saveCfg() {
  prefs.putBool("notes", cfgNotes); prefs.putBool("transport", cfgTransport); prefs.putUChar("ch", cfgChannel);
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
    USBSerial.printf("# Beat Gateway v%s | notes %d (ch %u) | transport %d | net 0x%02X ch %d | MAC %s\n", FW_VERSION,
                     cfgNotes, cfgChannel, cfgTransport, LINK_NET_ID, LINK_ESPNOW_CH, WiFi.macAddress().c_str());
  } else if (c.startsWith("notes ")) { cfgNotes = c.endsWith("1") || c.endsWith("on"); saveCfg(); USBSerial.printf("# notes %d\n", cfgNotes); }
  else if (c.startsWith("transport ")) { cfgTransport = c.endsWith("1") || c.endsWith("on"); saveCfg(); USBSerial.printf("# transport %d\n", cfgTransport); }
  else if (c.startsWith("channel ")) { int ch = c.substring(8).toInt(); if (ch >= 1 && ch <= 16) { cfgChannel = ch; saveCfg(); } USBSerial.printf("# channel %u\n", cfgChannel); }
  else if (c.length()) USBSerial.println("# commands: ?  notes on|off  transport on|off  channel 1-16  F,<bpm x10>,<beat 0-3>,<hue>");
}

// ---------------- setup / loop ----------------
void setup() {
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  prefs.begin("beatgw", false);
  cfgNotes = prefs.getBool("notes", true);
  cfgTransport = prefs.getBool("transport", true);
  cfgChannel = prefs.getUChar("ch", 16);

  evQ = xQueueCreate(16, sizeof(Ev));
  MIDI.begin();
  USBSerial.setTxTimeoutMs(0);
  USBSerial.begin();
  USB.productName("Beat Gateway");
  USB.manufacturerName("Anubis Darkwatch");
  USB.begin();

  delay(1500);                          // v1.2: let the host finish with USB before the radio's start-up current
  bool ok = espnowInit();
  delay(200);
  USBSerial.printf("# Beat Gateway v%s | ESP-NOW ch %d %s\n", FW_VERSION, LINK_ESPNOW_CH, ok ? "listening" : "FAILED");
  if (!ok) while (1) { digitalWrite(LED_PIN, LOW); delay(100); digitalWrite(LED_PIN, HIGH); delay(100); }
}

void loop() {
  Ev e;
  while (xQueueReceive(evQ, &e, 0) == pdTRUE) handle(e);
  int64_t now = esp_timer_get_time();

  // MIDI clock ticks between beats
  while (clockRun && tickIdx < 24 && now >= anchorUs + (int64_t)periodUs * tickIdx / 24) { midiByte(0xF8); tickIdx++; }

  // link went quiet: keep the beat going for a bit, then stop
  if (src != SRC_NONE && lastBeatUs && now > lastBeatUs + periodUs + periodUs / 4) {
    if (freeBeats < FREE_BEATS) {
      freeBeats++;
      beat(lastBeatUs + periodUs, (beatNo + 1) & 3, false);
      if (freeBeats == 1) USBSerial.println("# link quiet, free-running");
    } else {
      stopClock("beat lost");
      src = SRC_NONE; lastBeatUs = 0; bpm10 = 0; freeBeats = 0;
      USBSerial.println("S,WAIT,0.0,-,0,STOP,-");
    }
  }

  if (noteOn && now >= noteOffUs) { MIDI.noteOff(noteOn, 0, cfgChannel); noteOn = 0; }
  if (ledOffUs && now >= ledOffUs) { digitalWrite(LED_PIN, HIGH); ledOffUs = 0; }

  static int64_t lastStatus = 0, lastIdleBlip = 0;
  if (now - lastStatus >= 1000000) { lastStatus = now; status(); }
  if (src == SRC_NONE && now - lastIdleBlip >= 2000000) { lastIdleBlip = now; led(8); }

  static String line;
  while (USBSerial.available()) {
    char c = USBSerial.read();
    if (c == '\n' || c == '\r') { if (line.length()) command(line); line = ""; }
    else if (line.length() < 40) line += c;
  }
}
