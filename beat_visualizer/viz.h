// Beat Visualizer: beat-driven visuals for any colour screen (drawn into a sprite of any size).
// Everything is driven by the beat link: the time of the last beat, the BPM, the beat in the bar and the
// conductor's hue. Shared by the firmware and tools/preview_viz.cpp (host preview / GIF), LovyanGFX calls only.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fonts_viz.h"

enum VizMode : uint8_t { V_PULSE, V_TUNNEL, V_BARS, V_STARS, V_PLASMA, V_BPM, V_COUNT };
static const char* const VIZ_NAME[V_COUNT] = {"PULSE", "TUNNEL", "BARS", "WARP", "PLASMA", "BPM"};
enum VizSrc : uint8_t { SRC_DEMO, SRC_W10, SRC_BOOTH };

struct VizState {
  float bpm;            // beats per minute (demo uses 120)
  uint32_t beatMs;      // when the current beat started (millis)
  uint32_t beatId;      // increments every beat (real or flywheel)
  uint8_t beatPos;      // 0..3, 0 = downbeat
  uint8_t hue;          // conductor's colour, changes every bar
  uint32_t bars;
  uint8_t src;          // SRC_DEMO when nothing is on the link
  uint8_t mode; bool autoMode;
  uint32_t labelUntil;  // show the mode name until then
  bool hud;             // small BPM readout in the corner
  const char* hint;     // shown instead of the mode name while a gesture is in progress
};

