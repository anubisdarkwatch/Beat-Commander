// Beat Embed: Beat Commander on the LilyGo T-Embed CC1101 (ESP32-S3, 1.9" 320x170, rotary knob, 8-LED ring, mic).
//
// FOLLOW   follows the beat link over ESP-NOW (channel 1): the LED ring and the screen flash on every beat in the
//          conductor's colour. Turn the knob for the pages: BEAT, then the 11 visuals (PULSE ... RAIN).
// COMMAND  press the knob to take command of the beat (seamless, on the same grid): the knob sets the tempo
//          (+/- 0.1 BPM a click, faster when you spin it), the side key taps it. Beats go out through the Heltec
//          bridge or the W10 relay. Press again to hand it back.
// CONDUCT  the onboard mic finds the beat (the W10's engine, same as the StickS3 Pocket Conductor). Press the knob
//          for ON AIR: every beat goes out through the bridge / W10 relay. Side key taps the tempo.
//
// Controls
//   knob turn            page (FOLLOW / CONDUCT) or tempo (COMMAND)
//   knob press           take command / hand back (FOLLOW) - ON AIR on / off (CONDUCT)
//   knob hold 1.5 s      AUTO visuals (new one every 8 bars)        knob hold 4 s   display fix (colour / flip / invert)
//   side key tap         tap tempo (takes command in FOLLOW)        side key hold 1.5 s   FOLLOW <-> CONDUCT
//   side key hold 4 s    mic gain (club / loud room / quiet room / max)
// While a button is held, the screen says what letting go will do. Everything is saved.
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x, board "ESP32S3 Dev Module", Flash 16MB, PSRAM "Disabled" (the mic's
// DMA must stay in internal RAM), USB CDC On Boot "Enabled". Library: LovyanGFX.

#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <driver/i2s_pdm.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "link.h"
#define SCOPE_N 160
#include "beat_engine.h"
#include "ui.h"

#define FW_VERSION  "1.0"
#define LINK_AIR_MS 11
#define FLYWHEEL_MS 4000

// ---------- T-Embed CC1101 pins (LilyGo utilities.h) ----------
#define P_PWR_EN 15          // peripherals power: high
#define P_KEY 6              // side key
#define P_ENC_A 4
#define P_ENC_B 5
#define P_ENC_KEY 0          // knob press (also BOOT)
#define P_RING 14            // 8 x WS2812
#define RING_N 8
#define P_MIC_CLK 39         // PDM mic
#define P_MIC_DATA 42
#define P_TFT_BL 21
#define P_TFT_CS 41
#define P_TFT_DC 16
#define P_SCK 11
#define P_MOSI 9
#define P_MISO 10
#define P_CC1101_CS 12       // also on the display's SPI bus: keep deselected
#define P_NRF_CS 44
#define P_SD_CS 13
#define P_SDA 8
#define P_SCL 18

// ================= display =================
class LGFX_Embed : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
public:
  void setup(bool bgr, bool invert) {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST; b.spi_mode = 0; b.freq_write = 40000000; b.freq_read = 16000000;
    b.spi_3wire = false; b.use_lock = true; b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = P_SCK; b.pin_mosi = P_MOSI; b.pin_miso = P_MISO; b.pin_dc = P_TFT_DC;
    _bus.config(b); _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = P_TFT_CS; p.pin_rst = -1; p.pin_busy = -1;
    p.panel_width = 170; p.panel_height = 320; p.offset_x = 35; p.offset_y = 0;
    p.readable = false; p.invert = invert; p.rgb_order = bgr; p.bus_shared = true;
    _panel.config(p);
    auto l = _light.config();
    l.pin_bl = P_TFT_BL; l.invert = false; l.freq = 12000; l.pwm_channel = 7;
    _light.config(l); _panel.setLight(&_light);
    setPanel(&_panel);
  }
};
LGFX_Embed tft;
LGFX_Sprite cv(&tft);
uint8_t panelMode = 0;                  // bit0 colour order, bit1 turn 180, bit2 invert off
volatile bool panelChanged = false;
void applyPanel() {
  tft.setup(panelMode & 1, !(panelMode & 4));
  tft.init();
  tft.setRotation(panelMode & 2 ? 1 : 3);   // landscape, knob on the right
  tft.setBrightness(200);
}

