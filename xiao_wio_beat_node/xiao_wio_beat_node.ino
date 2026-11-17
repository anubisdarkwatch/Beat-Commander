// Beat Node: Seeed XIAO ESP32-S3 + Wio-SX1262 kit (the Seeed Meshtastic kit) in Beat Commander.
// One small stick that is both halves of the booth link:
//   BRIDGE   it hears the conductor on LoRa (916.0 MHz, SF7 / BW500) and relays every beat to ESP-NOW for the booth
//            gear, puts booth controllers' takeover beats on LoRa and confirms them, and sends the bridge heartbeat.
//            Does the Heltec V3 bridge's job, no screen needed.
//   GATEWAY  plugged into a computer, Pi or the Tab5 it is also the Beat Gateway: USB MIDI clock, the serial feed
//            for Beat Hub / TrackID, and "F,..." takeover lines from the Tab5 go straight out on LoRa.
// Runs on a USB charger / power bank too (bridge only).
//
// Bridge mode (saved): AUTO (default) bridges unless another bridge is on air: a Heltec bridge or T-Deck always wins,
// and between Beat Nodes the lowest MAC wins, so several nodes never transmit the same beat twice.
// ON = always bridge. OFF = never transmit (gateway + listener only).
//
// LED (XIAO, orange): flash on every beat, longer on beat 1, double flash when this node put a beat on air;
// a blip every 2 s while waiting. Button on the Wio board: hold 2 s = next bridge mode (1 / 2 / 3 blinks = AUTO /
// ON / OFF).
// Serial: ?  bridge auto|on|off  power <2-22>  notes on|off  transport on|off  channel 1-16  F,<bpm x10>,<beat>,<hue>
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x. Board "XIAO_ESP32S3", USB Mode: USB-OTG (TinyUSB). No extra libraries.
// Flashing again later: if the flasher can't connect, hold BOOT, tap RESET, let go of BOOT, then flash.

#if ARDUINO_USB_MODE
#error "Set Tools > USB Mode to USB-OTG (TinyUSB)"
#endif

#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <Preferences.h>
#include "USB.h"
#include "USBMIDI.h"
#include "link.h"
#include "gateway.h"

#define FW_VERSION "1.0"
#define LINK_FREQ_HZ 916000000UL

// ---------- XIAO ESP32-S3 + Wio-SX1262 (Meshtastic seeed_xiao_s3 variant) ----------
#define L_SCK 7
#define L_MISO 8
#define L_MOSI 9
#define L_NSS 41
#define L_DIO1 39
#define L_BUSY 40
#define L_RST 42
#define L_RXEN 38           // antenna switch: high while receiving
#define P_LED 21            // XIAO orange LED (active low). The Wio board's button is on the same pin:
                            // the LED is driven open-drain so a press can never short a driven-high pin.

#define LAT_FIRE_US  1000
#define LAT_LORA_US 10000
#define SRC_HOLD_US 2000000
#define FREE_BEATS  8
#define HELLO_NODE  0x02    // HELLO flags bit1: sent by a Beat Node (bit0 = LoRa radio OK)
#define HELLO_W10   0x04    // HELLO flags bit2: the W10's relay (it steps aside for us)

USBMIDI MIDI;
USBCDC USBSerial;
Preferences prefs;

bool cfgNotes = true, cfgTransport = true; uint8_t cfgChannel = 16;
enum BridgeMode : uint8_t { BR_AUTO, BR_ON, BR_OFF };
uint8_t bridgeMode = BR_AUTO;
int8_t txPower = 10;