// ------------------------------------------------------------------ colour
static inline uint16_t vc565(int r, int g, int b) {
  r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
static void vhue(uint8_t h, int& r, int& g, int& b) {
  int reg = h / 43, rem = (h - reg * 43) * 6, q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}
static uint16_t vhueCol(uint8_t h, float lvl) { int r, g, b; vhue(h, r, g, b); return vc565(r * lvl, g * lvl, b * lvl); }
static uint16_t vmix(uint8_t h, float lvl, float white) {   // hue at a level, washed towards white
  int r, g, b; vhue(h, r, g, b);
  return vc565((r + (255 - r) * white) * lvl, (g + (255 - g) * white) * lvl, (b + (255 - b) * white) * lvl);
}
static const uint16_t V_AMB = 0xFD80, V_DIM = 0x6B4D, V_BG = 0x0000;

// ------------------------------------------------------------------ fonts
lgfx::VLWfont vfBig, vfDec, vfLbl;
lgfx::PointerWrapper vwBig, vwDec, vwLbl;
void vizInitFonts() {
  vwBig.set(F_BIG, sizeof(F_BIG)); vfBig.loadFont(&vwBig);
  vwDec.set(F_DEC, sizeof(F_DEC)); vfDec.loadFont(&vwDec);
  vwLbl.set(F_LBL, sizeof(F_LBL)); vfLbl.loadFont(&vwLbl);
}

// ------------------------------------------------------------------ helpers
static uint32_t vrng = 0x1234567;
static inline float vrand() { vrng ^= vrng << 13; vrng ^= vrng >> 17; vrng ^= vrng << 5; return (vrng & 0xFFFFFF) / 16777216.0f; }
static inline float vperiod(const VizState& v) { return 60000.0f / (v.bpm > 30 ? v.bpm : 120); }
static inline float vphase(const VizState& v, uint32_t now) {        // 0 at the beat .. 1 at the next
  float p = (now - v.beatMs) / vperiod(v); return p < 0 ? 0 : p > 1 ? 1 : p;
}
static inline float vpunch(const VizState& v, uint32_t now) {        // 1 right on the beat, decays fast
  float p = vphase(v, now); float k = 1 - p; return k * k * k;
}

// ------------------------------------------------------------------ PULSE: rings from the centre
struct Ring { uint32_t t0; uint8_t hue; bool down; };
static Ring rings[8]; static int ringN = 0; static uint32_t lastRingBeat = 0xFFFFFFFF;
static void drawPulse(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cx = W / 2, cy = H / 2;
  float maxR = sqrtf((float)cx * cx + (float)cy * cy);
  if (v.beatId != lastRingBeat) {
    lastRingBeat = v.beatId;
    memmove(rings + 1, rings, sizeof(Ring) * 7); rings[0] = {v.beatMs, v.hue, v.beatPos == 0}; if (ringN < 8) ringN++;
  }
  float life = vperiod(v) * 3.2f;
  for (int i = ringN - 1; i >= 0; i--) {
    float a = (now - rings[i].t0) / life; if (a > 1) continue;
    int r = (int)(a * maxR), th = rings[i].down ? 10 : 6;
    float f = 1 - a; uint16_t c = vmix(rings[i].hue + i * 6, (0.35f + 0.65f * f) * (rings[i].down ? 1.0f : 0.8f), 0.25f * f * f);
    s.fillArc(cx, cy, r, r + th, 0, 360, c);
  }
  float k = vpunch(v, now);
  int core = (int)(H * (0.08f + 0.12f * k) * (v.beatPos == 0 ? 1.3f : 1));
  s.fillCircle(cx, cy, core, vmix(v.hue, 0.55f + 0.45f * k, 0.35f * k));
}

// ------------------------------------------------------------------ TUNNEL: rotating squares flying past
static float tunZ[10]; static bool tunInit = false; static uint32_t tunLast = 0; static float tunAng = 0;
static void drawTunnel(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cx = W / 2, cy = H / 2;
  if (!tunInit) { for (int i = 0; i < 10; i++) tunZ[i] = 0.1f + i * 0.1f; tunInit = true; tunLast = now; }
  float dt = (now - tunLast) / 1000.0f; tunLast = now; if (dt > 0.1f) dt = 0.1f;
  float k = vpunch(v, now), speed = v.bpm / 120.0f * (0.35f + 1.2f * k);
  tunAng += dt * (0.6f + 0.8f * k) * (v.bars % 2 ? 1 : -1);
  for (int i = 0; i < 10; i++) { tunZ[i] -= dt * speed; if (tunZ[i] <= 0.05f) tunZ[i] += 1.0f; }
  // draw far to near
  int order[10]; for (int i = 0; i < 10; i++) order[i] = i;
  for (int i = 0; i < 10; i++) for (int j = i + 1; j < 10; j++) if (tunZ[order[j]] > tunZ[order[i]]) { int t = order[i]; order[i] = order[j]; order[j] = t; }
  float base = (W < H ? W : H) * 0.12f;
  for (int n = 0; n < 10; n++) {
    int i = order[n]; float z = tunZ[i], sz = base / z;
    if (sz > W * 1.5f) continue;
    float a = tunAng + z * 2.2f, ca = cosf(a) * sz, sa = sinf(a) * sz;
    int px[4], py[4];
    for (int c = 0; c < 4; c++) {
      float ux = (c == 0 || c == 3) ? -1 : 1, uy = c < 2 ? -1 : 1;
      px[c] = cx + (int)(ux * ca - uy * sa); py[c] = cy + (int)(ux * sa + uy * ca);
    }
    float lvl = (1.0f - z) * (0.55f + 0.45f * k);
    uint16_t col = vmix(v.hue + i * 12, lvl, z < 0.25f ? 0.4f * k : 0);
    for (int c = 0; c < 4; c++) {
      int d = (c + 1) & 3;
      s.drawLine(px[c], py[c], px[d], py[d], col);
      s.drawLine(px[c] + 1, py[c], px[d] + 1, py[d], col);
    }
  }
}

// ------------------------------------------------------------------ BARS: a spectrum that jumps on the beat
static float barH[24], barPeak[24], barTarget[24]; static uint32_t barBeat = 0xFFFFFFFF, barLast = 0;
static void drawBars(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), n = W >= 300 ? 24 : 16;
  float dt = (now - barLast) / 1000.0f; barLast = now; if (dt > 0.1f) dt = 0.1f;
  if (v.beatId != barBeat) {
    barBeat = v.beatId;
    for (int i = 0; i < n; i++) {
      float shape = v.beatPos == 0 ? 1.0f - 0.5f * i / n : 0.45f + 0.5f * vrand();   // downbeat: big bass end
      barTarget[i] = shape * (0.55f + 0.45f * vrand());
    }
  }
  float decay = 1.6f * v.bpm / 120.0f;
  int gap = 2, bw = (W - gap * (n + 1)) / n, x0 = (W - (bw * n + gap * (n - 1))) / 2;
  for (int i = 0; i < n; i++) {
    if (barTarget[i] > barH[i]) barH[i] += (barTarget[i] - barH[i]) * 0.6f;
    else barH[i] -= dt * decay * 0.6f;
    if (barH[i] < 0.03f) barH[i] = 0.03f;
    barTarget[i] -= dt * decay; if (barTarget[i] < 0) barTarget[i] = 0;
    if (barH[i] > barPeak[i]) barPeak[i] = barH[i]; else barPeak[i] -= dt * 0.5f;
    int h = (int)(barH[i] * (H - 10)), x = x0 + i * (bw + gap);
    uint8_t hh = v.hue + i * (160 / n);
    for (int seg = 0; seg < h; seg += 4) {
      float lvl = 0.35f + 0.65f * seg / (float)(H - 10);
      s.fillRect(x, H - 4 - seg - 3, bw, 3, vhueCol(hh, lvl));
    }
    int py = H - 4 - (int)(barPeak[i] * (H - 10)) - 3;
    s.fillRect(x, py, bw, 2, vmix(hh, 1, 0.6f));
  }
}