// ================= LED ring (RMT, 10 MHz) =================
static rmt_data_t ringSym[RING_N * 24];
bool ringOk = false;
void ringShow(const uint8_t* rgb) {
  if (!ringOk) return;
  int k = 0;
  for (int i = 0; i < RING_N; i++) {
    uint8_t c[3] = {rgb[i * 3 + 1], rgb[i * 3], rgb[i * 3 + 2]};   // G, R, B
    for (int j = 0; j < 3; j++) for (int bit = 7; bit >= 0; bit--) {
      bool one = (c[j] >> bit) & 1;
      ringSym[k].level0 = 1; ringSym[k].duration0 = one ? 8 : 4;
      ringSym[k].level1 = 0; ringSym[k].duration1 = one ? 4 : 8;
      k++;
    }
  }
  rmtWrite(P_RING, ringSym, k, 10);
}

// ================= mic (PDM) =================
QueueHandle_t rxQ;
const float GAIN_MUL[4] = {1.0f, 2.0f, 4.0f, 8.0f};      // club / loud room / quiet room / max
const float GAIN_GATE[4] = {0.0015f, 0.0025f, 0.004f, 0.008f};
volatile int gainStep = 1;
bool micOk = false;
i2s_chan_handle_t micCh = nullptr;
bool micStart() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = 6; cc.dma_frame_num = HOP;
  if (i2s_new_channel(&cc, nullptr, &micCh) != ESP_OK) return false;
  i2s_pdm_rx_config_t pc = {
    .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(AUD_RATE),
    .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = { .clk = (gpio_num_t)P_MIC_CLK, .din = (gpio_num_t)P_MIC_DATA, .invert_flags = { .clk_inv = false } },
  };
  if (i2s_channel_init_pdm_rx_mode(micCh, &pc) != ESP_OK) return false;
  return i2s_channel_enable(micCh) == ESP_OK;
}
void audioTask(void*) {
  static int16_t buf[HOP];
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(micCh, buf, sizeof(buf), &got, pdMS_TO_TICKS(100)) != ESP_OK || !got) { vTaskDelay(1); continue; }
    int n = got / 2; float g = GAIN_MUL[gainStep & 3];
    for (int i = 0; i < n; i++) { int32_t v = (int32_t)(buf[i] * g); buf[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
    MIC_GATE = GAIN_GATE[gainStep & 3];
    processHop(buf, n, millis() - (uint32_t)(n * 1000 / AUD_RATE));
  }
}

// ================= ESP-NOW =================
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool radioUp = false;
bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) {
    if (!linkValid(d, len)) return;
    LinkFrame f; memcpy(&f, d, sizeof(f)); xQueueSend(rxQ, &f, 0);
  });
  return true;
}
void sendFrame(const LinkFrame& f) { if (radioUp) esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f)); }

// ================= shared state (control task <-> UI) =================
struct Core {
  uint8_t mode = M_FOLLOW;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0, beatId = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, bars = 0;
  int rssi = 0;
  uint32_t bridgeMs = 0, conflictUntil = 0; bool bridgeSeen = false;
  uint32_t flashMs = 0; bool flashDown = false, viaBooth = false;
  uint32_t tapTimes[5]; int tapCount = 0;
  float frac = 0;
  bool onAir = false;                  // CONDUCT
  uint8_t toast = T_NONE; int toastVal = 0; uint32_t toastUntil = 0;
};
Core core;
SemaphoreHandle_t lock;
enum Cmd : uint8_t { C_PRESS, C_TAP, C_TEMPO, C_MODE };
struct CmdEv { uint8_t cmd; float val; };
QueueHandle_t cmdQ;

static inline float periodMs(float b) { return 60000.0f / (b > 30 ? b : 120); }
static uint32_t stepMs() { float a = core.frac + periodMs(core.bpm); uint32_t s = (uint32_t)a; core.frac = a - s; return s; }
void beatAt(uint32_t t, bool down, bool booth) {
  core.flashMs = t; core.flashDown = down; core.lastBeatMs = t; core.viaBooth = booth; core.beatId++;
  if (down) core.bars++;
}
void setToast(uint8_t t, uint32_t now, int val = 0) { core.toast = t; core.toastVal = val; core.toastUntil = now + 1300; }

