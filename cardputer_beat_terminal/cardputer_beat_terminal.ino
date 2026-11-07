// Beat Commander terminal on the M5Stack Cardputer ADV (also runs on the original Cardputer).
// A keyboard booth controller: follows the beat link like the CYD and the Dial, and can take command of the beat.
// Talks ESP-NOW directly (channel 1, same link as the rest of Beat Commander); the Heltec bridge puts its beats on LoRa.
//
// Keys
//   T or G0 button   take over / release        R      release
//   SPACE            tap tempo (takes over)      H      help
//   128.5 ENTER      take over at a typed BPM (60-200). DEL fixes a digit, ` (esc) cancels.
//   - / =            tempo -/+ 0.1 BPM (with shift or fn: -/+ 1)
//   [ / ]            nudge the beat 10 ms early / late
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, board package "M5Stack" 3.x, board "M5Cardputer". Libraries: M5Cardputer (1.1+), M5Unified, M5GFX.

#include <M5Cardputer.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "link.h"
#include "ui.h"

#define FW_VERSION   "1.0"
#define LINK_AIR_MS  11     // in TAKE, flash this long after sending (ESP-NOW + LoRa airtime): matches the followers
#define FLYWHEEL_MS  4000   // keep the beat going this long after the last packet
#define CLICK_SOUND  1

// ================= ESP-NOW =================
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t rxQ;
bool radioUp = false;

void onFrame(const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  LinkFrame f; memcpy(&f, d, sizeof(f));
  xQueueSend(rxQ, &f, 0);
}
bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
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
void sendFrame(const LinkFrame& f) { esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f)); }

// ================= shared state (control task <-> UI) =================
struct Core {
  uint8_t mode = M_FOLLOW;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, lastFireMs = 0, bars = 0;
  int rssi = 0; int nudgeMs = 0;
  uint32_t bridgeMs = 0, conflictUntil = 0; bool bridgeSeen = false;
  uint32_t flashMs = 0; bool flashDown = false, viaBooth = false;
  uint32_t tapTimes[5]; int tapCount = 0;
  float frac = 0;
  uint8_t toast = T_NONE; uint32_t toastUntil = 0;
};
Core core;
SemaphoreHandle_t lock;            // a mutex (not a spinlock): the beat maths uses floats

