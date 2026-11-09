// Beat screen for the Heltec V3's 0.96" OLED (128x64, 1-bit): six beat-driven visuals plus a stats page.
// Keep this file identical in heltec_v3_beat_follower/ and heltec_v3_espnow_bridge/.
// Drawn into a 1-bit LovyanGFX sprite; shared with tools/preview_oled.cpp (host preview).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fonts_oled.h"

constexpr int OW = 128, OH = 64;
enum OMode : uint8_t { O_BEAT, O_PULSE, O_BARS, O_TUNNEL, O_WARP, O_SCOPE, O_STATS, O_COUNT };
static const char* const O_NAME[O_COUNT] = {"BEAT", "PULSE", "BARS", "TUNNEL", "WARP", "SCOPE", "STATS"};
enum OLink : uint8_t { OL_WAIT, OL_LIVE, OL_HOLD, OL_BOOTH };

struct OState {
  bool bridge;                 // false = follower
  uint8_t link;                // OL_*
  uint16_t bpm10;
  uint32_t beatMs, beatId;     // when the current beat started, increments every beat
  uint8_t beatPos;             // 0..3, 0 = downbeat
  uint32_t bars;
  int16_t rssi; int8_t snr4;   // last LoRa packet heard
  uint32_t pkts, lost;         // follower: packets / lost. bridge: heard / -
  uint32_t sent, clients;      // bridge: beats sent for the booth, booth controllers seen
  uint8_t mode; bool autoMode;
  uint32_t labelUntil;         // show the mode name until then
};

lgfx::VLWfont ofBpm, ofDec, ofHead;
lgfx::PointerWrapper owBpm, owDec, owHead;
void oledInitFonts() {
  owBpm.set(F_BPM, sizeof(F_BPM)); ofBpm.loadFont(&owBpm);
  owDec.set(F_DEC, sizeof(F_DEC)); ofDec.loadFont(&owDec);
  owHead.set(F_HEAD, sizeof(F_HEAD)); ofHead.loadFont(&owHead);
}

// ------------------------------------------------------------------ beat timing
static inline float operiod(const OState& v) { return 60000.0f / (v.bpm10 > 300 ? v.bpm10 / 10.0f : 120.0f); }
static inline float ophase(const OState& v, uint32_t now) { float p = (now - v.beatMs) / operiod(v); return p < 0 ? 0 : p > 1 ? 1 : p; }
static inline float opunch(const OState& v, uint32_t now) {          // 1 on the beat, decays fast
  float t = (now - v.beatMs) / operiod(v); return t < 0 ? 1 : expf(-t * 6.0f);
}
static inline uint32_t ohash(uint32_t a, uint32_t b) { uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u) * 2246822519u; h ^= h >> 15; h *= 2654435761u; return h ^ (h >> 13); }

static void otext(LGFX_Sprite& s, const lgfx::IFont* f, const char* str, int x, int y, int col, textdatum_t d) {
  s.setFont(f); s.setTextDatum(d); s.setTextColor(col); s.drawString(str, x, y);
}

// ------------------------------------------------------------------ modes
static void drawBeat(LGFX_Sprite& s, const OState& v, uint32_t now) {
  char b[24];
  const char* st = v.link == OL_LIVE ? "LIVE" : v.link == OL_BOOTH ? "BOOTH" : v.link == OL_HOLD ? "HOLD" : "WAIT";
  s.setFont(&ofHead); int pw = s.textWidth(st) + 8;
  if (v.link == OL_LIVE || v.link == OL_BOOTH) { s.fillRoundRect(OW - pw, 0, pw, 12, 3, 1); otext(s, &ofHead, st, OW - pw / 2, 10, 0, baseline_center); }
  else { s.drawRoundRect(OW - pw, 0, pw, 12, 3, 1); otext(s, &ofHead, st, OW - pw / 2, 10, 1, baseline_center); }
  s.setFont(&fonts::Font0); s.setTextDatum(baseline_left); s.setTextColor(1);
  if (v.link != OL_WAIT) { snprintf(b, sizeof(b), "BAR %lu", (unsigned long)v.bars); s.drawString(b, 0, 9); }
  else s.drawString(v.bridge ? "BEAT BRIDGE" : "BEAT FOLLOWER", 0, 9);
  // tempo sweep under the header
  if (v.link != OL_WAIT) s.drawFastHLine(0, 14, (int)(ophase(v, now) * OW), 1);
  char ip[8], dp[6];
  if (v.link != OL_WAIT && v.bpm10) { snprintf(ip, sizeof(ip), "%d", v.bpm10 / 10); snprintf(dp, sizeof(dp), ".%d", v.bpm10 % 10); }
  else { strcpy(ip, "---"); dp[0] = 0; }
  s.setFont(&ofBpm); int wi = s.textWidth(ip); s.setFont(&ofDec); int wd = dp[0] ? s.textWidth(dp) : 0;
  int x0 = (OW - wi - wd - 2) / 2;
  otext(s, &ofBpm, ip, x0, 50, 1, baseline_left);
  if (dp[0]) otext(s, &ofDec, dp, x0 + wi + 2, 50, 1, baseline_left);
  for (int i = 0; i < 4; i++) {
    int x = i * 32 + 1;
    if (v.link != OL_WAIT && v.beatPos == i) s.fillRect(x, 56, 30, 8, 1); else s.drawRect(x, 56, 30, 8, 1);
  }
}

