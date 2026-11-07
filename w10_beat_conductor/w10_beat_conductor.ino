// Beat Commander: W10 conductor - mic beat detection -> screen + RGB LED light show + LoRa beat broadcast
// Meshnology W10 (ESP32-S3, ES8311 mic, ST7789 240x240, WS2812 on GPIO48, E22/SX1262)
// BOOT tap = tap tempo (taps override the mic for 15 s). BOOT hold 1 s = LoRa beat broadcast on/off.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include "ESP_I2S.h"
#include <initializer_list>

// ---------------- Pins (Meshnology W10 pin table) ----------------
#define I2C_SDA 8
#define I2C_SCL 7
#define MCP_ADDR 0x20       // GPA1 LCD reset, GPA3 LoRa NRST, GPA7 speaker amp, GPB2 LoRa BUSY
#define ES_ADDR 0x18
#define SPI_SCK 12
#define SPI_MISO 11
#define SPI_MOSI 13
#define LORA_CS 14
#define LCD_CS 10
#define LCD_DC 16
#define LCD_BL 6
#define LED_PIN 48          // WS2812
#define BTN 0
#define W 240

// ---------------- Beat link (portable to other LoRa boards) ----------------
#define LINK_FREQ_HZ 916000000UL   // away from Meshtastic default and US LoRaWAN channels
#define LINK_POWER_DBM 10
struct __attribute__((packed)) BeatPacket { uint8_t m0, m1; uint8_t seq; uint16_t bpm10; uint8_t beat; uint8_t hue; };

SPISettings lcdSPI(40000000, MSBFIRST, SPI_MODE0);
SPISettings loraSPI(8000000, MSBFIRST, SPI_MODE0);
I2SClass i2s;