enum KeyCmd : uint8_t { K_TAKE_TOGGLE, K_RELEASE, K_TAP, K_SETBPM, K_TEMPO, K_NUDGE, K_FOLLOW_ONLY };
struct KeyEv { uint8_t cmd; float val; };
QueueHandle_t keyQ;

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
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {     // seamless: continue the current beat grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
  setToast(T_TAKE, now, 1200);
}
void release(uint32_t now) {
  if (core.mode != M_TAKE) return;
  core.mode = M_FOLLOW; core.gotAny = true;
  core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;    // HOLD: flywheel until the conductor is back
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

// returns true if this ACK should be mirrored for consoles (we sent the beat)
bool handleFrame(const LinkFrame& f, uint32_t now) {
  if (f.type == LF_FIRE) {                                  // another controller drives the bridge
    if (core.mode == M_TAKE) core.conflictUntil = now + 1500;
    return false;
  }
  core.bridgeMs = now; core.bridgeSeen = true;
  if (f.type == LF_HELLO) return false;
  bool echo = f.flags & LINK_FLAG_ECHO;
  if (core.mode == M_TAKE) {
    if (f.type == LF_ACK) { core.acks++; return true; }
    if (f.type == LF_RELAY && !echo) core.conflictUntil = now + 1500;   // the W10 is still on air
    return false;
  }
  if (f.type != LF_RELAY && f.type != LF_ACK) return false;
  if (core.gotAny && f.seq == core.lastSeq && now - core.lastRxMs < 250) return false;   // same beat twice
  bool booth = f.type == LF_ACK || echo;
  if (!booth) core.rssi = f.rssi;
  if (core.gotAny) { uint8_t gap = f.seq - core.lastSeq; if (gap > 1 && gap < 64) core.lost += gap - 1; }
  core.lastSeq = f.seq; core.rx++; core.gotAny = true; core.lastRxMs = now;
  core.bpm = f.bpm10 / 10.0f; core.beatPos = f.beat & 3; core.hue = f.hue;
  core.flyNext = now + (uint32_t)periodMs(core.bpm) + 40; core.frac = 0;
  beatAt(now, core.beatPos == 0, booth);
  return false;
}

void click(int kind) {
#if CLICK_SOUND
  if (kind == 1) M5Cardputer.Speaker.tone(6000, 10);
  else if (kind == 2) M5Cardputer.Speaker.tone(2800, 45);
#endif
}

// ================= control task: link, keys, beat timing =================
void controlTask(void*) {
  for (;;) {
    uint32_t now = millis();
    LinkFrame in[8]; int nIn = 0;
    while (nIn < 8 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;
    KeyEv ke[4]; int nKey = 0;
    while (nKey < 4 && xQueueReceive(keyQ, &ke[nKey], 0) == pdTRUE) nKey++;

    LinkFrame out[3]; int nOut = 0; int sound = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < nIn; i++)
      if (handleFrame(in[i], now) && nOut < 3) {            // our beat went out: mirror it for consoles
        LinkFrame e = in[i]; e.type = LF_RELAY; e.flags = LINK_FLAG_ECHO; e.rssi = 0; e.snr4 = 0; e.count = 0;
        out[nOut++] = e;
      }
    for (int i = 0; i < nKey; i++) {
      switch (ke[i].cmd) {
        case K_TAKE_TOGGLE: if (core.mode == M_TAKE) release(now); else takeOver(now); sound = 2; break;
        case K_RELEASE: release(now); sound = 2; break;
        case K_TAP: tapTempo(now); sound = 1; break;
        case K_SETBPM:
          if (ke[i].val < 60 || ke[i].val > 200) { setToast(T_BPM_RANGE, now, 1200); break; }
          takeOver(now); core.bpm = roundf(ke[i].val * 10) / 10; sound = 2; break;
        case K_TEMPO:
          if (core.mode != M_TAKE) { setToast(T_FOLLOW_ONLY, now, 1200); break; }
          core.bpm = constrain(roundf((core.bpm + ke[i].val) * 10) / 10, 60.0f, 200.0f); sound = 1; break;
        case K_NUDGE:
          if (core.mode != M_TAKE) { setToast(T_FOLLOW_ONLY, now, 1200); break; }
          core.nextBeat += (int)ke[i].val; core.nudgeMs += (int)ke[i].val; sound = 1; break;
      }
    }
    if (core.mode == M_TAKE) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        if (now - core.nextBeat > periodMs(core.bpm)) core.nextBeat = now;   // fell far behind: resync
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;                // new colour every bar, like the W10
        core.nextBeat += stepMs();
        core.fires++; core.lastFireMs = now;
        LinkFrame fr = linkFrame(LF_FIRE);
        fr.bpm10 = (uint16_t)(core.bpm * 10 + 0.5f); fr.beat = core.beatPos; fr.hue = core.hue;
        if (nOut < 3) out[nOut++] = fr;
        beatAt(now + LINK_AIR_MS, core.beatPos == 0, true);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS &&
               (int32_t)(now - core.flyNext) >= 0) {          // packet missed: keep the beat going
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0, core.viaBooth);
      core.flyNext += stepMs();
    }
    xSemaphoreGive(lock);

    for (int i = 0; i < nOut; i++) sendFrame(out[i]);
    if (sound) click(sound);
    vTaskDelay(1);
  }
}

// ================= keyboard + screen =================
M5Canvas cv(&M5Cardputer.Display);
char input[8] = "";
bool help = false;

void queueKey(uint8_t cmd, float val = 0) { KeyEv e = {cmd, val}; xQueueSend(keyQ, &e, 0); }

