// M5Dial Beat Dial: a handheld takeover controller for the W10 Beat Conductor.
// Board: M5Stack M5Dial (ESP32-S3 StampS3, 1.28" round 240x240 touch, rotary encoder, 13.56 MHz RFID).
//
// Talks ESP-NOW to the same Heltec V3 bridge as the CYD console, so it needs no radio of its own.
// Following: the ring flashes in the conductor's colour on every beat, BPM in the middle.
// TAKE: the Dial keeps time itself and the bridge transmits each beat on LoRa, so every follower keeps going.
//
// Controls
//   Scan an enrolled tag / implant  = TAKE (authorised). With the lock off, press the knob instead.
//   Turn                             = +/- 0.1 BPM (spin fast for 0.5 BPM steps)
//   Press + turn                     = nudge the beat 10 ms earlier / later per click
//   Tap the screen                   = tap tempo (first tap = downbeat)
//   Press the knob                   = RELEASE: hand the beat back to the W10 (locks again)
//   Hold the knob while powering up  = enroll tags again / switch the lock off
//
// While the Dial has the beat it also re-broadcasts each beat the bridge sends (flagged as an echo), so a
// CYD console in follow mode keeps showing the booth's tempo.
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x, board "M5Dial". Libraries: M5Dial (pulls in M5Unified + M5GFX).

#include <M5Dial.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "link.h"

#define FW_VERSION    "v1.0"

M5Canvas cv(&M5Dial.Display);   // full-screen frame buffer, drawn by ui.h
bool lockOn = true;             // tag lock (UI shows it)
#include "ui.h"


// ---------- options ----------
#define LINK_AIR_MS    11     // in TAKE, flash this long after sending: matches the followers (LoRa airtime)
#define FLYWHEEL_MS    4000   // keep the beat going this long after the last packet
#define ENC_PER_CLICK  4      // encoder counts per detent
#define BRIGHTNESS     160    // 0..255
#define MAX_TAGS       4      // enrolled tags / implants
#define CLICK_SOUND    1      // short tick on controls and scans

// ================= shared state =================
struct Tag { uint8_t len; uint8_t uid[10]; };
struct Core {
  uint8_t mode = M_FOLLOW;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, lastFireMs = 0, bars = 0;
  int rssi = 0; float snr = 0; int nudgeMs = 0;
  uint32_t bridgeMs = 0, conflictUntil = 0; bool radioOk = false, bridgeSeen = false;
  uint32_t flashSeq = 0, flashMs = 0; bool flashDown = false; bool viaBooth = false;
  uint32_t tapTimes[5]; int tapCount = 0;
  float frac = 0;
  uint8_t toast = T_NONE; uint32_t toastUntil = 0;
  uint32_t scanUntil = 0;           // knob pressed while locked: poll the reader fast until then
};
Core core;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t rxQ;
Preferences prefs;
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

Tag tags[MAX_TAGS]; int nTags = 0;

static inline float periodMs(float bpm) { return 60000.0f / (bpm > 30 ? bpm : 120); }
static uint32_t stepMs() { float a = core.frac + periodMs(core.bpm); uint32_t s = (uint32_t)a; core.frac = a - s; return s; }

void tick(uint16_t hz = 6000, uint32_t ms = 12) {
#if CLICK_SOUND
  M5Dial.Speaker.tone(hz, ms);
#endif
}

// ================= beat engine (runs inside the control task, mux held) =================
void beatAt(uint32_t t, bool down, bool booth) {
  core.flashSeq++; core.flashMs = t; core.flashDown = down; core.lastBeatMs = t; core.viaBooth = booth;
  if (down) core.bars++;
}
void setToast(uint8_t t, uint32_t now, uint32_t ms) { core.toast = t; core.toastUntil = now + ms; }