// ================= MCP23017 =================
void mcpW(uint8_t r, uint8_t v) { Wire.beginTransmission(MCP_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
int mcpR(uint8_t r) {
  Wire.beginTransmission(MCP_ADDR); Wire.write(r);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((uint8_t)MCP_ADDR, (uint8_t)1) != 1) return -1;
  return Wire.read();
}
void mcpPin(uint8_t bit, bool high) {   // GPA bit as output
  int d = mcpR(0x00), o = mcpR(0x14); if (d < 0 || o < 0) return;
  mcpW(0x00, d & ~(1 << bit)); mcpW(0x14, high ? (o | (1 << bit)) : (o & ~(1 << bit)));
}
bool loraBusy() { int v = mcpR(0x13); return v < 0 ? true : (v & 0x04); }
void waitBusy(uint32_t ms = 300) { uint32_t t = millis(); while (loraBusy() && millis() - t < ms) delayMicroseconds(50); }

// ================= SX1262 (TX only) =================
bool loraOk = false, txOn = false;
uint8_t txSeq = 0;
void loraXfer(const uint8_t* d, int n, uint8_t* rx = nullptr) {
  SPI.beginTransaction(loraSPI); digitalWrite(LORA_CS, LOW);
  for (int i = 0; i < n; i++) { uint8_t r = SPI.transfer(d[i]); if (rx) rx[i] = r; }
  digitalWrite(LORA_CS, HIGH); SPI.endTransaction();
}
void cmd(std::initializer_list<uint8_t> l) { uint8_t b[16]; int n = 0; for (auto x : l) b[n++] = x; loraXfer(b, n); }
uint8_t readReg(uint16_t a) { uint8_t t[5] = {0x1D, (uint8_t)(a >> 8), (uint8_t)a, 0, 0}, r[5]; loraXfer(t, 5, r); return r[4]; }
bool loraInit() {
  mcpPin(3, false); delay(10); mcpPin(3, true); delay(50); waitBusy(1000);
  cmd({0x80, 0x00}); waitBusy();                       // standby RC
  if (readReg(0x0740) != 0x14) { Serial.println("SX1262 not responding"); return false; }
  cmd({0x96, 0x01}); waitBusy();                       // DC-DC
  cmd({0x9D, 0x01}); waitBusy();                       // DIO2 = RF switch
  cmd({0x8A, 0x01}); waitBusy();                       // LoRa
  cmd({0x98, 0xE1, 0xE9}); delay(5); waitBusy();       // image cal 902-928
  uint32_t f = (uint32_t)(((uint64_t)LINK_FREQ_HZ << 25) / 32000000ULL);
  cmd({0x86, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f}); waitBusy();
  cmd({0x95, 0x04, 0x07, 0x00, 0x01}); waitBusy();     // PA config (SX1262 high power)
  cmd({0x8E, (uint8_t)LINK_POWER_DBM, 0x04}); waitBusy(); // TX power, 200 us ramp
  cmd({0x8B, 0x07, 0x06, 0x01, 0x00}); waitBusy();     // SF7, BW500, CR4/5 (~10 ms airtime)
  cmd({0x8F, 0x00, 0x00}); waitBusy();                 // buffer base
  cmd({0x80, 0x01}); waitBusy();                       // standby XOSC
  Serial.println("LoRa beat link ready (916.0 MHz, SF7/BW500, 10 dBm)");
  return true;
}
void sendBeat(float bpm, int beat, uint8_t hue) {
  if (!loraOk || !txOn) return;
  BeatPacket p = {'B', 'T', txSeq++, (uint16_t)(bpm * 10), (uint8_t)beat, hue};
  cmd({0x80, 0x01});
  uint8_t buf[2 + sizeof(p)]; buf[0] = 0x0E; buf[1] = 0x00; memcpy(buf + 2, &p, sizeof(p));
  loraXfer(buf, sizeof(buf));                          // write payload
  cmd({0x8C, 0x00, 0x08, 0x00, (uint8_t)sizeof(p), 0x01, 0x00}); // preamble 8, explicit, len, CRC on
  cmd({0x02, 0xFF, 0xFF});                             // clear IRQs
  cmd({0x83, 0x00, 0x00, 0x00});                       // TX (no timeout); ~10 ms, then auto standby
}

// ================= ST7789 =================
void lcdBegin() { SPI.beginTransaction(lcdSPI); digitalWrite(LCD_CS, LOW); }
void lcdEnd() { digitalWrite(LCD_CS, HIGH); SPI.endTransaction(); }
void lcdCmd(uint8_t c) { digitalWrite(LCD_DC, LOW); SPI.transfer(c); digitalWrite(LCD_DC, HIGH); }
void lcdWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  lcdCmd(0x2A); SPI.transfer16(x0); SPI.transfer16(x1);
  lcdCmd(0x2B); SPI.transfer16(y0); SPI.transfer16(y1); lcdCmd(0x2C);
}
uint16_t swapBuf[W * 170];
void pushCanvas(GFXcanvas16& c, int y, int h) {
  uint16_t* s = c.getBuffer();
  for (int i = 0; i < W * h; i++) swapBuf[i] = (s[i] >> 8) | (s[i] << 8);
  lcdBegin(); lcdWindow(0, y, W - 1, y + h - 1); SPI.writeBytes((uint8_t*)swapBuf, (uint32_t)W * h * 2); lcdEnd();
}
void lcdInit() {
  mcpPin(1, true); delay(10); mcpPin(1, false); delay(10); mcpPin(1, true); delay(120);
  lcdBegin(); lcdCmd(0x01); lcdEnd(); delay(150);
  lcdBegin(); lcdCmd(0x11); lcdEnd(); delay(120);
  lcdBegin(); lcdCmd(0x3A); SPI.transfer(0x55); lcdCmd(0x36); SPI.transfer(0x00);
  lcdCmd(0x21); lcdCmd(0x13); lcdCmd(0x29); lcdEnd(); delay(20);
}