void keys() {
  if (M5Cardputer.BtnA.wasPressed()) queueKey(K_TAKE_TOGGLE);
  auto& kb = M5Cardputer.Keyboard;
  if (!kb.isChange() || !kb.isPressed()) return;
  auto st = kb.keysState();
  bool big = st.shift || st.fn;
  if (st.enter) {
    if (input[0]) { queueKey(K_SETBPM, atof(input)); input[0] = 0; }
    return;
  }
  if (st.del) { size_t n = strlen(input); if (n) input[n - 1] = 0; return; }
  if (st.space) { queueKey(K_TAP); return; }
  for (char c : st.word) {
    size_t n = strlen(input);
    if ((c >= '0' && c <= '9') || (c == '.' && n && !strchr(input, '.'))) {
      if (n < 5) { input[n] = c; input[n + 1] = 0; }
      continue;
    }
    if (c == '`' || c == '~') { input[0] = 0; help = false; continue; }
    switch (c) {
      case 't': case 'T': queueKey(K_TAKE_TOGGLE); break;
      case 'r': case 'R': queueKey(K_RELEASE); break;
      case 'h': case 'H': help = !help; break;
      case '-': case '_': queueKey(K_TEMPO, big || c == '_' ? -1.0f : -0.1f); break;
      case '=': case '+': queueKey(K_TEMPO, big || c == '+' ? 1.0f : 0.1f); break;
      case '[': case '{': queueKey(K_NUDGE, -10); break;
      case ']': case '}': queueKey(K_NUDGE, 10); break;
    }
  }
}

void snapshot(View& v, uint32_t now) {
  v = {};
  xSemaphoreTake(lock, portMAX_DELAY);
  v.mode = core.mode;
  v.bridgeOk = core.bridgeSeen && now - core.bridgeMs < 3000;
  v.conflict = core.mode == M_TAKE && (int32_t)(core.conflictUntil - now) > 0;
  if (!radioUp) v.link = LS_NORADIO;
  else if (core.mode == M_TAKE) v.link = LS_TAKE;
  else if (!core.gotAny) v.link = LS_WAIT;
  else if (now - core.lastRxMs < 1500) v.link = LS_LIVE;
  else if (now - core.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
  else v.link = LS_WAIT;
  v.bpm = core.bpm; v.beatPos = core.beatPos; v.hue = core.hue; v.bars = core.bars;
  v.rssi = core.rssi; v.rx = core.rx; v.lost = core.lost; v.sent = core.acks;
  uint32_t pend = (core.fires > core.acks && now - core.lastFireMs < 80) ? 1 : 0;
  v.fail = core.fires > core.acks + pend ? core.fires - core.acks - pend : 0;
  v.nudgeMs = core.nudgeMs; v.viaBooth = core.viaBooth; v.flashDown = core.flashDown; v.flashMs = core.flashMs;
  v.toast = (int32_t)(core.toastUntil - now) > 0 ? core.toast : T_NONE;
  xSemaphoreGive(lock);
  static int bat = -1; static uint32_t batMs = 0;
  if (!batMs || now - batMs > 5000) { batMs = now | 1; bat = M5Cardputer.Power.getBatteryLevel(); }
  v.battery = bat;
  v.input = input; v.help = help;
}

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // anti-aliased glyphs are copied onto the stack while drawing

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  Serial.begin(115200);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(180);
  M5Cardputer.Speaker.setVolume(90);
  initFonts();
  cv.setColorDepth(16);
  cv.createSprite(SW, SH);
  lock = xSemaphoreCreateMutex();
  rxQ = xQueueCreate(24, sizeof(LinkFrame));
  keyQ = xQueueCreate(8, sizeof(KeyEv));
  radioUp = espnowInit();
  Serial.printf("# Beat Commander terminal v%s | ESP-NOW %s | MAC %s\n", FW_VERSION, radioUp ? "up" : "FAILED",
                WiFi.macAddress().c_str());
  xTaskCreatePinnedToCore(controlTask, "control", 4096, nullptr, 5, nullptr, 0);
}

void loop() {
  static uint32_t lastDraw = 0;
  M5Cardputer.update();
  keys();
  uint32_t now = millis();
  if (now - lastDraw < 25) { delay(2); return; }     // ~40 fps
  lastDraw = now;
  View v; snapshot(v, now);
  draw(cv, v, now);
  cv.pushSprite(0, 0);
}
