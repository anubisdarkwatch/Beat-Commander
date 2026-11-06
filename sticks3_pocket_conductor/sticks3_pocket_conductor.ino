// M5StickS3 Pocket Conductor: the W10's mic beat engine in your pocket, on the M5Stack StickS3.
// Board: M5StickS3 (ESP32-S3-PICO-1-N8R8, 1.14" 135x240 ST7789, ES8311 mic, two buttons).
//
// CONDUCT: the onboard mic finds the beat (same engine as the W10: onset strength, autocorrelation tempo,
//          phase lock). ON AIR sends every beat over ESP-NOW to the Heltec V3 bridge, which puts it on the
//          LoRa beat link, so every follower, the CYD console and the M5Dial follow the StickS3.
// FOLLOW:  a pocket badge. The screen flashes in the conductor's colour on every beat.
//          If the mic can't start, it boots straight into FOLLOW (the reason shows in CONDUCT and on serial).
//
// Buttons (KEY1 = big front button, KEY2 = side button)
//   KEY1 tap        = tap tempo (taps override the mic for 15 s, first tap = beat 1)      [CONDUCT]
//   KEY1 hold 1 s   = ON AIR on / off (beats go out through the bridge)                   [CONDUCT]
//   KEY2 tap        = mic gain: club / loud room / quiet room / max (remembered)          [CONDUCT]
//   KEY2 hold 1 s   = switch CONDUCT <-> FOLLOW (remembered)
// Switch the W10's LORA TX off before going ON AIR: two conductors on one link collide (CLASH warns you).
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x, board "ESP32S3 Dev Module": Flash 8MB, PSRAM "Disabled" (with PSRAM on,
// the I2S driver's DMA bookkeeping can land in PSRAM and the mic refuses to start: "user context not in internal RAM"),
// USB CDC On Boot "Enabled". Library: M5Unified (pulls in M5GFX).

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <driver/i2s_std.h>
#include "link.h"

#define FW_VERSION    "v1.0.5"

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
bool micOk = false, onAir = false, micFallback = false;

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

// ================= mic =================
// Path 1: M5Unified's Mic class (records 8 ms hops into a small ring).
// Path 2 (fallback): drive the ES8311 and I2S directly, the way the W10 does.
// Whichever path works is shown on the bottom line (M5 / I2S); a failure shows its step code.
#define NBUF 6
int16_t micBuf[NBUF][HOP];
volatile int micNext = 0;
volatile uint32_t hopsIn = 0;
struct Hop { int16_t* p; uint32_t t; };
enum MicPath : uint8_t { MIC_NONE, MIC_M5, MIC_I2S };
uint8_t micPath = MIC_NONE;
char micDiag[200] = "";

// ---- capture the ESP-IDF driver's own error text (e.g. "i2s_std: sample rate is too large") ----
char lastDrvErr[160] = "";
int logHook(const char* fmt, va_list ap) {
  char b[200]; va_list ap2; va_copy(ap2, ap); vsnprintf(b, sizeof(b), fmt, ap2); va_end(ap2);
  const char* p = b;
  while (*p == 0x1B) { while (*p && *p != 'm') p++; if (*p) p++; }   // skip colour codes
  if ((p[0] == 'E' || p[0] == 'W') && p[1] == ' ' && p[2] == '(' &&
      (strstr(p, "i2s") || strstr(p, "I2S") || strstr(p, "gdma") || strstr(p, "intr") || strstr(p, "mic"))) {
    const char* m = strchr(p, ')'); m = m ? m + 2 : p;               // drop "E (1234) "
    char line[120]; snprintf(line, sizeof(line), "%s", m);
    char* esc = strchr(line, 0x1B); if (esc) *esc = 0;
    char* e = line + strlen(line); while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
    // "i2s_std: i2s_channel_init_std_mode(236): msg" -> "i2s_std: msg"
    char* fn = strstr(line, ": "); char* cl = fn ? strstr(fn + 2, "): ") : nullptr;
    if (fn && cl && strchr(fn + 2, '(') && strchr(fn + 2, '(') < cl) memmove(fn + 2, cl + 3, strlen(cl + 3) + 1);
    size_t n = strlen(lastDrvErr);
    if (n + strlen(line) + 3 < sizeof(lastDrvErr)) snprintf(lastDrvErr + n, sizeof(lastDrvErr) - n, "%s%s", n ? "; " : "", line);
  }
  return vprintf(fmt, ap);
}
i2s_chan_handle_t rxCh = nullptr;

void onMicBuffer(void*, void* data, size_t len) {     // capture task: a hop is ready
  Hop h = {(int16_t*)data, millis() - (uint32_t)(len * 1000 / AUD_RATE)};
  xQueueSend(micQ, &h, 0);
  int i = micNext; micNext = (i + 1) % NBUF;
  M5.Mic.record(micBuf[i], HOP, AUD_RATE);              // keep the queue full
}

void audioTask(void*) {
  static int16_t raw[HOP * 2], mono[HOP];
  Hop h;
  for (;;) {
    if (micPath == MIC_I2S) {                          // blocking reads keep this in step with real time
      size_t got = 0;
      if (i2s_channel_read(rxCh, raw, HOP * 4, &got, pdMS_TO_TICKS(100)) != ESP_OK) got = 0;
      int n = got / 4;
      if (n <= 0) { vTaskDelay(1); continue; }
      for (int i = 0; i < n; i++) {                    // mic lands on one slot: sum both (other is ~0)
        int32_t v = (int32_t)raw[i * 2] + raw[i * 2 + 1];
        mono[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
      }
      hopsIn++;
      processHop(mono, n, millis() - (uint32_t)(n * 1000 / AUD_RATE));
    } else if (xQueueReceive(micQ, &h, pdMS_TO_TICKS(50)) == pdTRUE) {
      hopsIn++;
      processHop(h.p, HOP, h.t);
    }
  }
}

bool esW(uint8_t r, uint8_t v) { return M5.In_I2C.writeRegister8(0x18, r, v, 400000); }

void setGain(int g) {
  gain = g & 3; MIC_GATE = GAIN_GATE[gain];
  esW(0x16, GAIN_REG[gain]);
  prefs.putUChar("gain", gain);
}

bool waitHops(uint32_t ms) {
  uint32_t h0 = hopsIn, t0 = millis();
  while (millis() - t0 < ms) { if (hopsIn - h0 >= 10) return true; delay(10); }
  return false;
}

bool micStartM5() {
  if (M5.getBoard() != m5::board_t::board_M5StickS3) { snprintf(micDiag, sizeof(micDiag), "E1 board %d", (int)M5.getBoard()); return false; }
  auto mc = M5.Mic.config();
  if (mc.pin_data_in < 0) { strcpy(micDiag, "E2 no mic pin"); return false; }
  mc.sample_rate = AUD_RATE; mc.noise_filter_level = 0; mc.magnification = 1;
  M5.Mic.config(mc);
  M5.Mic.setBufferReleaseCallback(nullptr, onMicBuffer);
  if (!M5.Mic.begin()) { strcpy(micDiag, "E3 mic begin"); Serial.printf("# M5 mic begin failed, driver said: %s\n", lastDrvErr); return false; }
  setGain(gain);
  micNext = 2;
  if (!M5.Mic.record(micBuf[0], HOP, AUD_RATE) || !M5.Mic.record(micBuf[1], HOP, AUD_RATE)) { strcpy(micDiag, "E4 record"); return false; }
  micPath = MIC_M5;
  if (!waitHops(600)) { strcpy(micDiag, "E5 no audio"); micPath = MIC_NONE; return false; }
  return true;
}

// Direct path: ES8311 over I2C + ESP-IDF I2S std driver, so a failure reports the exact esp_err name.
// Attempt A: ESP32 drives MCLK (W10 set-up). Attempt B: no MCLK, the codec clocks from BCLK.
void codecInit(bool mclkFromPin) {
  esW(0x00, 0x1F); delay(20);
  esW(0x00, 0x00); esW(0x00, 0x80);
  if (mclkFromPin) { esW(0x01, 0x3F); esW(0x02, 0x00); }
  else { esW(0x01, 0xBF); esW(0x02, 0x18); }          // MCLK = BCLK, pre-multiply x8 (32 x 16 kHz x 8 = 4.096 MHz)
  esW(0x03, 0x10); esW(0x04, 0x10); esW(0x05, 0x00);
  esW(0x06, 0x03); esW(0x07, 0x00); esW(0x08, 0xFF);
  esW(0x09, 0x0C); esW(0x0A, 0x0C);                    // 16-bit I2S in/out
  esW(0x0D, 0x01); esW(0x0E, 0x02);                    // analog power, PGA + ADC modulator on
  esW(0x14, 0x1A);                                     // analog mic, PGA up
  esW(0x17, 0xC8);                                     // ADC volume
  esW(0x1C, 0x6A);
  setGain(gain);
}

bool i2sOpen(bool withMclk, char tag) {
  if (rxCh) { i2s_channel_disable(rxCh); i2s_del_channel(rxCh); rxCh = nullptr; }
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  esp_err_t e = i2s_new_channel(&cc, nullptr, &rxCh);
  if (e != ESP_OK) { snprintf(micDiag, sizeof(micDiag), "%c new %s", tag, esp_err_to_name(e)); rxCh = nullptr; return false; }
  i2s_std_config_t sc = {};
  sc.clk_cfg.sample_rate_hz = AUD_RATE; sc.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT; sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = withMclk ? GPIO_NUM_18 : I2S_GPIO_UNUSED;
  sc.gpio_cfg.bclk = GPIO_NUM_17; sc.gpio_cfg.ws = GPIO_NUM_15;
  sc.gpio_cfg.dout = I2S_GPIO_UNUSED; sc.gpio_cfg.din = GPIO_NUM_16;
  e = i2s_channel_init_std_mode(rxCh, &sc);
  if (e != ESP_OK) { snprintf(micDiag, sizeof(micDiag), "%c init %s", tag, esp_err_to_name(e)); i2s_del_channel(rxCh); rxCh = nullptr; return false; }
  e = i2s_channel_enable(rxCh);
  if (e != ESP_OK) { snprintf(micDiag, sizeof(micDiag), "%c enable %s", tag, esp_err_to_name(e)); i2s_del_channel(rxCh); rxCh = nullptr; return false; }
  return true;
}

bool micStartI2S() {
  M5.Mic.end(); delay(30);
  if (!esW(0x00, 0x00)) { strcpy(micDiag, "E6 codec i2c"); return false; }
  for (int attempt = 0; attempt < 2; attempt++) {
    bool mclk = attempt == 0; char tag = mclk ? 'A' : 'B';
    codecInit(mclk);
    lastDrvErr[0] = 0;
    if (!i2sOpen(mclk, tag)) { Serial.printf("# direct I2S %c failed: %s | driver: %s\n", tag, micDiag, lastDrvErr); continue; }
    micPath = MIC_I2S;
    if (waitHops(600)) { snprintf(micDiag, sizeof(micDiag), "I2S%c", tag); return true; }
    micPath = MIC_NONE;
    snprintf(micDiag, sizeof(micDiag), "%c no audio", tag);
    Serial.printf("# direct I2S %c: no audio\n", tag);
  }
  return false;
}

// I2S opened before M5.begin(): claims its DMA channel before the display and the rest of the board start
bool earlyI2S = false;
bool micStart() {
  if (earlyI2S) {
    i2s_channel_disable(rxCh);                          // board start-up may have re-muxed the pins: route them again
    i2s_std_gpio_config_t g = {};
    g.mclk = GPIO_NUM_18; g.bclk = GPIO_NUM_17; g.ws = GPIO_NUM_15; g.dout = I2S_GPIO_UNUSED; g.din = GPIO_NUM_16;
    i2s_channel_reconfig_std_gpio(rxCh, &g);
    i2s_channel_enable(rxCh);
    codecInit(true);
    micPath = MIC_I2S;
    if (waitHops(600)) { strcpy(micDiag, "I2SE"); return true; }
    micPath = MIC_NONE; strcpy(micDiag, "early I2S no audio");
    Serial.println("# early I2S opened but no audio, trying the other paths");
  }
  lastDrvErr[0] = 0;
  if (micStartM5()) { strcpy(micDiag, "M5"); return true; }
  Serial.printf("# M5 mic path failed: %s, trying direct I2S\n", micDiag);
  char first[24]; snprintf(first, sizeof(first), "%s", micDiag);
  if (micStartI2S()) return true;
  char both[200]; snprintf(both, sizeof(both), "%s | %s%s%s", first, micDiag, lastDrvErr[0] ? " > " : "", lastDrvErr);
  strcpy(micDiag, both);
  return false;
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
  esp_log_set_vprintf(logHook);                         // keep the driver's error text for the screen
  esp_log_level_set("*", ESP_LOG_WARN);
  earlyI2S = i2sOpen(true, 'E');                        // before anything else grabs DMA / interrupts
  char earlyErr[160]; snprintf(earlyErr, sizeof(earlyErr), "%s", earlyI2S ? "" : lastDrvErr);
  auto cfg = M5.config();
  cfg.internal_spk = false;                             // the speaker shares the mic's I2S pins
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("# M5StickS3 Pocket Conductor " FW_VERSION);
  M5.Display.setRotation(0);                            // portrait, buttons below the screen
  M5.Display.setBrightness(BRIGHTNESS);
  initColors();
  cv.setColorDepth(16); cv.setPsram(false);              // 65 KB frame buffer in internal RAM
  if (!cv.createSprite(SW, SH)) Serial.println("# sprite alloc failed");
  splash(); cv.pushSprite(0, 0);

  prefs.begin("pocket", false);
  mode = prefs.getUChar("mode", MODE_CONDUCT) ? MODE_FOLLOW : MODE_CONDUCT;
  gain = prefs.getUChar("gain", 1) & 3;

  rxQ = xQueueCreate(16, sizeof(LinkFrame));
  micQ = xQueueCreate(NBUF, sizeof(Hop));
  if (!linkInit()) Serial.println("# ESP-NOW init failed");
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  if (!earlyI2S) Serial.printf("# early I2S failed: %s | %s\n", micDiag, earlyErr);
  micOk = micStart();
  if (!micOk && !earlyI2S) {                            // add the early attempt's error too
    char t[200]; snprintf(t, sizeof(t), "%s || early: %s", micDiag, earlyErr[0] ? earlyErr : "?"); strcpy(micDiag, t);
  }
  if (!micOk && mode == MODE_CONDUCT) {                 // no mic: don't sit on a fake 120 BPM, follow the rig instead
    mode = MODE_FOLLOW; micFallback = true;             // not saved: next boot tries the mic again
  }
  Serial.printf("# display %dx%d\n", M5.Display.width(), M5.Display.height());
  Serial.printf("# mic %s (%s) | board %d | gain %d | mode %s | ESP-NOW ch %d net 0x%02X | MAC %s\n", micOk ? "ok" : "FAILED", micDiag, (int)M5.getBoard(), gain,
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
      v.sent = acks; v.micDiag = micDiag; v.level = level; v.conf = conf; v.gain = gain; v.micOk = micOk;
      v.scope = onsetHistory; v.scopeOn = ohOnset; v.scopePos = ohPos; v.scopeN = SCOPE_N;
      v.bpm = bpm; v.beatPos = beatPosShown; v.hue = hue;
    } else {
      if (!fol.gotAny) v.link = LS_WAIT;
      else if (now - fol.lastRxMs < 1500) v.link = LS_LIVE;
      else if (now - fol.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
      else v.link = LS_WAIT;
      v.viaBooth = fol.viaBooth; v.rssi = fol.rssi; v.lost = fol.lost; v.micFallback = micFallback; v.micDiag = micDiag;
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
    else Serial.printf("# follow | mic %s\n", micDiag);
    if (!micOk) Serial.printf("# MIC FAILED: %s (board %d)\n", micDiag, (int)M5.getBoard());
    onsetCount = 0;
  }
  delay(1);
}