// ================= SX1262 =================
SPISettings loraSPI(8000000, MSBFIRST, SPI_MODE0);
bool loraOk = false;
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
  digitalWrite(L_RXEN, HIGH);
  cmd({0x8C, 0x00, 0x08, 0x00, 0xFF, 0x01, 0x00});
  cmd({0x02, 0xFF, 0xFF});
  cmd({0x82, 0xFF, 0xFF, 0xFF});                     // RX continuous
}
void setPower() { cmd({0x8E, (uint8_t)txPower, 0x04}); }
bool loraInit() {
  pinMode(L_RST, OUTPUT); digitalWrite(L_RST, LOW); delay(10); digitalWrite(L_RST, HIGH); delay(20); waitBusy(1000);
  cmd({0x80, 0x00});
  if (readReg(0x0740) != 0x14) return false;         // no answer: no radio
  cmd({0x97, 0x02, 0x00, 0x01, 0x40});               // DIO3 -> TCXO 1.8 V
  cmd({0x89, 0x7F}); delay(10); waitBusy();
  cmd({0x96, 0x01});                                 // DC-DC
  cmd({0x9D, 0x01});                                 // DIO2 = RF switch
  cmd({0x8A, 0x01});                                 // LoRa
  cmd({0x98, 0xE1, 0xE9}); delay(5);                 // image cal 902-928
  uint32_t f = (uint32_t)(((uint64_t)LINK_FREQ_HZ << 25) / 32000000ULL);
  cmd({0x86, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f});
  cmd({0x95, 0x04, 0x07, 0x00, 0x01});               // PA config (SX1262)
  setPower();
  cmd({0x8B, 0x07, 0x06, 0x01, 0x00});               // SF7, BW500, CR4/5
  cmd({0x8F, 0x00, 0x00});
  writeReg(0x08AC, 0x96);                            // boosted RX gain
  cmd({0x08, 0x02, 0x43, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00});
  startRx();
  return true;
}
void loraSend(const BeatPacket& p) {
  uint8_t buf[2 + sizeof(BeatPacket)] = {0x0E, 0x00};
  memcpy(buf + 2, &p, sizeof(p));
  cmd({0x80, 0x01});
  digitalWrite(L_RXEN, LOW);
  loraXfer(buf, sizeof(buf));
  cmd({0x8C, 0x00, 0x08, 0x00, (uint8_t)sizeof(p), 0x01, 0x00});
  cmd({0x02, 0xFF, 0xFF});
  cmd({0x83, 0x00, 0x00, 0x00});
}
int loraPoll(BeatPacket& p, int& rssi, int8_t& snr4) {   // 1 = TX done, 2 = beat received
  if (!digitalRead(L_DIO1)) return 0;
  uint8_t t[4] = {0x12, 0, 0, 0}, r[4]; loraXfer(t, 4, r);
  uint16_t irq = (r[2] << 8) | r[3];
  cmd({0x02, 0xFF, 0xFF});
  if (irq & 0x0001) { startRx(); return 1; }
  if (!(irq & 0x0002) || (irq & 0x0040)) return 0;
  uint8_t s[4] = {0x13, 0, 0, 0}, rs[4]; loraXfer(s, 4, rs);
  if (rs[2] != sizeof(BeatPacket)) return 0;
  uint8_t b[3 + sizeof(BeatPacket)] = {0x1E, rs[3], 0}, rb[3 + sizeof(BeatPacket)];
  loraXfer(b, sizeof(b), rb);
  uint8_t q[5] = {0x14, 0, 0, 0, 0}, rq[5]; loraXfer(q, 5, rq);
  memcpy(&p, rb + 3, sizeof(p));
  if (p.m0 != 'B' || p.m1 != 'T') return 0;
  rssi = -(int)rq[2] / 2; snr4 = (int8_t)rq[3];
  return 2;
}

// ================= ESP-NOW =================
QueueHandle_t evQ;
volatile uint32_t framesIn = 0;
uint8_t myMac[6];
QueueHandle_t netQ;
void onFrame(const uint8_t* mac, const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  framesIn++;
  NetEv n; memcpy(&n.e.f, d, sizeof(LinkFrame)); n.e.t = esp_timer_get_time();
  if (mac) memcpy(n.mac, mac, 6); else memset(n.mac, 0, 6);
  xQueueSend(netQ, &n, 0);
}
bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
  esp_now_register_recv_cb([](const esp_now_recv_info_t* i, const uint8_t* d, int len) { onFrame(i ? i->src_addr : nullptr, d, len); });
  WiFi.macAddress(myMac);
  return true;
}
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
void sendFrame(const LinkFrame& f) { esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f)); }

