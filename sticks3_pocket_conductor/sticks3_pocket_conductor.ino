// M5StickS3 Pocket Conductor: the W10's mic beat engine in your pocket, on the M5Stack StickS3.
// Board: M5StickS3 (ESP32-S3-PICO-1-N8R8, 1.14" 135x240 ST7789, ES8311 mic, two buttons).
//
// CONDUCT: the onboard mic finds the beat (same engine as the W10: onset strength, autocorrelation tempo,
//          phase lock). ON AIR sends every beat over ESP-NOW to the Heltec V3 bridge, which puts it on the
//          LoRa beat link, so every follower, the CYD console and the M5Dial follow the StickS3.
// FOLLOW:  a pocket badge. The screen flashes in the conductor's colour on every beat.
//
// Buttons (KEY1 = big front button, KEY2 = side button)
//   KEY1 tap        = tap tempo (taps override the mic for 15 s, first tap = beat 1)      [CONDUCT]
//   KEY1 hold 1 s   = ON AIR on / off (beats go out through the bridge)                   [CONDUCT]
//   KEY2 tap        = mic gain: club / loud room / quiet room / max (remembered)          [CONDUCT]
//   KEY2 hold 1 s   = switch CONDUCT <-> FOLLOW (remembered)
// Switch the W10's LORA TX off before going ON AIR: two conductors on one link collide (CLASH warns you).
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x, board "ESP32S3 Dev Module": Flash 8MB, PSRAM "OPI PSRAM",
// USB CDC On Boot "Enabled". Library: M5Unified (pulls in M5GFX).

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include "link.h"

#define FW_VERSION    "v1.0"

// ---------- options ----------
#define LINK_AIR_MS    11      // flash this long after sending: matches the followers (LoRa airtime)
#define FLYWHEEL_MS    4000    // FOLLOW: keep the beat going this long after the last packet
#define BRIGHTNESS     140     // 0..255
#define TAP_LOCK_MS    15000   // taps override the mic this long

M5Canvas cv(&M5.Display);
#include "ui.h"
#include "beat_engine.h"

// ES8311 mic gain (reg 0x16, 6 dB per step): club 6 dB / loud room 12 dB / quiet room 18 dB / max 30 dB
const uint8_t GAIN_REG[4] = {1, 2, 3, 5};
const float   GAIN_GATE[4] = {0.0015f, 0.0025f, 0.004f, 0.008f};  // silence gate rises with the noise floor

Preferences prefs;
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t rxQ, micQ;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t mode = MODE_CONDUCT;
int gain = 1;
bool micOk = false, onAir = false;

// ---- conduct state (loop) ----
uint8_t hue = 160;
uint32_t flashMs = 0; bool flashDown = false; int beatPosShown = 3;
uint32_t sent = 0, acks = 0, conflictUntil = 0;

// ---- link / follow state (loop) ----
struct Follow {
  bool gotAny = false; uint8_t lastSeq = 0; float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  uint32_t lastRxMs = 0, flyNext = 0, lost = 0, rx = 0; int rssi = 0; bool viaBooth = false;
  uint32_t bridgeMs = 0; bool bridgeSeen = false; float frac = 0;
} fol;

// ================= mic: M5Unified records 8 ms hops into a small ring =================
#define NBUF 6
int16_t micBuf[NBUF][HOP];
volatile int micNext = 0;
struct Hop { int16_t* p; uint32_t t; };

void onMicBuffer(void*, void* data, size_t len) {     // capture task: a hop is ready
  Hop h = {(int16_t*)data, millis() - (uint32_t)(len * 1000 / AUD_RATE)};
  xQueueSend(micQ, &h, 0);
  int i = micNext; micNext = (i + 1) % NBUF;
  M5.Mic.record(micBuf[i], HOP, AUD_RATE);              // keep the queue full
}

void audioTask(void*) {
  Hop h;
  for (;;) {
    if (xQueueReceive(micQ, &h, portMAX_DELAY) == pdTRUE) processHop(h.p, HOP, h.t);
  }
}

void setGain(int g) {
  gain = g & 3; MIC_GATE = GAIN_GATE[gain];
  M5.In_I2C.writeRegister8(0x18, 0x16, GAIN_REG[gain], 400000);
  prefs.putUChar("gain", gain);
}

bool micStart() {
  auto mc = M5.Mic.config();
  mc.sample_rate = AUD_RATE; mc.noise_filter_level = 0; mc.magnification = 1;
  M5.Mic.config(mc);
  M5.Mic.setBufferReleaseCallback(nullptr, onMicBuffer);
  if (!M5.Mic.begin()) return false;
  setGain(gain);
  micNext = 2;
  return M5.Mic.record(micBuf[0], HOP, AUD_RATE) && M5.Mic.record(micBuf[1], HOP, AUD_RATE);
}