// ------------------------------------------------------------------ WARP: starfield, kicks on the beat
struct Star { float x, y, z, pz; };
static Star stars[140]; static bool starInit = false; static uint32_t starLast = 0;
static void drawStars(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cx = W / 2, cy = H / 2, n = W * H > 50000 ? 140 : 80;
  if (!starInit) { for (int i = 0; i < 140; i++) stars[i] = {vrand() * 2 - 1, vrand() * 2 - 1, vrand(), 1}; starInit = true; starLast = now; }
  float dt = (now - starLast) / 1000.0f; starLast = now; if (dt > 0.1f) dt = 0.1f;
  float k = vpunch(v, now), speed = (0.25f + 1.6f * k * (v.beatPos == 0 ? 1.4f : 1)) * v.bpm / 120.0f;
  float f = (W > H ? W : H) * 0.5f;
  for (int i = 0; i < n; i++) {
    Star& st = stars[i];
    st.pz = st.z; st.z -= dt * speed;
    if (st.z <= 0.02f) { st = {vrand() * 2 - 1, vrand() * 2 - 1, 1, 1}; continue; }
    int x1 = cx + (int)(st.x * f / st.z), y1 = cy + (int)(st.y * f / st.z);
    int x0 = cx + (int)(st.x * f / st.pz), y0 = cy + (int)(st.y * f / st.pz);
    if (x1 < 0 || x1 >= W || y1 < 0 || y1 >= H) { st = {vrand() * 2 - 1, vrand() * 2 - 1, 1, 1}; continue; }
    float lvl = 1.0f - st.z;
    uint16_t c = vmix(v.hue + (i & 31), lvl, 0.5f * lvl);
    s.drawLine(x0, y0, x1, y1, c);
    if (st.z < 0.3f) s.drawLine(x0 + 1, y0, x1 + 1, y1, c);
  }
}

// ------------------------------------------------------------------ PLASMA: colour field that pumps
static void drawPlasma(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cell = W >= 300 ? 10 : 8;
  float t = now / 1000.0f * v.bpm / 120.0f, k = vpunch(v, now);
  float lvl = 0.30f + 0.70f * k;
  for (int y = 0; y < H; y += cell) {
    float fy = y / (float)H;
    for (int x = 0; x < W; x += cell) {
      float fx = x / (float)W;
      float p = sinf(fx * 7 + t) + sinf(fy * 6 - t * 1.3f) + sinf((fx + fy) * 5 + t * 0.7f)
              + sinf(sqrtf((fx - 0.5f) * (fx - 0.5f) + (fy - 0.5f) * (fy - 0.5f)) * 14 - t * 2);
      uint8_t h = v.hue + (int)(p * 22);
      float b = 0.55f + 0.45f * sinf(p * 1.7f);
      s.fillRect(x, y, cell, cell, vmix(h, lvl * b, 0.25f * k));
    }
  }
}