void takeOver(uint32_t now) {
  if (core.mode == M_TAKE) return;
  core.mode = M_TAKE; core.fires = core.acks = 0; core.tapCount = 0;
  if (core.bpm < 60 || core.bpm > 200) core.bpm = 120;
  float p = periodMs(core.bpm);
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {          // seamless: continue the current grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
  setToast(T_TAKE, now);
}
void release(uint32_t now) {
  if (core.mode != M_TAKE) return;
  core.mode = M_FOLLOW; core.gotAny = true;
  core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;     // HOLD until the conductor is back
  core.conflictUntil = 0; core.tapCount = 0;
  setToast(T_RELEASE, now);
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
  core.nextBeat = now;
}

// true = this ACK confirms our beat: mirror it for consoles
bool handleFrame(const LinkFrame& f, uint32_t now) {
  bool sending = core.mode == M_TAKE || (core.mode == M_CONDUCT && core.onAir);
  if (f.type == LF_FIRE) { if (sending) core.conflictUntil = now + 1500; return false; }
  core.bridgeMs = now; core.bridgeSeen = true;
  if (f.type == LF_HELLO) return false;
  bool echo = f.flags & LINK_FLAG_ECHO;
  if (core.mode != M_FOLLOW) {
    if (!sending) return false;
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

// ================= control task: link, commands, beat timing =================
void controlTask(void*) {
  for (;;) {
    uint32_t now = millis();
    LinkFrame in[8]; int nIn = 0;
    while (nIn < 8 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;
    CmdEv ce[4]; int nCmd = 0;
    while (nCmd < 4 && xQueueReceive(cmdQ, &ce[nCmd], 0) == pdTRUE) nCmd++;
    LinkFrame out[3]; int nOut = 0; bool saveMode = false;

    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < nIn; i++)
      if (handleFrame(in[i], now) && nOut < 3) {
        LinkFrame e = in[i]; e.type = LF_RELAY; e.flags = LINK_FLAG_ECHO; e.rssi = 0; e.snr4 = 0; e.count = 0;
        out[nOut++] = e;
      }
    for (int i = 0; i < nCmd; i++) {
      switch (ce[i].cmd) {
        case C_PRESS:
          if (core.mode == M_CONDUCT) { core.onAir = !core.onAir; core.fires = core.acks = 0; core.conflictUntil = 0; setToast(core.onAir ? T_ONAIR : T_LOCAL, now); }
          else if (core.mode == M_TAKE) release(now); else takeOver(now);
          break;
        case C_TAP:
          if (core.mode == M_CONDUCT) tap(now); else tapTempo(now);
          break;
        case C_TEMPO:
          if (core.mode == M_TAKE) core.bpm = constrain(roundf((core.bpm + ce[i].val) * 10) / 10, 60.0f, 200.0f);
          break;
        case C_MODE:
          if (core.mode == M_CONDUCT) { core.mode = M_FOLLOW; core.onAir = false; setToast(T_FOLLOW, now); }
          else { if (core.mode == M_TAKE) release(now); core.mode = M_CONDUCT; core.onAir = false; setToast(T_CONDUCT, now); }
          saveMode = true;
          break;
      }
    }
    if (core.mode == M_TAKE) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        if (now - core.nextBeat > periodMs(core.bpm)) core.nextBeat = now;
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;
        core.nextBeat += stepMs();
        core.fires++;
        LinkFrame fr = linkFrame(LF_FIRE);
        fr.bpm10 = (uint16_t)(core.bpm * 10 + 0.5f); fr.beat = core.beatPos; fr.hue = core.hue;
        if (nOut < 3) out[nOut++] = fr;
        beatAt(now + LINK_AIR_MS, core.beatPos == 0, true);
      }
    } else if (core.mode == M_CONDUCT) {                       // the mic engine's beat clock
      { int32_t adj = phaseAdj; if (adj) { phaseAdj = 0; nextBeat += adj; } }
      if (micOk && (int32_t)(now - nextBeat) >= 0) {
        beatNum++;
        int bi = ((beatNum % 4) + 4) % 4;
        if (bi == 0) core.hue += 40;
        float period = 60000.0f / bpm;
        nextBeat += (uint32_t)period;
        if ((int32_t)(now - nextBeat) > 0) nextBeat = now + (uint32_t)period;
        core.bpm = bpm; core.beatPos = bi;
        if (core.onAir) {
          LinkFrame fr = linkFrame(LF_FIRE);
          fr.bpm10 = (uint16_t)(bpm * 10 + 0.5f); fr.beat = bi; fr.hue = core.hue;
          if (nOut < 3) out[nOut++] = fr;
          core.fires++;
        }
        beatAt(now + (core.onAir ? LINK_AIR_MS : 0), bi == 0, false);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS &&
               (int32_t)(now - core.flyNext) >= 0) {
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0, core.viaBooth);
      core.flyNext += stepMs();
    }
    uint8_t m = core.mode;
    xSemaphoreGive(lock);

    for (int i = 0; i < nOut; i++) sendFrame(out[i]);
    if (saveMode) { Preferences p; p.begin("beatembed", false); p.putUChar("mode", m == M_CONDUCT ? 1 : 0); p.end(); }
    vTaskDelay(1);
  }
}

// ================= inputs =================
volatile int32_t encCount = 0;
volatile uint8_t encState = 0;
void IRAM_ATTR encIsr() {                       // quadrature state table
  static const int8_t T[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  uint8_t s = (digitalRead(P_ENC_A) << 1) | digitalRead(P_ENC_B);
  encCount += T[(encState << 2) | s]; encState = s;
}
void queueCmd(uint8_t c, float v = 0) { CmdEv e = {c, v}; xQueueSend(cmdQ, &e, 0); }

Preferences prefs;
uint8_t page = 0;                               // 0 = BEAT, 1.. = visuals
bool autoMode = false;
uint32_t labelUntil = 0;
const char* holdHint = nullptr;

struct Btn {
  int pin; bool down = false, armed = false; uint32_t t0 = 0, upSince = 0;
  // hold time when released (0 = nothing yet); down stays true while held
  uint32_t poll(uint32_t now) {
    bool d = digitalRead(pin) == LOW;
    if (!d) { if (!upSince) upSince = now | 1; if (now - upSince > 400) armed = true; } else upSince = 0;
    if (!armed) return 0;
    if (d && !down) { down = true; t0 = now; return 0; }
    if (!d && down) { down = false; uint32_t h = now - t0; return h > 30 ? h : 0; }
    return 0;
  }
};
Btn bKnob = {P_ENC_KEY}, bKey = {P_KEY};
void toastNow(uint8_t t, int val = 0) { uint32_t now = millis(); xSemaphoreTake(lock, portMAX_DELAY); setToast(t, now, val); xSemaphoreGive(lock); }

void inputs(uint32_t now) {
  // knob turn: 2 transitions a click
  static int32_t encUsed = 0; static uint32_t lastTurn = 0;
  int32_t clicks = (encCount - encUsed) / 2;
  if (clicks) {
    encUsed += clicks * 2;
    bool fast = now - lastTurn < 60; lastTurn = now;
    xSemaphoreTake(lock, portMAX_DELAY); uint8_t m = core.mode; xSemaphoreGive(lock);
    if (m == M_TAKE) queueCmd(C_TEMPO, clicks * (fast ? 1.0f : 0.1f));
    else { page = (page + (clicks > 0 ? 1 : V_COUNT)) % (V_COUNT + 1); labelUntil = now + 1500; prefs.putUChar("page", page); }
  }
  // knob press / hold
  uint32_t h = bKnob.poll(now);
  if (bKnob.down) { uint32_t t = now - bKnob.t0; holdHint = t >= 4000 ? "LET GO: DISPLAY" : t >= 1500 ? (autoMode ? "LET GO: AUTO OFF" : "LET GO: AUTO ON") : nullptr; }
  if (h) {
    holdHint = nullptr;
    if (h >= 4000) { panelMode = (panelMode + 1) & 7; prefs.putUChar("panel", panelMode); panelChanged = true; toastNow(T_PANEL, panelMode); }
    else if (h >= 1500) { autoMode = !autoMode; if (autoMode && page == 0) page = 1; prefs.putBool("auto", autoMode); toastNow(autoMode ? T_AUTO_ON : T_AUTO_OFF); }
    else queueCmd(C_PRESS);
  }
  // side key: tap / hold 1.5 s mode / hold 4 s gain
  uint32_t k = bKey.poll(now);
  if (bKey.down) { uint32_t t = now - bKey.t0; if (t >= 4000) holdHint = "LET GO: MIC GAIN"; else if (t >= 1500) holdHint = "LET GO: MODE"; }
  if (k) {
    holdHint = nullptr;
    if (k >= 4000) { gainStep = (gainStep + 1) & 3; prefs.putUChar("gain", gainStep); toastNow(T_GAIN, gainStep); }
    else if (k >= 1500) queueCmd(C_MODE);
    else queueCmd(C_TAP);
  }
}

// ================= battery (BQ27220 fuel gauge) =================
int battery() {
  static int pct = -1; static uint32_t at = 0;
  if (at && millis() - at < 5000) return pct;
  at = millis() | 1;
  Wire.beginTransmission(0x55); Wire.write(0x2C);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(0x55, 2) != 2) return pct = -1;
  int v = Wire.read() | (Wire.read() << 8);
  return pct = v <= 100 ? v : -1;
}

// ================= ring + screen =================
uint8_t ring[RING_N * 3];
void drawRing(const View& v, uint32_t now) {
  bool active = v.mode == M_CONDUCT ? v.micOk : (v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE);
  memset(ring, 0, sizeof(ring));
  if (!active) {                                            // waiting: a slow amber glow chasing round
    int i = (now / 250) % RING_N; ring[i * 3] = 30; ring[i * 3 + 1] = 20;
  } else {
    uint32_t dt = now - v.flashMs; float k = dt < 400 ? 1 - dt / 400.0f : 0; k *= k;
    int r, g, b; vhue(v.hue, r, g, b);
    for (int i = 0; i < RING_N; i++) {
      bool mine = i / 2 == v.beatPos;                         // two LEDs per beat of the bar
      float lvl = v.flashDown ? 0.15f + 0.85f * k : mine ? 0.1f + 0.9f * k : 0.04f;
      ring[i * 3] = r * lvl * 0.45f; ring[i * 3 + 1] = g * lvl * 0.45f; ring[i * 3 + 2] = b * lvl * 0.45f;
    }
  }
  ringShow(ring);
}

void snapshot(View& v, uint32_t now) {
  v = {};
  xSemaphoreTake(lock, portMAX_DELAY);
  v.mode = core.mode; v.onAir = core.onAir; v.micOk = micOk;
  v.bridgeOk = core.bridgeSeen && now - core.bridgeMs < 3000;
  v.conflict = (core.mode == M_TAKE || core.onAir) && (int32_t)(core.conflictUntil - now) > 0;
  if (!radioUp) v.link = LS_NORADIO;
  else if (core.mode == M_TAKE) v.link = LS_TAKE;
  else if (!core.gotAny) v.link = LS_WAIT;
  else if (now - core.lastRxMs < 1500) v.link = LS_LIVE;
  else if (now - core.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
  else v.link = LS_WAIT;
  v.bpm = core.bpm; v.beatPos = core.beatPos; v.hue = core.hue; v.bars = core.bars;
  v.rssi = core.rssi; v.rx = core.rx; v.lost = core.lost; v.sent = core.acks;
  v.flashMs = core.flashMs; v.flashDown = core.flashDown; v.viaBooth = core.viaBooth;
  v.toast = (int32_t)(core.toastUntil - now) > 0 ? core.toast : T_NONE; v.toastVal = core.toastVal;
  xSemaphoreGive(lock);
  v.level = level; v.conf = conf; v.gain = gainStep;
  v.scope = onsetHistory; v.scopeOn = ohOnset; v.scopePos = ohPos; v.scopeN = SCOPE_N;
  v.battery = battery(); v.hint = holdHint;
}

VizState vs = {};
void updateViz(const View& v, uint32_t now) {
  static uint32_t lastFlash = 0;
  bool active = v.mode == M_CONDUCT ? v.micOk : (v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE);
  if (active) {
    if (v.flashMs != lastFlash && (int32_t)(now - v.flashMs) >= 0) {
      lastFlash = v.flashMs; vs.beatMs = v.flashMs; vs.beatId++; vs.beatPos = v.beatPos;
      if (v.beatPos == 0) { vs.bars++; if (autoMode && page > 0 && vs.bars % 8 == 0) { page = page % V_COUNT + 1; labelUntil = now + 1500; } }
    }
    vs.bpm = v.bpm; vs.hue = v.hue; vs.src = v.mode == M_FOLLOW && !v.viaBooth ? SRC_W10 : SRC_BOOTH;
  } else {
    static uint32_t demoNext = 0;
    if (vs.src != SRC_DEMO) { vs.src = SRC_DEMO; vs.bpm = 120; demoNext = now; }
    if ((int32_t)(now - demoNext) >= 0) { vs.beatPos = (vs.beatPos + 1) & 3; vs.beatMs = now; vs.beatId++; demoNext = now + 500; if (!vs.beatPos) vs.hue += 24; }
  }
  vs.mode = page ? page - 1 : 0; vs.autoMode = autoMode; vs.labelUntil = labelUntil; vs.hud = true; vs.hint = nullptr;
}

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
  pinMode(P_PWR_EN, OUTPUT); digitalWrite(P_PWR_EN, HIGH);          // power the screen, ring and radios
  for (int p : {P_CC1101_CS, P_NRF_CS, P_SD_CS, P_TFT_CS}) { pinMode(p, OUTPUT); digitalWrite(p, HIGH); }
  Serial.begin(115200);
  pinMode(P_KEY, INPUT_PULLUP); pinMode(P_ENC_KEY, INPUT_PULLUP);
  pinMode(P_ENC_A, INPUT_PULLUP); pinMode(P_ENC_B, INPUT_PULLUP);
  encState = (digitalRead(P_ENC_A) << 1) | digitalRead(P_ENC_B);
  attachInterrupt(P_ENC_A, encIsr, CHANGE); attachInterrupt(P_ENC_B, encIsr, CHANGE);
  ringOk = rmtInit(P_RING, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
  Wire.begin(P_SDA, P_SCL, 400000);
  delay(50);

  prefs.begin("beatembed", false);
  panelMode = prefs.getUChar("panel", 0) & 7;
  page = prefs.getUChar("page", 0) % (V_COUNT + 1);
  autoMode = prefs.getBool("auto", false);
  gainStep = prefs.getUChar("gain", 1) & 3;
  core.mode = prefs.getUChar("mode", 0) == 1 ? M_CONDUCT : M_FOLLOW;

  applyPanel();
  embedInitFonts();
  cv.setColorDepth(16);
  if (!cv.createSprite(SW, SH)) { cv.setColorDepth(8); cv.createSprite(SW, SH); }
  cv.fillScreen(E_BG);
  etext(cv, &vfLbl, "BEAT COMMANDER", SW / 2, SH / 2 - 10, E_AMB, middle_center);
  etext(cv, &efMono, "Beat Embed v" FW_VERSION, SW / 2, SH / 2 + 14, E_AMB_D, middle_center);
  cv.pushSprite(0, 0);
  uint8_t hello[RING_N * 3];
  for (int s = 0; s < RING_N; s++) {                                 // amber hello round the ring
    memset(hello, 0, sizeof(hello)); hello[s * 3] = 120; hello[s * 3 + 1] = 80; ringShow(hello); delay(60);
  }

  lock = xSemaphoreCreateMutex();
  rxQ = xQueueCreate(24, sizeof(LinkFrame));
  cmdQ = xQueueCreate(16, sizeof(CmdEv));
  micOk = micStart();                                                // before Wi-Fi: its DMA wants internal RAM
  if (micOk) xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  tft.setBrightness(40);                                             // weak USB ports: low backlight while Wi-Fi starts
  radioUp = espnowInit();
  delay(150);
  tft.setBrightness(200);
  Serial.printf("# Beat Embed v%s | ESP-NOW %s | mic %s | ring %s | MAC %s\n", FW_VERSION, radioUp ? "up" : "FAILED",
                micOk ? "OK" : "FAILED", ringOk ? "OK" : "FAILED", WiFi.macAddress().c_str());
  labelUntil = millis() + 1500;
  xTaskCreatePinnedToCore(controlTask, "control", 6144, nullptr, 5, nullptr, 1);
}

void loop() {
  static uint32_t lastDraw = 0, lastLog = 0;
  uint32_t now = millis();
  inputs(now);
  if (panelChanged) { panelChanged = false; applyPanel(); }
  if (now - lastDraw >= 20) {
    lastDraw = now;
    View v; snapshot(v, now);
    drawRing(v, now);
    updateViz(v, now);
    if (page == 0) drawBeatPage(cv, v, now); else vizDraw(cv, vs, now);
    drawToast(cv, v);
    cv.pushSprite(0, 0);
  }
  if (now - lastLog > 1000) {
    lastLog = now;
    if (core.mode == M_CONDUCT) Serial.printf("conduct bpm %.1f est %.1f conf %.2f onsets %d level %.4f gain %d %s\n",
                                              bpm, bpmEst, conf, onsetCount, level, gainStep, core.onAir ? "ON AIR" : "local");
    onsetCount = 0;
  }
  delay(1);
}