static void drawPulse(LGFX_Sprite& s, const OState& v, uint32_t now) {
  float per = operiod(v), ph = (now - v.beatMs) / per;
  for (int k = 0; k < 4; k++) {                    // the last few beats' rings, still travelling out
    float age = ph + k;
    int r = 5 + (int)(age * 22);
    if (r > 80) continue;
    s.drawCircle(OW / 2, OH / 2, r, 1);
    bool down = ((v.beatPos - k) & 3) == 0;
    if (down) { s.drawCircle(OW / 2, OH / 2, r + 1, 1); s.drawCircle(OW / 2, OH / 2, r + 2, 1); }
  }
  int c = 3 + (int)(opunch(v, now) * 9);
  s.fillCircle(OW / 2, OH / 2, c, 1);
}

static void drawBars(LGFX_Sprite& s, const OState& v, uint32_t now) {
  static float peak[16]; static uint32_t lastMs = 0;
  float dt = lastMs ? (now - lastMs) / 1000.0f : 0; lastMs = now;
  float p = opunch(v, now);
  for (int i = 0; i < 16; i++) {
    float r = (ohash(v.beatId, i) & 1023) / 1023.0f;
    float lvl = v.beatPos == 0 ? 0.55f + 0.45f * r : 0.2f + 0.8f * r * r;
    int h = 2 + (int)(lvl * p * 60);
    int x = i * 8;
    s.fillRect(x, OH - h, 6, h, 1);
    peak[i] -= dt * 40; if (peak[i] < h) peak[i] = h;      // caps fall slowly
    s.drawFastHLine(x, OH - 1 - (int)peak[i] - 1, 6, 1);
  }
}

static void drawTunnel(LGFX_Sprite& s, const OState& v, uint32_t now) {
  float ph = ophase(v, now), p = opunch(v, now);
  float spin = now / 1000.0f * 0.6f + v.beatId * 0.35f;
  for (int k = 0; k < 7; k++) {
    float z = fmodf(k / 7.0f + ph / 7.0f + (v.beatId % 7) / 7.0f, 1.0f);   // 0 far .. 1 near
    float sz = 4 + z * z * 110 * (1 + p * 0.15f);
    float a = spin + z * 1.8f;
    int px[4], py[4];
    for (int c = 0; c < 4; c++) {
      float ang = a + c * 1.5707963f;
      px[c] = OW / 2 + (int)(cosf(ang) * sz * 0.5f); py[c] = OH / 2 + (int)(sinf(ang) * sz * 0.5f);
    }
    for (int c = 0; c < 4; c++) s.drawLine(px[c], py[c], px[(c + 1) & 3], py[(c + 1) & 3], 1);
  }
}

static void drawWarp(LGFX_Sprite& s, const OState& v, uint32_t now) {
  struct Star { float x, y, z; };
  static Star st[64]; static bool init = false; static uint32_t lastMs = 0; static uint32_t rng = 0x2468ACE1;
  auto rnd = [&]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (rng & 0xFFFF) / 65535.0f; };
  if (!init) { for (auto& t : st) { t.x = rnd() * 2 - 1; t.y = rnd() * 2 - 1; t.z = rnd(); } init = true; }
  float dt = lastMs ? (now - lastMs) / 1000.0f : 0; lastMs = now; if (dt > 0.1f) dt = 0.1f;
  float speed = 0.25f + opunch(v, now) * (v.beatPos == 0 ? 2.6f : 1.4f);
  for (auto& t : st) {
    float pz = t.z;
    t.z -= speed * dt;
    if (t.z <= 0.03f) { t.x = rnd() * 2 - 1; t.y = rnd() * 2 - 1; t.z = 1; continue; }
    int x0 = OW / 2 + (int)(t.x / pz * 40), y0 = OH / 2 + (int)(t.y / pz * 40);
    int x1 = OW / 2 + (int)(t.x / t.z * 40), y1 = OH / 2 + (int)(t.y / t.z * 40);
    s.drawLine(x0, y0, x1, y1, 1);
  }
}