// ================= ESP-NOW =================
void onFrame(const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  LinkFrame f; memcpy(&f, d, sizeof(f));
  xQueueSend(rxQ, &f, 0);
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
void sendFrame(const LinkFrame& f) { esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f)); }

static inline float periodOf(float b) { return 60000.0f / (b > 30 ? b : 120); }

void handleFrame(const LinkFrame& f, uint32_t now) {
  if (f.type == LF_FIRE) {                               // another controller drives the bridge
    if (mode == MODE_CONDUCT && onAir) conflictUntil = now + 1500;
    return;
  }
  fol.bridgeMs = now; fol.bridgeSeen = true;
  if (f.type == LF_HELLO) return;
  bool echo = f.flags & LINK_FLAG_ECHO;
  if (mode == MODE_CONDUCT) {
    if (!onAir) return;
    if (f.type == LF_ACK) {                              // our beat went out: mirror it for consoles
      acks++;
      LinkFrame e = f; e.type = LF_RELAY; e.flags = LINK_FLAG_ECHO; e.rssi = 0; e.snr4 = 0; e.count = 0;
      sendFrame(e);
    } else if (f.type == LF_RELAY && !echo) conflictUntil = now + 1500;   // the W10 is still on air
    return;
  }
  // FOLLOW
  if (f.type != LF_RELAY && f.type != LF_ACK) return;
  if (fol.gotAny && f.seq == fol.lastSeq && now - fol.lastRxMs < 250) return;   // same beat twice
  bool booth = f.type == LF_ACK || echo;
  if (!booth) fol.rssi = f.rssi;
  if (fol.gotAny) { uint8_t gap = f.seq - fol.lastSeq; if (gap > 1 && gap < 64) fol.lost += gap - 1; }
  fol.gotAny = true; fol.lastSeq = f.seq; fol.lastRxMs = now; fol.rx++; fol.viaBooth = booth;
  fol.bpm = f.bpm10 / 10.0f; fol.beatPos = f.beat & 3; fol.hue = f.hue;
  fol.flyNext = now + (uint32_t)periodOf(fol.bpm) + 40; fol.frac = 0;
  flashMs = now; flashDown = fol.beatPos == 0;
}

// ================= buttons =================
struct Btn { uint32_t downAt = 0; bool held = false; };
Btn k1, k2;
uint32_t gainToastUntil = 0;

void buttons(uint32_t now) {
  auto& A = M5.BtnA; auto& B = M5.BtnB;
  if (A.wasPressed()) {
    k1.downAt = now; k1.held = false;
    if (mode == MODE_CONDUCT) tap(now);
  }
  if (A.isPressed() && !k1.held && now - k1.downAt > 1000 && mode == MODE_CONDUCT) {
    k1.held = true; onAir = !onAir; tapCount = 0; tapLock = 0;     // a hold isn't a tap
    sent = acks = 0; conflictUntil = 0;
    Serial.printf("# ON AIR %s\n", onAir ? "ON" : "OFF");
  }
  if (B.wasPressed()) { k2.downAt = now; k2.held = false; }
  if (B.isPressed() && !k2.held && now - k2.downAt > 1000) {
    k2.held = true;
    mode = mode == MODE_CONDUCT ? MODE_FOLLOW : MODE_CONDUCT; onAir = false;
    prefs.putUChar("mode", mode);
    Serial.printf("# mode %s\n", mode == MODE_CONDUCT ? "CONDUCT" : "FOLLOW");
  }
  if (B.wasReleased() && !k2.held && mode == MODE_CONDUCT) { setGain(gain + 1); gainToastUntil = now + 1200; }
}

// ================= main =================
void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = false;                             // the speaker shares the mic's I2S pins
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("# M5StickS3 Pocket Conductor " FW_VERSION);
  M5.Display.setRotation(0);                            // portrait, buttons below the screen
  M5.Display.setBrightness(BRIGHTNESS);
  initColors();
  cv.setColorDepth(16); cv.setPsram(true);
  if (!cv.createSprite(SW, SH)) { cv.setPsram(false); cv.createSprite(SW, SH); }
  splash(); cv.pushSprite(0, 0);

  prefs.begin("pocket", false);
  mode = prefs.getUChar("mode", MODE_CONDUCT) ? MODE_FOLLOW : MODE_CONDUCT;
  gain = prefs.getUChar("gain", 1) & 3;

  rxQ = xQueueCreate(16, sizeof(LinkFrame));
  micQ = xQueueCreate(NBUF, sizeof(Hop));
  if (!linkInit()) Serial.println("# ESP-NOW init failed");
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  micOk = micStart();
  Serial.printf("# display %dx%d\n", M5.Display.width(), M5.Display.height());
  Serial.printf("# mic %s | gain %d | mode %s | ESP-NOW ch %d net 0x%02X | MAC %s\n", micOk ? "ok" : "FAILED", gain,
                mode == MODE_CONDUCT ? "CONDUCT" : "FOLLOW", LINK_ESPNOW_CH, LINK_NET_ID, WiFi.macAddress().c_str());
  delay(900);
  nextBeat = millis() + 500;
}