// ================= Colors =================
uint16_t rgbRaw(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
uint8_t g8(uint8_t v) { return (uint8_t)(powf(v / 255.0f, 1.8f) * 255.0f + 0.5f); }
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return rgbRaw(g8(r), g8(g), g8(b)); } // panel shows dark tones bright
void hsv(uint8_t h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  float hh = h / 255.0f * 6.0f; int i = (int)hh; float f = hh - i;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f)), R, G, B;
  switch (i % 6) { case 0: R=v;G=t;B=p; break; case 1: R=q;G=v;B=p; break; case 2: R=p;G=v;B=t; break;
                   case 3: R=p;G=q;B=v; break; case 4: R=t;G=p;B=v; break; default: R=v;G=p;B=q; }
  r = R * 255; g = G * 255; b = B * 255;
}
#define C_PANEL rgb(14, 20, 36)
#define C_TEXT  rgb(150, 165, 190)
#define C_CYAN  rgb(0, 230, 210)
#define C_AMBER rgb(255, 190, 40)
#define C_GREEN rgb(57, 255, 20)
#define C_RED   rgb(255, 50, 60)
#define C_GRID  rgb(28, 38, 60)

// ================= Mic (ES8311 ADC) =================
#define AUD_RATE 16000
void esW(uint8_t r, uint8_t v) { Wire.beginTransmission(ES_ADDR); Wire.write(r); Wire.write(v); Wire.endTransmission(); }
bool micInit() {
  mcpPin(7, false);                   // speaker amp off (no feedback)
  esW(0x00, 0x1F); delay(20); esW(0x00, 0x00); esW(0x00, 0x80);
  esW(0x01, 0x3F); esW(0x02, 0x00); esW(0x03, 0x10); esW(0x04, 0x10); esW(0x05, 0x00);
  esW(0x06, 0x03); esW(0x07, 0x00); esW(0x08, 0xFF);
  esW(0x09, 0x0C); esW(0x0A, 0x0C);   // 16-bit I2S in/out
  esW(0x0D, 0x01); esW(0x0E, 0x02);   // analog power, PGA + ADC modulator on
  esW(0x14, 0x1A);                    // analog mic, PGA max
  esW(0x16, 0x05);                    // mic gain 30 dB
  esW(0x17, 0xC8);                    // ADC volume
  esW(0x1C, 0x6A);
  i2s.setPins(2, 4, 5, 3, 1);         // BCK, LRCK, DOUT, DIN(mic), MCLK
  bool ok = i2s.begin(I2S_MODE_STD, AUD_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  Serial.printf("Mic %s\n", ok ? "ok" : "I2S FAILED");
  return ok;
}

// ================= Beat engine (portable, no hardware) =================
// Onset strength (log-energy rise, kick band + full band) every 8 ms hop,
// tempo by autocorrelation over ~6 s, beat clock phase-locked to strong onsets.
#define HOP 128                       // 8 ms at 16 kHz
#define OSS_N 768                     // 6.1 s of onset strength
#define HOP_MS 8.0f
float oss[OSS_N]; int ossPos = 0; uint32_t hops = 0;
float lp = 0, prevLow = -100, prevFull = -100, level = 0;
float ossMean = 0, ossVar = 1e-4f;
uint32_t lastOnsetMs = 0;
float bpm = 120.0f, bpmEst = 0, conf = 0;
float estHist[3] = {0, 0, 0}; int estN = 0;
uint32_t nextBeat = 0; volatile int32_t phaseAdj = 0;
int beatNum = 0;                      // beat in bar = beatNum % 4
uint32_t tapTimes[5]; int tapCount = 0; uint32_t tapLock = 0;
int onsetsSec = 0, onsetCount = 0;
float onsetHistory[W]; int ohPos = 0; bool ohOnset[W];

void estimateTempo(uint32_t tNow) {
  static float x[OSS_N];
  float m = 0; for (int i = 0; i < OSS_N; i++) m += oss[i]; m /= OSS_N;
  for (int i = 0; i < OSS_N; i++) x[i] = oss[(ossPos + i) % OSS_N] - m;   // oldest -> newest
  float r0 = 0; for (int i = 0; i < OSS_N; i++) r0 += x[i] * x[i];
  if (r0 < 1e-6f) { conf = 0; return; }
  const int Lmin = 39, Lmax = 108;    // 190 .. 70 BPM at 8 ms hops
  static float sc[120];
  auto acf = [&](int L) { float r = 0; for (int i = L; i < OSS_N; i++) r += x[i] * x[i - L]; return r; };
  for (int L = Lmin - 1; L <= Lmax + 1; L++) {
    float r = acf(L) + 0.5f * acf(2 * L) + 0.33f * acf(3 * L) + 0.25f * acf(4 * L); // beat-period harmonics
    float b = 60000.0f / (L * HOP_MS);
    float o = log2f(b / 125.0f);                          // mild prior toward dance tempos
    sc[L] = r * expf(-0.5f * (o * o) / (0.45f * 0.45f));
  }
  int best = Lmin; for (int L = Lmin; L <= Lmax; L++) if (sc[L] > sc[best]) best = L;
  float a = sc[best - 1], b = sc[best], c = sc[best + 1];
  float d = (a - 2 * b + c) != 0 ? 0.5f * (a - c) / (a - 2 * b + c) : 0;   // parabolic peak
  if (d > 0.5f) d = 0.5f; if (d < -0.5f) d = -0.5f;
  bpmEst = 60000.0f / ((best + d) * HOP_MS);
  conf = b / (2.08f * r0);
  if (conf < 0.08f) return;
  estHist[estN % 3] = bpmEst; estN++;
  if (estN >= 3) {                                        // median of last 3 estimates
    float e0 = estHist[0], e1 = estHist[1], e2 = estHist[2];
    float med = fmaxf(fminf(e0, e1), fminf(fmaxf(e0, e1), e2));
    if (fabsf(med - bpm) / bpm > 0.04f) bpm = med;        // big change: jump
    else bpm = bpm * 0.8f + med * 0.2f;                   // small: glide
  }

  // phase: fold recent onset strength at the beat period, align beat clock to the strongest slot
  {
    float P = 60000.0f / bpm / HOP_MS;                    // period in hops
    int Pi = (int)(P + 0.5f);
    float bestS = -1; int bestK = 0;
    for (int k = 0; k < Pi; k++) {
      float sum = 0;
      for (int j = 0; ; j++) {
        int back = k + (int)(j * P + 0.5f);
        if (back >= OSS_N / 2) break;                     // last ~3 s
        int idx = (ossPos - 1 - back + OSS_N) % OSS_N;
        { float c = oss[idx] + 0.5f * oss[(idx + OSS_N - 1) % OSS_N] + 0.5f * oss[(idx + 1) % OSS_N]; sum += c * c; }  // kicks dominate
      }
      if (sum > bestS) { bestS = sum; bestK = k; }
    }
    float period = 60000.0f / bpm;
    float lastBeatMs = (float)tNow - bestK * HOP_MS - HOP_MS;   // onset lags the kick by ~1 hop
    float err = fmodf((float)(int32_t)(nextBeat - (uint32_t)lastBeatMs), period);  // where our clock sits vs the kick grid
    if (err > period / 2) err -= period;
    if (err < -period / 2) err += period;
    if (fabsf(err) > period * 0.15f) phaseAdj -= (int32_t)err;      // wrong phase: snap
    else phaseAdj -= (int32_t)(err * 0.5f);                         // close: glide
  }
}

// one 8 ms hop of mono samples taken at audio time tMs (millis domain)
void processHop(const int16_t* x, int n, uint32_t tMs) {
  const float a = 0.057f;             // ~150 Hz low-pass: kick band
  float el = 0, ef = 0;
  for (int i = 0; i < n; i++) { float s = x[i] / 32768.0f; lp += a * (s - lp); el += lp * lp; ef += s * s; }
  el /= n; ef /= n;
  level = level * 0.8f + sqrtf(ef) * 0.2f;
  float ldb = 10 * log10f(el + 1e-10f), fdb = 10 * log10f(ef + 1e-10f);
  float o = fmaxf(0, ldb - prevLow) + 0.5f * fmaxf(0, fdb - prevFull);
  prevLow = ldb; prevFull = fdb;
  if (level < 0.0015f) o = 0;         // silence: no onsets
  oss[ossPos] = o; ossPos = (ossPos + 1) % OSS_N; hops++;
  ossMean = ossMean * 0.99f + o * 0.01f;
  float dv = o - ossMean; ossVar = ossVar * 0.99f + dv * dv * 0.01f;
  float thr = ossMean + 2.0f * sqrtf(ossVar);
  bool on = false;
  if (o > thr && o > 1.5f && tMs - lastOnsetMs > 220) {
    on = true; lastOnsetMs = tMs; onsetCount++;
  }
  if ((hops & 1) == 0) {              // scope at 62 Hz
    float v = o / 12.0f; if (v > 1) v = 1;
    onsetHistory[ohPos] = v; ohOnset[ohPos] = on; ohPos = (ohPos + 1) % W;
  } else if (on) ohOnset[(ohPos + W - 1) % W] = true;
  if (hops % 64 == 0 && tMs > tapLock) estimateTempo(tMs);   // every ~0.5 s
}
void tap(uint32_t now) {
  if (tapCount && now - tapTimes[tapCount - 1] > 2000) tapCount = 0;
  if (tapCount == 5) { memmove(tapTimes, tapTimes + 1, 4 * sizeof(uint32_t)); tapCount = 4; }
  tapTimes[tapCount++] = now;
  if (tapCount >= 2) {
    float avg = (float)(tapTimes[tapCount - 1] - tapTimes[0]) / (tapCount - 1);
    bpm = 60000.0f / avg; tapLock = now + 15000; estN = 0;
  }
  if (tapCount == 1) beatNum = -1;    // first tap = downbeat
  nextBeat = now;                     // fire on the tap
}

// ================= Lights =================
GFXcanvas16 top(W, 30), stage(W, 170), scope(W, 40);
float flash = 0; uint8_t hue = 160;
void ledShow(float v, uint8_t h) {
  uint8_t r, g, b; hsv(h, 1.0f, v * 0.6f, r, g, b);  // 0.6 = don't blind the crowd
  rgbLedWrite(LED_PIN, r, g, b);
}
void drawTop() {
  top.fillScreen(C_PANEL); top.setTextSize(1);
  top.setTextColor(C_CYAN); top.setCursor(4, 4); top.print("W10 // BEAT CONDUCTOR");
  bool manual = millis() < tapLock;
  top.setTextColor(manual ? C_AMBER : C_GREEN); top.setCursor(4, 16); top.print(manual ? "TAP" : "MIC");
  top.setTextColor(C_TEXT); top.setCursor(30, 16); top.print(manual ? "manual tempo" : "auto beat");
  if (txOn) { top.fillRoundRect(178, 13, 58, 13, 3, C_RED); top.setTextColor(rgb(255, 255, 255)); top.setCursor(184, 16); top.print("LORA TX"); }
  else { top.drawRoundRect(178, 13, 58, 13, 3, C_GRID); top.setTextColor(C_GRID); top.setCursor(184, 16); top.print("LORA off"); }
  pushCanvas(top, 0, 30);
}
void drawStage() {
  uint8_t r, g, b; hsv(hue, 0.9f, 0.08f + flash * 0.92f, r, g, b);
  stage.fillScreen(rgbRaw(r, g, b));                 // raw: the flash should hit full brightness
  // big BPM
  char s[8]; snprintf(s, sizeof(s), "%d", (int)(bpm + 0.5f));
  stage.setTextSize(7); stage.setTextColor(rgb(255, 255, 255));
  int tw = strlen(s) * 42; stage.setCursor((W - tw) / 2, 30); stage.print(s);
  stage.setTextSize(2); stage.setTextColor(rgb(255, 255, 255)); stage.setCursor(102, 92); stage.print("BPM");
  // beat dots 1-4
  int bi = ((beatNum % 4) + 4) % 4;
  for (int i = 0; i < 4; i++) {
    int x = 48 + i * 48;
    if (i == bi) stage.fillCircle(x, 140, 12, i == 0 ? C_AMBER : rgb(255, 255, 255));
    else stage.drawCircle(x, 140, 12, rgb(255, 255, 255));
  }
  pushCanvas(stage, 30, 170);
}
void drawScope() {
  scope.fillScreen(rgb(6, 9, 18));
  for (int i = 0; i < W; i++) {
    int k = (ohPos + i) % W;
    int h = (int)(onsetHistory[k] * 34);
    scope.drawFastVLine(i, 38 - h, h, ohOnset[k] ? C_AMBER : C_CYAN);
  }
  int vu = (int)(fminf(level * 6, 1.0f) * W);
  scope.fillRect(0, 0, vu, 3, C_GREEN);
  pushCanvas(scope, 200, 40);
}

// ================= Main =================
bool micOk = false;
// audio runs in its own task on core 0: blocking reads keep it in step with real time
void audioTask(void*) {
  static int16_t raw[HOP * 2], mono[HOP];
  for (;;) {
    size_t got = i2s.readBytes((char*)raw, HOP * 4);
    int n = got / 4;
    if (n <= 0) { vTaskDelay(1); continue; }
    for (int i = 0; i < n; i++) {                      // mic lands on one slot: sum both (other is ~0)
      int32_t v = (int32_t)raw[i * 2] + raw[i * 2 + 1];
      mono[i] = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
    }
    processHop(mono, n, millis() - (uint32_t)(n * 1000 / AUD_RATE));
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis(); while (!Serial && millis() - t0 < 2000) delay(10);
  Serial.println("\n=== W10 Beat Conductor ===");
  pinMode(BTN, INPUT_PULLUP);
  pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH);
  pinMode(LCD_CS, OUTPUT); digitalWrite(LCD_CS, HIGH);
  pinMode(LCD_DC, OUTPUT); digitalWrite(LCD_DC, HIGH);
  pinMode(LCD_BL, OUTPUT); digitalWrite(LCD_BL, LOW);
  rgbLedWrite(LED_PIN, 0, 0, 0);
  pinMode(I2C_SDA, INPUT_PULLUP); pinMode(I2C_SCL, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL); Wire.setClock(10000); Wire.setTimeOut(1000); delay(100);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  lcdInit(); digitalWrite(LCD_BL, HIGH);
  drawTop();
  Wire.setClock(100000);
  loraOk = loraInit();
  micOk = micInit();
  if (micOk) xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
  nextBeat = millis() + 500;
  Serial.println("BOOT tap = tap tempo, BOOT hold 1s = LoRa beat broadcast on/off");
}

uint32_t btnDown = 0; bool btnWas = false, held = false;
uint32_t lastFrame = 0, lastLog = 0;

void loop() {
  uint32_t now = millis();

  // --- button: tap on press, toggle LoRa on 1 s hold ---
  bool b = digitalRead(BTN) == LOW;
  if (b && !btnWas) { btnDown = now; held = false; tap(now); }
  if (b && !held && now - btnDown > 1000) {
    held = true; txOn = !txOn; tapCount = 0; tapLock = 0;   // a hold isn't a tap
    Serial.printf("LoRa beat broadcast %s\n", txOn ? "ON" : "OFF");
  }
  btnWas = b;

  // --- apply phase corrections from the audio task ---
  { int32_t adj = phaseAdj; if (adj) { phaseAdj = 0; nextBeat += adj; } }

  // --- beat clock ---
  if ((int32_t)(now - nextBeat) >= 0) {
    beatNum++;
    int bi = ((beatNum % 4) + 4) % 4;
    if (bi == 0) hue += 40;                            // new color every bar
    flash = 1.0f;
    sendBeat(bpm, bi, hue);
    float period = 60000.0f / bpm;
    nextBeat += (uint32_t)period;
    if ((int32_t)(now - nextBeat) > 0) nextBeat = now + (uint32_t)period; // catch up after stalls
  }

  // --- lights ~30 fps ---
  if (now - lastFrame >= 40) {
    float dt = (now - lastFrame) / 1000.0f; lastFrame = now;
    float period = 60.0f / bpm;
    flash *= expf(-dt * 4.0f / period);               // decay over roughly one beat
    ledShow(flash, hue);
    drawStage(); drawScope();
    static uint32_t lastTop = 0; if (now - lastTop > 500) { drawTop(); lastTop = now; }
  }

  if (now - lastLog > 1000) {
    lastLog = now;
    Serial.printf("bpm %.1f  est %.1f  conf %.2f  onsets/s %d  level %.3f  mode %s  lora %s\n",
      bpm, bpmEst, conf, onsetCount, level, now < tapLock ? "TAP" : "MIC", txOn ? "TX" : "off");
    onsetCount = 0;
  }
  delay(1);
}