static void drawScope(LGFX_Sprite& s, const OState& v, uint32_t now) {
  float p = opunch(v, now), t = now / 1000.0f;
  float amp = 3 + p * (v.beatPos == 0 ? 28 : 18);
  int py = OH / 2;
  for (int x = 0; x < OW; x++) {
    float u = x / (float)OW;
    float env = sinf(u * 3.14159f);                         // pinned at the edges
    float y = sinf(u * 18 + t * 7) * 0.7f + sinf(u * 41 - t * 11) * 0.3f;
    int yy = OH / 2 - (int)(y * amp * env);
    if (x) s.drawLine(x - 1, py, x, yy, 1);
    py = yy;
  }
  s.drawFastHLine(0, OH / 2, 3, 1); s.drawFastHLine(OW - 3, OH / 2, 3, 1);
}

static void drawStats(LGFX_Sprite& s, const OState& v, uint32_t now) {
  char b[24];
  auto row = [&](int y, const char* k, const char* val) {
    s.setFont(&fonts::Font0); s.setTextColor(1); s.setTextDatum(top_left); s.drawString(k, 0, y); s.setTextDatum(top_right); s.drawString(val, OW, y);
  };
  otext(s, &ofHead, v.bridge ? "BRIDGE" : "FOLLOWER", 0, 10, 1, baseline_left);
  s.drawFastHLine(0, 13, OW, 1);
  if (v.rssi) snprintf(b, sizeof(b), "%d dBm", v.rssi); else strcpy(b, "-");
  row(16, "LORA RSSI", b);
  if (v.rssi) snprintf(b, sizeof(b), "%.1f dB", v.snr4 / 4.0f); else strcpy(b, "-");
  row(25, "SNR", b);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.pkts); row(34, v.bridge ? "HEARD" : "BEATS IN", b);
  if (v.bridge) {
    snprintf(b, sizeof(b), "%lu", (unsigned long)v.sent); row(43, "SENT (BOOTH)", b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)v.clients); row(52, "BOOTH GEAR", b);
  } else {
    snprintf(b, sizeof(b), "%lu", (unsigned long)v.lost); row(43, "LOST", b);
    snprintf(b, sizeof(b), "%.1f", v.bpm10 / 10.0f); row(52, "BPM", v.bpm10 ? b : "-");
  }
  // signal bar
  int lvl = v.rssi ? (v.rssi + 120) * 40 / 70 : 0; lvl = lvl < 0 ? 0 : lvl > 40 ? 40 : lvl;
  s.drawRect(OW - 42, 0, 42, 8, 1); s.fillRect(OW - 41, 1, lvl, 6, 1);
}

// ------------------------------------------------------------------ frame
void oledDraw(LGFX_Sprite& s, const OState& v, uint32_t now) {
  s.fillScreen(0);
  switch (v.mode) {
    case O_PULSE: drawPulse(s, v, now); break;
    case O_BARS: drawBars(s, v, now); break;
    case O_TUNNEL: drawTunnel(s, v, now); break;
    case O_WARP: drawWarp(s, v, now); break;
    case O_SCOPE: drawScope(s, v, now); break;
    case O_STATS: drawStats(s, v, now); break;
    default: drawBeat(s, v, now); break;
  }
  bool visual = v.mode != O_BEAT && v.mode != O_STATS;
  if ((int32_t)(v.labelUntil - now) > 0) {                 // mode name after a change
    char b[24]; snprintf(b, sizeof(b), "%s%s", O_NAME[v.mode], v.autoMode && visual ? " AUTO" : "");
    s.setFont(&ofHead); int w = s.textWidth(b) + 12;
    s.fillRoundRect(OW / 2 - w / 2, 2, w, 15, 4, 0); s.drawRoundRect(OW / 2 - w / 2, 2, w, 15, 4, 1);
    otext(s, &ofHead, b, OW / 2, 14, 1, baseline_center);
  } else if (visual && v.link == OL_WAIT) {                // visuals idle along at 120 BPM until the beat arrives
    const char* m = v.bridge ? "WAITING FOR BEAT" : "LISTENING 916.0";
    s.setFont(&fonts::Font0); int w = s.textWidth(m) + 8;
    s.fillRect(OW / 2 - w / 2, OH - 11, w, 11, 0); s.drawRect(OW / 2 - w / 2, OH - 11, w, 11, 1);
    s.setTextDatum(middle_center); s.setTextColor(1); s.drawString(m, OW / 2, OH - 5);
  }
}

// SSD1306 page bytes for page p (8 rows) from the 1-bit sprite (row-major, MSB = leftmost pixel)
static void oledPage(LGFX_Sprite& s, int p, uint8_t* out) {
  const uint8_t* buf = (const uint8_t*)s.getBuffer();
  const int stride = OW / 8;
  for (int x = 0; x < OW; x++) {
    uint8_t v = 0, mask = 0x80 >> (x & 7);
    for (int bit = 0; bit < 8; bit++) if (buf[(p * 8 + bit) * stride + (x >> 3)] & mask) v |= 1 << bit;
    out[x] = v;
  }
}