void takeOver(uint32_t now) {
  if (core.mode == M_TAKE) return;
  core.mode = M_TAKE; core.fires = core.acks = 0; core.nudgeMs = 0; core.tapCount = 0; core.scanUntil = 0;
  if (core.bpm < 60 || core.bpm > 200) core.bpm = 120;
  float p = periodMs(core.bpm);
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {   // seamless: continue the current beat grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
}
void release(uint32_t now) {
  if (core.mode != M_TAKE) return;
  core.mode = M_FOLLOW; core.gotAny = true;
  core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;   // HOLD: flywheel until the W10 is back
  core.conflictUntil = 0;
}
void tapTempo(uint32_t now) {
  takeOver(now);
  if (core.tapCount && now - core.tapTimes[core.tapCount - 1] > 2000) core.tapCount = 0;
  if (core.tapCount == 5) { memmove(core.tapTimes, core.tapTimes + 1, 4 * sizeof(uint32_t)); core.tapCount = 4; }
  core.tapTimes[core.tapCount++] = now;
  if (core.tapCount >= 2) {
    float avg = (float)(core.tapTimes[core.tapCount - 1] - core.tapTimes[0]) / (core.tapCount - 1);
    core.bpm = constrain(60000.0f / avg, 60.0f, 200.0f);
  }
  if (core.tapCount == 1) core.beatPos = 3;
  core.nextBeat = now; core.nudgeMs = 0;
}

void handleFrame(const LinkFrame& f, uint32_t now) {
  if (f.type == LF_FIRE) {                               // another controller is driving the bridge
    if (core.mode == M_TAKE) core.conflictUntil = now + 1500;
    return;
  }
  core.bridgeMs = now; core.bridgeSeen = true;
  if (f.type == LF_HELLO) { core.radioOk = f.flags & 1; return; }
  bool booth = f.type == LF_ACK || (f.flags & LINK_FLAG_ECHO);
  if (core.mode == M_TAKE) {
    if (f.type == LF_ACK) core.acks++;
    else if (f.type == LF_RELAY && !(f.flags & LINK_FLAG_ECHO)) core.conflictUntil = now + 1500;   // W10 still on air
    return;
  }
  if (f.type != LF_RELAY && f.type != LF_ACK) return;
  // following: a beat heard on LoRa (RELAY), or one the bridge just sent for another controller (ACK / echo)
  if (core.gotAny && f.seq == core.lastSeq && now - core.lastRxMs < 250) return;   // same beat twice
  if (f.type == LF_RELAY && !booth) { core.radioOk = true; core.rssi = f.rssi; core.snr = f.snr4 / 4.0f; }
  if (core.gotAny) { uint8_t gap = f.seq - core.lastSeq; if (gap > 1 && gap < 64) core.lost += gap - 1; }
  core.lastSeq = f.seq; core.rx++;
  core.gotAny = true; core.lastRxMs = now;
  core.bpm = f.bpm10 / 10.0f; core.beatPos = f.beat & 3; core.hue = f.hue;
  core.flyNext = now + (uint32_t)periodMs(core.bpm) + 40;
  beatAt(now, core.beatPos == 0, booth);
}

void onFrame(const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  LinkFrame f; memcpy(&f, d, sizeof(f));
  xQueueSend(rxQ, &f, 0);
}

// ================= RFID =================
bool sameTag(const Tag& a, const Tag& b) { return a.len == b.len && memcmp(a.uid, b.uid, a.len) == 0; }
bool readTag(Tag& t) {
  if (!M5Dial.Rfid.PICC_IsNewCardPresent() || !M5Dial.Rfid.PICC_ReadCardSerial()) return false;
  t.len = min<uint8_t>(M5Dial.Rfid.uid.size, 10);
  memcpy(t.uid, M5Dial.Rfid.uid.uidByte, t.len);
  M5Dial.Rfid.PICC_HaltA();
  return true;
}
bool tagAllowed(const Tag& t) { for (int i = 0; i < nTags; i++) if (sameTag(t, tags[i])) return true; return false; }
void printTag(const char* what, const Tag& t) {
  Serial.printf("# %s tag ", what); for (int i = 0; i < t.len; i++) Serial.printf("%02X", t.uid[i]); Serial.println();
}

// ================= control task: link, encoder, knob, touch, reader, beat timing =================
void controlTask(void*) {
  int32_t encLast = M5Dial.Encoder.read();
  uint32_t lastTouch = 0, lastScan = 0, lastDetent = 0;
  bool nudged = false;
  for (;;) {
    uint32_t now = millis();
    LinkFrame in[8]; int nIn = 0;
    while (nIn < 8 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;

    // inputs (I2C reads happen here, outside the lock)
    bool tapped = false, knobClick = false;
    if (now - lastTouch >= 5) {                          // button / touch events are only valid right after update()
      lastTouch = now;
      M5Dial.update();
      tapped = M5Dial.Touch.getDetail().wasPressed();
      if (M5Dial.BtnA.wasPressed()) nudged = false;
      knobClick = M5Dial.BtnA.wasReleased() && !nudged;   // a press used for nudging is not a click
    }
    bool knobDown = M5Dial.BtnA.isPressed();
    int32_t enc = M5Dial.Encoder.read();
    int clicks = (enc - encLast) / ENC_PER_CLICK;
    if (clicks) encLast += clicks * ENC_PER_CLICK;

    portENTER_CRITICAL(&mux);
    uint8_t mode = core.mode; bool scanning = (int32_t)(core.scanUntil - now) > 0;
    portEXIT_CRITICAL(&mux);

    // reader: only while following (a read blocks for a few ms; in TAKE that would delay beats)
    Tag tag; bool gotTag = false;
    if (lockOn && mode == M_FOLLOW && now - lastScan >= (scanning ? 60u : 400u)) {
      lastScan = now; gotTag = readTag(tag);
    }

    bool fire = false, echo = false; LinkFrame fr, ef; int sound = 0;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < nIn; i++) {
      handleFrame(in[i], now);
      if (core.mode == M_TAKE && in[i].type == LF_ACK) {   // mirror our beat for consoles in follow mode
        ef = in[i]; ef.type = LF_RELAY; ef.flags = LINK_FLAG_ECHO; ef.rssi = 0; ef.snr4 = 0; ef.count = 0; echo = true;
      }
    }
    if (gotTag) {
      if (tagAllowed(tag)) { takeOver(now); setToast(T_GRANTED, now, 1200); sound = 2; }
      else { setToast(T_DENIED, now, 1500); sound = 3; }
    }
    if (core.mode == M_TAKE) {
      if (knobClick) { release(now); sound = 1; }
      else if (clicks) {
        if (knobDown) { core.nextBeat += clicks * 10; core.nudgeMs += clicks * 10; nudged = true; }
        else {
          float step = (now - lastDetent < 35) ? 0.5f : 0.1f;
          core.bpm = constrain(roundf((core.bpm + clicks * step) * 10) / 10, 60.0f, 200.0f);
        }
        lastDetent = now;
      }
      if (tapped) { tapTempo(now); sound = 1; }
    } else {
      if (knobClick) {
        if (!lockOn) { takeOver(now); sound = 1; }
        else { core.scanUntil = now + 6000; setToast(T_SCAN, now, 6000); }
      }
      if (tapped) {
        if (!lockOn) { tapTempo(now); sound = 1; }
        else { core.scanUntil = now + 6000; setToast(T_SCAN, now, 6000); }
      }
      if (clicks && core.toast != T_SCAN) setToast(T_TURN, now, 1500);
    }

    if (core.mode == M_TAKE) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        float p = periodMs(core.bpm);
        if (now - core.nextBeat > p) core.nextBeat = now;   // fell far behind: resync
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;              // new colour every bar, like the W10
        core.nextBeat += stepMs();
        core.fires++; core.lastFireMs = now;
        fr = linkFrame(LF_FIRE);
        fr.bpm10 = (uint16_t)(core.bpm * 10 + 0.5f); fr.beat = core.beatPos; fr.hue = core.hue;
        fire = true;
        beatAt(now + LINK_AIR_MS, core.beatPos == 0, true);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS &&
               (int32_t)(now - core.flyNext) >= 0) {        // packet missed: keep the beat going
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0, core.viaBooth);
      core.flyNext += stepMs();
    }
    portEXIT_CRITICAL(&mux);

    if (fire) esp_now_send(BCAST, (const uint8_t*)&fr, sizeof(fr));
    if (echo) esp_now_send(BCAST, (const uint8_t*)&ef, sizeof(ef));
    if (gotTag) printTag(sound == 2 ? "granted" : "denied", tag);
    if (sound == 1) tick();
    else if (sound == 2) { tick(2800, 60); }
    else if (sound == 3) { tick(400, 250); }
    else if (clicks && mode == M_TAKE) tick(9000, 4);
    vTaskDelay(1);
  }
}