void loop() {
  uint32_t now = millis();
  M5.update();
  buttons(now);

  LinkFrame f;
  while (xQueueReceive(rxQ, &f, 0) == pdTRUE) handleFrame(f, now);

  if (mode == MODE_CONDUCT) {
    { int32_t adj = phaseAdj; if (adj) { phaseAdj = 0; nextBeat += adj; } }
    if ((int32_t)(now - nextBeat) >= 0) {
      beatNum++;
      int bi = ((beatNum % 4) + 4) % 4;
      if (bi == 0) hue += 40;                            // new colour every bar, like the W10
      float period = 60000.0f / bpm;
      nextBeat += (uint32_t)period;
      if ((int32_t)(now - nextBeat) > 0) nextBeat = now + (uint32_t)period;   // catch up after stalls
      if (onAir) {
        LinkFrame fr = linkFrame(LF_FIRE);
        fr.bpm10 = (uint16_t)(bpm * 10 + 0.5f); fr.beat = bi; fr.hue = hue;
        sendFrame(fr); sent++;
      }
      flashMs = now + (onAir ? LINK_AIR_MS : 0); flashDown = bi == 0; beatPosShown = bi;
    }
  } else if (fol.gotAny && fol.bpm > 30 && now - fol.lastRxMs < FLYWHEEL_MS && (int32_t)(now - fol.flyNext) >= 0) {
    fol.beatPos = (fol.beatPos + 1) & 3;                 // packet missed: keep the beat going
    flashMs = now; flashDown = fol.beatPos == 0;
    float a = fol.frac + periodOf(fol.bpm); uint32_t s = (uint32_t)a; fol.frac = a - s; fol.flyNext += s;
  }

  static uint32_t lastDraw = 0, lastLog = 0, lastBat = 0; static int bat = -1;
  if (now - lastBat > 5000 || bat < 0) { lastBat = now; bat = M5.Power.getBatteryLevel(); }
  if (now - lastDraw >= 20) {                           // ~50 fps
    lastDraw = now;
    View v = {};
    v.mode = mode; v.battery = bat; v.flashMs = flashMs; v.flashDown = flashDown;
    v.bridgeOk = fol.bridgeSeen && now - fol.bridgeMs < 3000;
    v.toastGain = (int32_t)(gainToastUntil - now) > 0;
    if (mode == MODE_CONDUCT) {
      v.onAir = onAir; v.manual = now < tapLock; v.conflict = onAir && (int32_t)(conflictUntil - now) > 0;
      v.sent = acks; v.level = level; v.conf = conf; v.gain = gain; v.micOk = micOk;
      v.scope = onsetHistory; v.scopeOn = ohOnset; v.scopePos = ohPos; v.scopeN = SCOPE_N;
      v.bpm = bpm; v.beatPos = beatPosShown; v.hue = hue;
    } else {
      if (!fol.gotAny) v.link = LS_WAIT;
      else if (now - fol.lastRxMs < 1500) v.link = LS_LIVE;
      else if (now - fol.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
      else v.link = LS_WAIT;
      v.viaBooth = fol.viaBooth; v.rssi = fol.rssi; v.lost = fol.lost;
      v.bpm = fol.bpm; v.beatPos = fol.beatPos; v.hue = fol.hue;
    }
    drawFrame(v, now);
    cv.pushSprite(0, 0);
  }
  if (now - lastLog > 1000) {
    lastLog = now;
    if (mode == MODE_CONDUCT)
      Serial.printf("bpm %.1f  est %.1f  conf %.2f  onsets/s %d  level %.4f  gain %d  %s  %s  sent %lu ack %lu\n",
                    bpm, bpmEst, conf, onsetCount, level, gain, now < tapLock ? "TAP" : "MIC", onAir ? "ON AIR" : "local",
                    (unsigned long)sent, (unsigned long)acks);
    onsetCount = 0;
  }
  delay(1);
}
