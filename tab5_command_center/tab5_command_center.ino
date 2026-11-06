// Beat Command Center on the M5Stack Tab5 (ESP32-P4, 5" 1280x720 touch).
// The big booth screen for the W10 Beat Conductor: follows the beat link and can take it over.
//
// Radio: the Tab5's own Wi-Fi chip can't do ESP-NOW, so the Beat Gateway (XIAO ESP32-S3, firmware v1.1+) plugs
// into the Tab5's USB-A port and acts as its radio. Every frame on the link arrives as a text line; in TAKE the
// Tab5 sends "F,<bpm x10>,<beat>,<hue>" per beat and the Heltec bridge puts it on LoRa for every follower.
//
// Screen
//   Header  : status (LIVE / BOOTH / HOLD / TAKEOVER / CLASH / NO GATEWAY), gateway + bridge, battery
//   Left    : TAKE / RELEASE, TAP, tempo -/+ 0.1 BPM (hold to repeat), nudge 10 ms early / late
//   Center  : beat ring (beat 1 at the top) flashing in the conductor's colour, BPM, bar count, 2-minute tempo graph
//   Right   : source, RSSI / SNR, beats in, lost, sent, nudge, gateway
//   Hold the header for 2 s to turn the screen upside down (remembered).
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, board package "M5Stack" 3.3.x, board "M5Tab5". Libraries: M5Unified (0.2.23+), M5GFX (0.2.30+).

#include <M5Unified.h>
#include <Preferences.h>
#include "ui.h"
#include "usbgw.h"
#include <esp_system.h>

#define FW_VERSION   "1.0.1"

// ---------- options ----------
#define LINK_AIR_MS    12     // in TAKE, flash this long after sending (USB + ESP-NOW + LoRa airtime): matches followers
#define FLYWHEEL_MS    4000   // keep the beat going this long after the last packet
#define BRIGHTNESS     200    // 0..255
#define CLICK_SOUND    1

// ================= frames from the gateway =================
struct Frame { char type; uint8_t seq, beat, hue, echo, radio; uint16_t bpm10; int16_t rssi; float snr; uint32_t count; };
QueueHandle_t rxQ;
char gwVersion[40] = "";

static uint16_t b10(const char* s) { return (uint16_t)lroundf(atof(s) * 10); }