// ================= display =================
void snapshot(View& v, uint32_t now) {
  portENTER_CRITICAL(&mux);
  v.mode = core.mode;
  v.bridgeOk = core.bridgeSeen && now - core.bridgeMs < 3000;
  v.radioOk = core.radioOk;
  v.conflict = core.mode == M_TAKE && (int32_t)(core.conflictUntil - now) > 0;
  if (core.mode == M_TAKE) v.link = LS_TAKE;
  else if (!core.gotAny) v.link = LS_WAIT;
  else if (now - core.lastRxMs < 1500) v.link = LS_LIVE;
  else if (now - core.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
  else v.link = LS_WAIT;
  v.bpm = core.bpm; v.beatPos = core.beatPos; v.hue = core.hue;
  v.rssi = core.rssi; v.snr = core.snr; v.rx = core.rx; v.lost = core.lost;
  v.sent = core.acks; v.bars = core.bars;
  uint32_t pend = (core.fires > core.acks && now - core.lastFireMs < 80) ? 1 : 0;
  v.fail = core.fires - core.acks - pend;
  v.nudgeMs = core.nudgeMs; v.viaBooth = core.viaBooth; v.flashDown = core.flashDown; v.flashMs = core.flashMs;
  v.toast = (int32_t)(core.toastUntil - now) > 0 ? core.toast : T_NONE;
  if (core.toast == T_SCAN && (core.mode == M_TAKE || (int32_t)(core.scanUntil - now) <= 0)) v.toast = T_NONE;
  portEXIT_CRITICAL(&mux);
}

// ================= tags (NVS) =================
void loadTags() {
  lockOn = prefs.getUChar("lock", 1);
  nTags = prefs.getBytes("tags", tags, sizeof(tags)) / sizeof(Tag);
  if (nTags > MAX_TAGS) nTags = 0;
}
void saveTags() { prefs.putUChar("lock", lockOn); prefs.putBytes("tags", tags, nTags * sizeof(Tag)); prefs.putUChar("setup", 1); }

// enroll: scan up to MAX_TAGS tags; press the knob to finish (with none scanned = lock off)
void enroll() {
  Serial.println("# enroll: scan tags, press the knob to finish (none = lock off)");
  nTags = 0;
  while (M5Dial.BtnA.isPressed()) { M5Dial.update(); delay(10); }
  page("ENROLL", COL_PURP, "Scan your implant", "press knob = no lock", ""); cv.pushSprite(0, 0);
  while (true) {
    M5Dial.update();
    if (M5Dial.BtnA.wasClicked() || M5Dial.BtnA.wasReleased()) break;
    Tag t;
    if (nTags < MAX_TAGS && readTag(t)) {
      bool dup = false; for (int i = 0; i < nTags; i++) if (sameTag(t, tags[i])) dup = true;
      if (!dup) { tags[nTags++] = t; printTag("enrolled", t); }
      tick(2800, 60);
      char l1[24]; snprintf(l1, sizeof(l1), "%d tag%s enrolled", nTags, nTags == 1 ? "" : "s");
      page("ENROLL", COL_GRN, l1, nTags < MAX_TAGS ? "scan another, or" : "", "press knob = done"); cv.pushSprite(0, 0);
      delay(600);
    }
    delay(40);
  }
  lockOn = nTags > 0;
  saveTags();
  page(lockOn ? "LOCKED" : "OPEN", lockOn ? COL_GRN : COL_AMB,
       lockOn ? "Scan to take the beat" : "Knob takes the beat", lockOn ? "" : "no tag needed", ""); cv.pushSprite(0, 0);
  Serial.printf("# lock %s, %d tag(s)\n", lockOn ? "ON" : "OFF", nTags);
  delay(1500);
}

bool linkInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) { onFrame(d, len); });
  return true;
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, true);                       // encoder + RFID
  Serial.begin(115200);
  Serial.println("# M5Dial Beat Dial " FW_VERSION);
  M5Dial.Display.setBrightness(BRIGHTNESS);
  M5Dial.Speaker.setVolume(120);
  cv.setColorDepth(16); cv.setPsram(false);
  if (!cv.createSprite(W, W)) Serial.println("# sprite alloc failed");
  initColors();
  splash(); cv.pushSprite(0, 0);

  prefs.begin("beatdial", false);
  loadTags();
  uint32_t t0 = millis(); bool held = false;
  while (millis() - t0 < 1500) { M5Dial.update(); if (M5Dial.BtnA.isPressed()) held = true; delay(10); }
  if (held || !prefs.getUChar("setup", 0)) enroll();

  rxQ = xQueueCreate(16, sizeof(LinkFrame));
  if (!linkInit()) Serial.println("# ESP-NOW init failed");
  Serial.printf("# lock %s, %d tag(s) | ESP-NOW ch %d net 0x%02X | MAC %s\n", lockOn ? "ON" : "OFF", nTags,
                LINK_ESPNOW_CH, LINK_NET_ID, WiFi.macAddress().c_str());
  xTaskCreatePinnedToCore(controlTask, "control", 6144, nullptr, 5, nullptr, 0);
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  if (now - lastDraw < 16) { delay(1); return; }        // ~60 fps cap
  lastDraw = now;
  View v; snapshot(v, now);
  drawFrame(v, now);
  cv.pushSprite(0, 0);
}