// ================= LED (open-drain) + button =================
int64_t ledOffUs = 0, ledNextUs = 0; int ledPulses = 0, ledLen = 0;
void ledRaw(bool on) { digitalWrite(P_LED, on ? LOW : HIGH); }
void led(int ms, int pulses = 1) { ledLen = ms; ledPulses = pulses; ledRaw(true); ledOffUs = esp_timer_get_time() + ms * 1000LL; ledNextUs = 0; }
void ledStep(int64_t now) {
  if (ledOffUs && now >= ledOffUs) {
    ledRaw(false); ledOffUs = 0;
    if (--ledPulses > 0) ledNextUs = now + 90000;
  }
  if (ledNextUs && now >= ledNextUs) { ledNextUs = 0; ledRaw(true); ledOffUs = now + ledLen * 1000LL; }
}

// ================= beat state (gateway logic) =================
const char* srcName(Src s) { return s == SRC_AIR ? "AIR" : s == SRC_BOOTH ? "BOOTH" : "-"; }
Src src = SRC_NONE;
int64_t srcLastUs = 0, lastBeatUs = 0;
bool lastReal = false;
uint32_t periodUs = 500000;
uint16_t bpm10 = 0;
uint8_t beatNo = 0, hue = 0;
int freeBeats = 0;
uint32_t beatsReal = 0;
int16_t rssi = 0;
bool clockRun = false;
int tickIdx = 24; int64_t anchorUs = 0, noteOffUs = 0; uint8_t noteOn = 0;
void midiByte(uint8_t b) { midiEventPacket_t p = {0x0F, b, 0, 0}; MIDI.writePacket(&p); }
void stopClock(const char* why) {
  if (clockRun && cfgTransport) midiByte(0xFC);
  clockRun = false; tickIdx = 24;
  if (why) USBSerial.printf("# clock stopped (%s)\n", why);
}
bool myBeat = false;                                 // the beat being handled is one this node put on air
void beat(int64_t tb, uint8_t no, bool real) {
  if (clockRun) while (tickIdx < 24) { midiByte(0xF8); tickIdx++; }
  if (!clockRun && no == 0) { clockRun = true; if (cfgTransport) midiByte(0xFA); USBSerial.println("# clock running"); }
  if (clockRun) { anchorUs = tb; midiByte(0xF8); tickIdx = 1; }
  if (cfgNotes) {
    if (noteOn) MIDI.noteOff(noteOn, 0, cfgChannel);
    noteOn = no == 0 ? 36 : 37;
    MIDI.noteOn(noteOn, no == 0 ? 127 : 100, cfgChannel);
    noteOffUs = esp_timer_get_time() + 40000;
  }
  if (real) led(no == 0 ? 70 : 25, myBeat ? 2 : 1);
  lastBeatUs = tb; lastReal = real; beatNo = no;
}
void raw(const LinkFrame& f) {
  switch (f.type) {
    case LF_RELAY: USBSerial.printf("R,%u,%.1f,%u,%u,%d,%.1f,%u\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, f.rssi, f.snr4 / 4.0f, f.flags & LINK_FLAG_ECHO ? 1 : 0); break;
    case LF_ACK:   USBSerial.printf("A,%u,%.1f,%u,%u,%lu\n", f.seq, f.bpm10 / 10.0f, f.beat & 3, f.hue, (unsigned long)f.count); break;
    case LF_FIRE:  USBSerial.printf("F,%.1f,%u,%u\n", f.bpm10 / 10.0f, f.beat & 3, f.hue); break;
    case LF_HELLO: USBSerial.printf("H,%u,%lu\n", f.flags & 1, (unsigned long)f.count); break;
  }
}
int64_t lastFireUs = 0;
int64_t otherHelloUs = 0; bool otherRadio = false; uint32_t otherHeard = 0;
void handle(const Ev& e) {
  const LinkFrame& f = e.f;
  raw(f);
  if (f.type == LF_ACK && lastFireUs && e.t - lastFireUs < 150000) {   // another bridge sent our FIRE: mirror it
    LinkFrame ef = f; ef.type = LF_RELAY; ef.flags = LINK_FLAG_ECHO; ef.rssi = 0; ef.snr4 = 0; ef.count = 0;
    sendFrame(ef);
  }
  if (f.type == LF_HELLO) return;
  if (f.type != LF_RELAY && f.type != LF_ACK && f.type != LF_FIRE) return;
  if (f.bpm10 < 300 || f.bpm10 > 3000) return;
  bool echo = f.flags & LINK_FLAG_ECHO;
  Src s = (f.type == LF_RELAY && !echo) ? SRC_AIR : SRC_BOOTH;
  if (src != SRC_NONE && s != src && e.t - srcLastUs < SRC_HOLD_US) return;
  if (s != src) USBSerial.printf("# source %s\n", srcName(s));
  src = s; srcLastUs = e.t;
  int64_t lat = f.type == LF_FIRE ? LAT_FIRE_US : LAT_LORA_US + (echo ? 1000 : 0);
  int64_t tb = e.t - lat;
  uint32_t per = 600000000UL / f.bpm10;
  bool resync = false;
  if (lastBeatUs) {
    int64_t gap = tb - lastBeatUs;
    if (lastReal && gap < (int64_t)per * 45 / 100) return;          // same beat again (LoRa + relay, FIRE + ACK...)
    resync = !lastReal && gap < (int64_t)per / 2;
  }
  periodUs = per; bpm10 = f.bpm10; hue = f.hue; freeBeats = 0; beatsReal++;
  if (f.type == LF_RELAY && !echo) rssi = f.rssi;
  if (resync) { lastBeatUs = tb; lastReal = true; beatNo = f.beat & 3; if (clockRun) anchorUs = tb; }
  else beat(tb, f.beat & 3, true);
  USBSerial.printf("B,%u,%.1f,%s,%lld,%u,%u,%d\n", (f.beat & 3) + 1, f.bpm10 / 10.0f, srcName(s),
                   (long long)(esp_timer_get_time() - tb), f.seq, f.hue, s == SRC_AIR ? f.rssi : 0);
}

// ================= bridge =================
uint8_t lastSeq = 0; bool seqKnown = false;
uint32_t heard = 0, sent = 0;
bool txBusy = false; int64_t txStartUs = 0; BeatPacket txPkt;
int64_t deferUs = 0;                                 // another bridge's heartbeat that we give way to
bool bridging() {
  if (!loraOk || bridgeMode == BR_OFF) return false;
  if (bridgeMode == BR_ON) return true;
  return !(deferUs && esp_timer_get_time() - deferUs < 3000000);
}
// a HELLO from another bridge: give way to Heltecs / T-Decks, and to Beat Nodes with a lower MAC
void noteHello(const LinkFrame& f, const uint8_t* mac, int64_t t) {
  otherHelloUs = t; otherRadio = f.flags & 1; otherHeard = f.count;
  if (f.flags & HELLO_W10) return;                   // the W10 steps aside for us, not the other way round
  bool node = f.flags & HELLO_NODE;
  if (!node || memcmp(mac, myMac, 6) < 0) deferUs = t;
}
void transmit(uint16_t b10, uint8_t no, uint8_t h) {
  txPkt = {'B', 'T', (uint8_t)(seqKnown ? lastSeq + 1 : esp_random() & 0xFF), b10, (uint8_t)(no & 3), h};
  loraSend(txPkt); txBusy = true; txStartUs = esp_timer_get_time();
}
// a beat for the link from the host (Tab5 "F,...") or a booth controller's FIRE
void fire(uint16_t b10, uint8_t no, uint8_t h, bool fromHost) {
  if (bridging()) { if (!txBusy) transmit(b10, no, h); return; }
  if (fromHost) {                                    // not the bridge: hand it to whoever is
    LinkFrame f = linkFrame(LF_FIRE); f.bpm10 = b10; f.beat = no & 3; f.hue = h;
    sendFrame(f); lastFireUs = esp_timer_get_time();
  }
}

// ================= commands =================
const char* modeName() { return bridgeMode == BR_ON ? "on" : bridgeMode == BR_OFF ? "off" : "auto"; }
void status() {
  int64_t now = esp_timer_get_time();
  const char* st = src == SRC_NONE ? "WAIT" : freeBeats ? "FREE" : "LOCK";
  const char* br = bridging() ? "OK" : otherHelloUs && now - otherHelloUs < 3000000 ? (otherRadio ? "OK" : "NORADIO") : "-";
  USBSerial.printf("S,%s,%.1f,%s,%lu,%s,%s,%lu\n", st, bpm10 / 10.0f, srcName(src), (unsigned long)beatsReal,
                   clockRun ? "RUN" : "STOP", br, (unsigned long)framesIn);
}
void info() {
  USBSerial.printf("# Beat Node v%s | LoRa %s, %d dBm | bridge %s (%s) | heard %lu sent %lu | MAC %s\n", FW_VERSION,
                   loraOk ? "OK" : "FAILED", txPower, modeName(), bridging() ? "bridging" : "standing by",
                   (unsigned long)heard, (unsigned long)sent, WiFi.macAddress().c_str());
}
void saveCfg() {
  prefs.putBool("notes", cfgNotes); prefs.putBool("transport", cfgTransport); prefs.putUChar("ch", cfgChannel);
  prefs.putUChar("bridge", bridgeMode); prefs.putChar("power", txPower);
}
void command(String c) {
  c.trim();
  if (c.startsWith("F,")) {
    int a = c.indexOf(',', 2), b = c.indexOf(',', a + 1);
    long b10 = c.substring(2, a).toInt();
    if (a > 0 && b > 0 && b10 >= 300 && b10 <= 3000) fire(b10, c.substring(a + 1, b).toInt(), c.substring(b + 1).toInt(), true);
    return;
  }
  c.toLowerCase();
  if (c == "?" || c == "status") { status(); info(); return; }
  if (c.startsWith("bridge ")) { bridgeMode = c.endsWith("on") ? BR_ON : c.endsWith("off") ? BR_OFF : BR_AUTO; }
  else if (c.startsWith("power ")) { int p = c.substring(6).toInt(); if (p >= 2 && p <= 22) { txPower = p; if (loraOk) setPower(); } }
  else if (c.startsWith("notes ")) cfgNotes = c.endsWith("1") || c.endsWith("on");
  else if (c.startsWith("transport ")) cfgTransport = c.endsWith("1") || c.endsWith("on");
  else if (c.startsWith("channel ")) { int ch = c.substring(8).toInt(); if (ch >= 1 && ch <= 16) cfgChannel = ch; }
  else { USBSerial.println("# commands: ?  bridge auto|on|off  power 2-22  notes on|off  transport on|off  channel 1-16  F,<bpm x10>,<beat 0-3>,<hue>"); return; }
  saveCfg(); info();
}

// ================= setup / loop =================
void setup() {
  pinMode(P_LED, OUTPUT_OPEN_DRAIN); ledRaw(false);
  pinMode(L_NSS, OUTPUT); digitalWrite(L_NSS, HIGH);
  pinMode(L_RXEN, OUTPUT); digitalWrite(L_RXEN, HIGH);
  pinMode(L_BUSY, INPUT); pinMode(L_DIO1, INPUT);
  prefs.begin("beatnode", false);
  cfgNotes = prefs.getBool("notes", true); cfgTransport = prefs.getBool("transport", true); cfgChannel = prefs.getUChar("ch", 16);
  bridgeMode = prefs.getUChar("bridge", BR_AUTO) % 3;
  txPower = prefs.getChar("power", 10); if (txPower < 2 || txPower > 22) txPower = 10;

  evQ = xQueueCreate(16, sizeof(Ev));
  netQ = xQueueCreate(16, sizeof(NetEv));
  MIDI.begin();
  USBSerial.setTxTimeoutMs(0);
  USBSerial.begin();
  USB.productName("Beat Gateway");                    // same name as the XIAO gateway: Beat Hub and the Tab5 find it
  USB.manufacturerName("Anubis Darkwatch");
  USB.begin();
  delay(1500);                                        // let the host finish with USB before the radios start
  SPI.begin(L_SCK, L_MISO, L_MOSI, L_NSS);
  loraOk = loraInit();
  bool ok = espnowInit();
  delay(200);
  USBSerial.printf("# Beat Gateway v%s (Beat Node, XIAO + Wio-SX1262) | ESP-NOW ch %d %s\n", FW_VERSION, LINK_ESPNOW_CH, ok ? "listening" : "FAILED");
  info();
  if (!loraOk) for (int i = 0; i < 6; i++) { ledRaw(true); delay(80); ledRaw(false); delay(80); }   // radio missing
}

void loop() {
  int64_t now = esp_timer_get_time();

  // ---- LoRa ----
  if (loraOk) {
    BeatPacket p; int r = 0; int8_t s4 = 0;
    int ev = loraPoll(p, r, s4);
    if (ev == 1 && txBusy) {                          // a beat went out: tell the booth, follow it ourselves
      txBusy = false; sent++; lastSeq = txPkt.seq; seqKnown = true;
      LinkFrame a = linkFrame(LF_ACK);
      a.seq = txPkt.seq; a.bpm10 = txPkt.bpm10; a.beat = txPkt.beat; a.hue = txPkt.hue; a.count = sent;
      sendFrame(a);
      myBeat = true; Ev e = {a, now}; handle(e); myBeat = false;
    } else if (ev == 2) {
      heard++; lastSeq = p.seq; seqKnown = true;
      LinkFrame rf = linkFrame(LF_RELAY);
      rf.seq = p.seq; rf.bpm10 = p.bpm10; rf.beat = p.beat; rf.hue = p.hue; rf.rssi = r; rf.snr4 = s4;
      if (bridging()) sendFrame(rf);
      Ev e = {rf, now}; handle(e);
    }
    if (txBusy && now - txStartUs > 200000) { txBusy = false; startRx(); }
  }

  // ---- ESP-NOW ----
  NetEv n;
  while (xQueueReceive(netQ, &n, 0) == pdTRUE) {
    const LinkFrame& f = n.e.f;
    if (f.type == LF_HELLO) { noteHello(f, n.mac, n.e.t); raw(f); continue; }
    if (f.type == LF_FIRE && bridging()) { raw(f); fire(f.bpm10, f.beat, f.hue, false); continue; }
    if (f.type == LF_RELAY && !(f.flags & LINK_FLAG_ECHO) && loraOk) { raw(f); continue; }   // we hear LoRa ourselves
    if (f.type == LF_ACK) { lastSeq = f.seq; seqKnown = true; }
    handle(n.e);
  }

  // ---- bridge heartbeat ----
  static int64_t lastHello = 0;
  if (bridging() && now - lastHello >= 1000000) {
    lastHello = now;
    LinkFrame h = linkFrame(LF_HELLO); h.flags = 1 | HELLO_NODE; h.count = heard;
    sendFrame(h);
  }

  // ---- gateway: MIDI clock, free-run ----
  while (clockRun && tickIdx < 24 && now >= anchorUs + (int64_t)periodUs * tickIdx / 24) { midiByte(0xF8); tickIdx++; }
  if (src != SRC_NONE && lastBeatUs && now > lastBeatUs + periodUs + periodUs / 4) {
    if (freeBeats < FREE_BEATS) {
      freeBeats++; beat(lastBeatUs + periodUs, (beatNo + 1) & 3, false);
      if (freeBeats == 1) USBSerial.println("# link quiet, free-running");
    } else {
      stopClock("beat lost"); src = SRC_NONE; lastBeatUs = 0; bpm10 = 0; freeBeats = 0;
      USBSerial.println("S,WAIT,0.0,-,0,STOP,-");
    }
  }
  if (noteOn && now >= noteOffUs) { MIDI.noteOff(noteOn, 0, cfgChannel); noteOn = 0; }
  ledStep(now);

  // ---- button (shares the LED pin): read it only while the LED is off ----
  static int64_t btnDown = 0, lastBtn = 0; static bool btnDone = false;
  if (!ledOffUs && !ledNextUs && now - lastBtn > 20000) {
    lastBtn = now;
    bool pressed = digitalRead(P_LED) == LOW;
    if (pressed && !btnDown) { btnDown = now; btnDone = false; }
    if (!pressed) btnDown = 0;
    if (pressed && !btnDone && now - btnDown > 2000000) {
      btnDone = true; bridgeMode = (bridgeMode + 1) % 3; saveCfg(); info();
      led(150, bridgeMode + 1);
    }
  }

  static int64_t lastStatus = 0, lastIdle = 0;
  if (now - lastStatus >= 1000000) { lastStatus = now; status(); }
  if (src == SRC_NONE && now - lastIdle >= 2000000) { lastIdle = now; led(8); }

  static String line;
  while (USBSerial.available()) {
    char c = USBSerial.read();
    if (c == '\n' || c == '\r') { if (line.length()) command(line); line = ""; }
    else if (line.length() < 40) line += c;
  }
}