void onGatewayLine(const char* line) {           // runs in the USB task
  Frame f = {}; f.type = line[0];
  char buf[160]; strncpy(buf, line, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  char* p[10]; int n = 0;
  for (char* t = strtok(buf, ","); t && n < 10; t = strtok(nullptr, ",")) p[n++] = t;
  switch (f.type) {
    case 'R': if (n < 8) return; f.seq = atoi(p[1]); f.bpm10 = b10(p[2]); f.beat = atoi(p[3]); f.hue = atoi(p[4]);
              f.rssi = atoi(p[5]); f.snr = atof(p[6]); f.echo = atoi(p[7]); break;
    case 'A': if (n < 6) return; f.seq = atoi(p[1]); f.bpm10 = b10(p[2]); f.beat = atoi(p[3]); f.hue = atoi(p[4]); f.count = atol(p[5]); break;
    case 'F': if (n < 4) return; f.bpm10 = b10(p[1]); f.beat = atoi(p[2]); f.hue = atoi(p[3]); break;
    case 'H': if (n < 3) return; f.radio = atoi(p[1]); f.count = atol(p[2]); break;
    case '#':
      if (!strncmp(line, "# Beat Gateway v", 16)) { const char* e = strchr(line + 2, '|'); int k = e ? e - line - 3 : strlen(line + 2);
        k = min(k, (int)sizeof(gwVersion) - 1); memcpy(gwVersion, line + 2, k); gwVersion[k] = 0; }
      return;
    default: return;
  }
  xQueueSend(rxQ, &f, 0);
}

// ================= shared state (control task <-> UI) =================
struct Core {
  uint8_t mode = M_FOLLOW;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, lastFireMs = 0, bars = 0;
  int rssi = 0; float snr = 0; int nudgeMs = 0;
  uint32_t bridgeMs = 0, conflictUntil = 0; bool radioOk = false, bridgeSeen = false;
  uint32_t flashMs = 0; bool flashDown = false; bool viaBooth = false;
  uint32_t tapTimes[5]; int tapCount = 0;
  float frac = 0;
  uint8_t toast = T_NONE; uint32_t toastUntil = 0;
};
Core core;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t padQ;                               // pad presses from the UI loop
struct PadEv { uint8_t pad; bool repeat; };

static inline float periodMs(float bpm) { return 60000.0f / (bpm > 30 ? bpm : 120); }
static uint32_t stepMs() { float a = core.frac + periodMs(core.bpm); uint32_t s = (uint32_t)a; core.frac = a - s; return s; }

void beatAt(uint32_t t, bool down, bool booth) {
  core.flashMs = t; core.flashDown = down; core.lastBeatMs = t; core.viaBooth = booth;
  if (down) core.bars++;
}
void setToast(uint8_t t, uint32_t now, uint32_t ms) { core.toast = t; core.toastUntil = now + ms; }

void takeOver(uint32_t now) {
  if (core.mode == M_TAKE) return;
  core.mode = M_TAKE; core.fires = core.acks = 0; core.nudgeMs = 0; core.tapCount = 0;
  if (core.bpm < 60 || core.bpm > 200) core.bpm = 120;
  float p = periodMs(core.bpm);
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {   // seamless: continue the current beat grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
  setToast(T_TAKE, now, 1200);
}
void release(uint32_t now) {
  if (core.mode != M_TAKE) return;
  core.mode = M_FOLLOW; core.gotAny = true;
  core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;   // HOLD: flywheel until the W10 is back
  core.conflictUntil = 0; core.tapCount = 0;
  setToast(T_RELEASE, now, 1200);
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

void handleFrame(const Frame& f, uint32_t now) {
  if (f.type == 'F') {                                   // another controller is driving the bridge
    if (core.mode == M_TAKE) core.conflictUntil = now + 1500;
    return;
  }
  core.bridgeMs = now; core.bridgeSeen = true;
  if (f.type == 'H') { core.radioOk = f.radio; return; }
  bool booth = f.type == 'A' || f.echo;
  if (core.mode == M_TAKE) {
    if (f.type == 'A') core.acks++;
    else if (f.type == 'R' && !f.echo) core.conflictUntil = now + 1500;   // W10 still on air
    return;
  }
  // following: a beat heard on LoRa (R), or one the bridge just sent for another controller (A / echo)
  if (core.gotAny && f.seq == core.lastSeq && now - core.lastRxMs < 250) return;   // same beat twice
  if (f.type == 'R' && !booth) { core.radioOk = true; core.rssi = f.rssi; core.snr = f.snr; }
  if (core.gotAny) { uint8_t gap = f.seq - core.lastSeq; if (gap > 1 && gap < 64) core.lost += gap - 1; }
  core.lastSeq = f.seq; core.rx++;
  core.gotAny = true; core.lastRxMs = now;
  core.bpm = f.bpm10 / 10.0f; core.beatPos = f.beat & 3; core.hue = f.hue;
  core.flyNext = now + (uint32_t)periodMs(core.bpm) + 40;
  beatAt(now, core.beatPos == 0, booth);
}

void click(int kind) {
#if CLICK_SOUND
  if (kind == 1) M5.Speaker.tone(6000, 12);
  else if (kind == 2) M5.Speaker.tone(2800, 50);
#endif
}

// ================= control task: link + pads + beat timing =================
void controlTask(void*) {
  char cmd[40];
  for (;;) {
    uint32_t now = millis();
    Frame in[8]; int nIn = 0;
    while (nIn < 8 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;
    PadEv pe[4]; int nPad = 0;
    while (nPad < 4 && xQueueReceive(padQ, &pe[nPad], 0) == pdTRUE) nPad++;

    bool fire = false; uint16_t fb10 = 0; uint8_t fbeat = 0, fhue = 0; int sound = 0;
    portENTER_CRITICAL(&mux);
    for (int i = 0; i < nIn; i++) handleFrame(in[i], now);
    for (int i = 0; i < nPad; i++) {
      uint8_t p = pe[i].pad;
      if (p == PAD_TAKE) { if (core.mode == M_TAKE) release(now); else takeOver(now); sound = 2; }
      else if (p == PAD_TAP) { tapTempo(now); sound = 1; }
      else if (core.mode != M_TAKE) { if (!pe[i].repeat) setToast(T_FOLLOW_ONLY, now, 1200); }
      else if (p == PAD_MINUS || p == PAD_PLUS) {
        core.bpm = constrain(roundf((core.bpm + (p == PAD_PLUS ? 0.1f : -0.1f)) * 10) / 10, 60.0f, 200.0f);
        if (!pe[i].repeat) sound = 1;
      } else if (p == PAD_EARLY || p == PAD_LATE) {
        int d = p == PAD_LATE ? 10 : -10;
        core.nextBeat += d; core.nudgeMs += d; sound = 1;
      }
    }
    if (core.mode == M_TAKE) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        float p = periodMs(core.bpm);
        if (now - core.nextBeat > p) core.nextBeat = now;   // fell far behind: resync
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;              // new colour every bar, like the W10
        core.nextBeat += stepMs();
        core.fires++; core.lastFireMs = now;
        fb10 = (uint16_t)(core.bpm * 10 + 0.5f); fbeat = core.beatPos; fhue = core.hue; fire = true;
        beatAt(now + LINK_AIR_MS, core.beatPos == 0, true);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS &&
               (int32_t)(now - core.flyNext) >= 0) {        // packet missed: keep the beat going
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0, core.viaBooth);
      core.flyNext += stepMs();
    }
    portEXIT_CRITICAL(&mux);

    if (fire) { snprintf(cmd, sizeof(cmd), "F,%u,%u,%u\n", fb10, fbeat, fhue); gw::send(cmd); }
    if (sound) click(sound);
    vTaskDelay(1);
  }
}

// ================= UI =================
M5Canvas ps[P_COUNT];
uint32_t lastHash[P_COUNT];
float hist[HIST_N]; int histPos = 0; uint32_t histSec = 0;
uint8_t heldPad = PAD_NONE; uint32_t heldSince = 0, lastRepeat = 0;
uint32_t headHoldSince = 0; bool flipped = false;
Preferences prefs;
const char* resetNote = nullptr;

void snapshot(View& v, uint32_t now) {
  v = {};
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
  v.bpm = core.bpm; v.beatPos = core.beatPos; v.hue = core.hue; v.bars = core.bars;
  v.rssi = core.rssi; v.snr = core.snr; v.rx = core.rx; v.lost = core.lost; v.sent = core.acks;
  uint32_t pend = (core.fires > core.acks && now - core.lastFireMs < 80) ? 1 : 0;
  v.fail = core.fires > core.acks + pend ? core.fires - core.acks - pend : 0;
  v.nudgeMs = core.nudgeMs; v.viaBooth = core.viaBooth; v.flashDown = core.flashDown; v.flashMs = core.flashMs;
  v.tapCount = core.mode == M_TAKE && now - (core.tapCount ? core.tapTimes[core.tapCount - 1] : 0) < 2000 ? core.tapCount : 0;
  v.toast = (int32_t)(core.toastUntil - now) > 0 ? core.toast : T_NONE;
  portEXIT_CRITICAL(&mux);
  v.gwOk = gw::connected();
  if (!v.gwOk && v.mode != M_TAKE) v.link = LS_NOGW;
  v.gwName = gwVersion[0] ? gwVersion : gw::product();
  static int bat = -1; static bool chg = false; static uint32_t batMs = 0;
  if (!batMs || now - batMs > 3000) {                  // I2C reads: only every few seconds
    batMs = now | 1; bat = M5.Power.getBatteryLevel(); chg = M5.Power.isCharging() == m5::Power_Class::is_charging;
  }
  v.battery = bat; v.charging = chg;
  v.hist = hist; v.histPos = histPos;
  v.pressed = heldPad;
  v.note = resetNote;
}

void handleTouch(uint32_t now) {
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    heldPad = PAD_NONE;
    for (int i = 0; i < PAD_COUNT; i++) if (PAD[i].hit(t.x, t.y)) heldPad = i;
    if (heldPad != PAD_NONE) { PadEv e = {heldPad, false}; xQueueSend(padQ, &e, 0); heldSince = lastRepeat = now; }
    headHoldSince = PANEL[P_HEAD].hit(t.x, t.y) ? now : 0;
  }
  if (t.isPressed()) {
    if ((heldPad == PAD_MINUS || heldPad == PAD_PLUS) && now - heldSince > 400 && now - lastRepeat > 90) {
      PadEv e = {heldPad, true}; xQueueSend(padQ, &e, 0); lastRepeat = now;
    }
    if (headHoldSince && now - headHoldSince > 2000) {
      headHoldSince = 0; flipped = !flipped; prefs.putBool("flip", flipped);
      M5.Display.setRotation(M5.Display.getRotation() ^ 2);
      memset(lastHash, 0, sizeof(lastHash));
    }
  }
  if (t.wasReleased() || !t.isPressed()) { heldPad = PAD_NONE; headHoldSince = 0; }
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("# Beat Command Center (Tab5) v" FW_VERSION);
  switch (esp_reset_reason()) {                        // say why the last run ended, if it wasn't a normal start
    case ESP_RST_BROWNOUT: resetNote = "last restart: POWER DIP (brownout)"; break;
    case ESP_RST_PANIC:    resetNote = "last restart: CRASH (panic)"; break;
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: resetNote = "last restart: CRASH (watchdog)"; break;
    default: break;
  }
  if (resetNote) Serial.printf("# %s\n", resetNote);
  prefs.begin("beatcmd", false);
  flipped = prefs.getBool("flip", false);
  if (M5.Display.width() < M5.Display.height()) M5.Display.setRotation(M5.Display.getRotation() ^ 1);   // landscape
  if (flipped) M5.Display.setRotation(M5.Display.getRotation() ^ 2);
  M5.Display.setBrightness(BRIGHTNESS);
  M5.Display.fillScreen(C_BG);
  M5.Speaker.setVolume(110);
  initFonts();
  for (int i = 0; i < P_COUNT; i++) {
    ps[i].setColorDepth(16); ps[i].setPsram(true);
    if (!ps[i].createSprite(PANEL[i].w, PANEL[i].h)) Serial.printf("# sprite %d alloc failed\n", i);
  }
  rxQ = xQueueCreate(32, sizeof(Frame));
  padQ = xQueueCreate(8, sizeof(PadEv));
  M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);   // 5 V on the USB-A port for the gateway
  if (!gw::begin(onGatewayLine)) Serial.println("# USB host failed to start");
  xTaskCreatePinnedToCore(controlTask, "control", 6144, nullptr, 5, nullptr, 1);
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  M5.update();
  handleTouch(now);
  if (now - lastDraw < 16) { delay(1); return; }      // ~60 fps cap
  lastDraw = now;

  View v; snapshot(v, now);
  if (now / 1000 != histSec) {                         // one tempo sample a second
    histSec = now / 1000;
    hist[histPos] = (v.link == LS_LIVE || v.link == LS_TAKE || v.link == LS_HOLD) ? v.bpm : 0;
    histPos = (histPos + 1) % HIST_N; v.histPos = histPos;
    lastHash[P_CENTER] = 0;
  }
  uint32_t h[P_COUNT] = {hashHead(v), hashLeft(v), 0, hashRight(v)};
  // center: redraw while the beat flash fades, and whenever the beat / BPM / state changes
  uint32_t hc = mix(mix(mix(mix(2166136261u, v.beatPos), (uint32_t)lroundf(v.bpm * 10)), v.link), v.toast);
  hc = mix(hc, v.bars); hc = mix(hc, flashLevel(v, now) >> 3); hc = mix(hc, v.hue);
  h[P_CENTER] = hc;
  for (int i = 0; i < P_COUNT; i++) {
    if (h[i] == lastHash[i]) continue;
    lastHash[i] = h[i];
    switch (i) {
      case P_HEAD: drawHead(ps[i], v); break;
      case P_LEFT: drawLeft(ps[i], v); break;
      case P_CENTER: drawCenter(ps[i], v, now); break;
      case P_RIGHT: drawRight(ps[i], v); break;
    }
    ps[i].pushSprite(&M5.Display, PANEL[i].x, PANEL[i].y);
  }
}