// ------------------------------------------------------------------ BPM: the number, big
static void drawBpm(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height();
  float k = vpunch(v, now);
  if (v.beatPos == 0 && k > 0.2f) s.fillScreen(vhueCol(v.hue, 0.25f * k));
  char ip[8], dp[6]; int t = (int)lroundf(v.bpm * 10);
  snprintf(ip, sizeof(ip), "%d", t / 10); snprintf(dp, sizeof(dp), ".%d", t % 10);
  s.setFont(&vfBig); int wi = s.textWidth(ip); s.setFont(&vfDec); int wd = s.textWidth(dp);
  int x0 = (W - wi - wd - 4) / 2, base = H / 2 + 22;
  uint16_t c = k > 0.6f ? 0xFFFF : vmix(v.hue, 1, 0.55f);
  s.setTextDatum(baseline_left); s.setTextColor(c);
  s.setFont(&vfBig); s.drawString(ip, x0, base);
  s.setFont(&vfDec); s.drawString(dp, x0 + wi + 4, base);
  int bw = (W - 50) / 4;
  for (int i = 0; i < 4; i++) {
    int x = 10 + i * (bw + 10), y = H - 22;
    if (i == v.beatPos) s.fillRoundRect(x, y, bw, 12, 4, vhueCol(v.hue, 0.5f + 0.5f * k));
    else s.drawRoundRect(x, y, bw, 12, 4, vhueCol(v.hue, 0.35f));
  }
  char b[24]; snprintf(b, sizeof(b), "BAR %lu", (unsigned long)v.bars);
  s.setFont(&vfLbl); s.setTextColor(V_DIM); s.setTextDatum(top_left); s.drawString(b, 8, 6);
}

// ------------------------------------------------------------------ frame
void vizDraw(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  s.fillScreen(V_BG);
  switch (v.mode) {
    case V_PULSE: drawPulse(s, v, now); break;
    case V_TUNNEL: drawTunnel(s, v, now); break;
    case V_BARS: drawBars(s, v, now); break;
    case V_STARS: drawStars(s, v, now); break;
    case V_PLASMA: drawPlasma(s, v, now); break;
    default: drawBpm(s, v, now); break;
  }
  int W = s.width(), H = s.height();
  s.setFont(&vfLbl);
  if (v.hint || (int32_t)(v.labelUntil - now) > 0) {    // mode name after a change, or a gesture hint
    char b[48];
    if (v.hint) snprintf(b, sizeof(b), "%s", v.hint); else snprintf(b, sizeof(b), "%s%s", VIZ_NAME[v.mode], v.autoMode ? "  AUTO" : "");
    int w = s.textWidth(b) + 20;
    s.fillRoundRect(W / 2 - w / 2, 6, w, 24, 12, V_BG); s.drawRoundRect(W / 2 - w / 2, 6, w, 24, 12, V_AMB);
    s.setTextDatum(middle_center); s.setTextColor(V_AMB); s.drawString(b, W / 2, 19);
  }
  if (v.src == SRC_DEMO) {
    s.setTextDatum(bottom_center); s.setTextColor(V_DIM); s.drawString(W < 200 ? "DEMO" : "DEMO - waiting for the beat link", W / 2, H - 3);
  } else if (v.hud && v.mode != V_BPM) {
    char b[16]; snprintf(b, sizeof(b), "%.1f", v.bpm);
    s.setTextDatum(bottom_right); s.setTextColor(V_DIM); s.drawString(b, W - 4, H - 3);
  }
}
