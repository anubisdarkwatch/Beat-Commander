// Beat Visualizer: beat-driven visuals for any colour screen (drawn into a sprite of any size).
// Everything is driven by the beat link: the time of the last beat, the BPM, the beat in the bar and the
// conductor's hue. Shared by the firmware and tools/preview_viz.cpp (host preview / GIF), LovyanGFX calls only.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "fonts_viz.h"

enum VizMode : uint8_t { V_PULSE, V_TUNNEL, V_BARS, V_STARS, V_PLASMA, V_BPM, V_KALEIDO, V_GRID, V_PYRAMID, V_FIREWORKS, V_RAIN, V_COUNT };
static const char* const VIZ_NAME[V_COUNT] = {"PULSE", "TUNNEL", "BARS", "WARP", "PLASMA", "BPM", "KALEIDO", "GRID", "PYRAMID", "FIREWORKS", "RAIN"};
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
lgfx::VLWfont vfBig, vfDec, vfLbl, vfRain, vfAnkh;
lgfx::PointerWrapper vwBig, vwDec, vwLbl, vwRain, vwAnkh;
void vizInitFonts() {
  vwBig.set(F_BIG, sizeof(F_BIG)); vfBig.loadFont(&vwBig);
  vwDec.set(F_DEC, sizeof(F_DEC)); vfDec.loadFont(&vwDec);
  vwLbl.set(F_LBL, sizeof(F_LBL)); vfLbl.loadFont(&vwLbl);
  vwRain.set(F_RAIN, sizeof(F_RAIN)); vfRain.loadFont(&vwRain);
  vwAnkh.set(F_ANKH, sizeof(F_ANKH)); vfAnkh.loadFont(&vwAnkh);
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

// ------------------------------------------------------------------ KALEIDO: mirrored shards, a new pattern every beat
static void drawKaleido(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cx = W / 2, cy = H / 2;
  const int K = 6;                                         // 6 wedges, each mirrored = 12-fold
  float R = sqrtf((float)cx * cx + (float)cy * cy);
  float k = vpunch(v, now), ph = vphase(v, now);
  static float spin = 0; static uint32_t last = 0;
  float dt = last ? (now - last) / 1000.0f : 0; last = now; if (dt > 0.1f) dt = 0.1f;
  spin += dt * (0.25f + 0.9f * k) * (v.bars & 1 ? -1 : 1);   // flips direction every bar
  uint32_t seed = v.beatId * 2654435761u;
  for (int sh = 0; sh < 7; sh++) {
    uint32_t h = seed ^ (sh * 0x9E3779B9u); h ^= h >> 13; h *= 0x85EBCA6Bu; h ^= h >> 16;
    float r0 = R * (0.08f + 0.85f * ((h & 255) / 255.0f)) * (0.85f + 0.25f * k + 0.1f * ph);
    float a0 = ((h >> 8) & 255) / 255.0f * (3.14159f / K);
    float sz = R * (0.10f + 0.18f * (((h >> 16) & 255) / 255.0f)) * (1 + 0.35f * k);
    uint8_t hh = v.hue + ((h >> 24) & 63) - 16;
    uint16_t col = vmix(hh, 0.35f + 0.65f * (1 - ph * 0.6f), (v.beatPos == 0 ? 0.45f : 0.15f) * k);
    for (int w = 0; w < K * 2; w++) {
      float base = spin + (w / 2) * (6.28318f / K);
      float sgn = (w & 1) ? -1 : 1;                        // odd wedges are mirror images
      float pa[3] = {a0, a0 + 0.30f, a0 - 0.25f}, pr[3] = {r0, r0 + sz, r0 + sz * 0.6f};
      int x[3], y[3];
      for (int c = 0; c < 3; c++) { float a = base + sgn * pa[c]; x[c] = cx + (int)(cosf(a) * pr[c]); y[c] = cy + (int)(sinf(a) * pr[c]); }
      s.fillTriangle(x[0], y[0], x[1], y[1], x[2], y[2], col);
    }
  }
  s.fillCircle(cx, cy, (int)(R * (0.04f + 0.06f * k)), vmix(v.hue, 1, 0.6f * k));
}

// ------------------------------------------------------------------ GRID: synthwave floor, sun on the horizon
static void drawGrid(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cx = W / 2, hy = (int)(H * 0.48f);
  float k = vpunch(v, now);
  // sky glow + sun with stripes cut out of its lower half
  for (int y = 0; y < hy; y += 3) s.fillRect(0, y, W, 3, vhueCol(v.hue + 20, 0.02f + 0.16f * (float)y / hy));
  int sr = (int)(H * (0.22f + 0.05f * k * (v.beatPos == 0 ? 1.6f : 1)));
  for (int y = -sr; y <= 0; y++) {
    int row = hy + y - 2;
    float f = (float)(-y) / sr;                           // 1 at the top of the sun, 0 at the horizon
    if (f < 0.55f && ((int)(row / 4.0f + now / 120.0f) % 3) == 0) continue;   // scrolling cut-out bands
    int hw = (int)sqrtf((float)sr * sr - (float)y * y);
    s.drawFastHLine(cx - hw, row, hw * 2, vmix(v.hue + 40 - (int)(40 * f), 0.75f + 0.25f * k, 0.25f * f));
  }
  // floor
  s.fillRect(0, hy, W, H - hy, vhueCol(v.hue, 0.04f));
  float speed = v.bpm / 120.0f * (0.6f + 1.4f * k);
  static float off = 0; static uint32_t last = 0;
  float dt = last ? (now - last) / 1000.0f : 0; last = now; if (dt > 0.1f) dt = 0.1f;
  off += dt * speed; off -= floorf(off);
  uint16_t line = vmix(v.hue, 0.45f + 0.55f * k, 0.3f * k);
  for (int i = 0; i < 12; i++) {                           // horizontal lines rushing towards you
    float z = (i + 1 - off) / 12.0f; if (z <= 0) continue;
    int y = hy + (int)((H - hy) * (0.04f / (z * z + 0.04f) - 0.04f / 1.04f) / (1 - 0.04f / 1.04f));
    y = hy + (int)((H - hy) * (1 - z) * (1 - z));
    if (y > hy && y < H) s.drawFastHLine(0, y, W, line);
  }
  for (int i = -10; i <= 10; i++) {                        // lines to the vanishing point
    int xb = cx + i * W / 7;
    s.drawLine(cx + i * 4, hy, xb, H, line);
  }
  s.drawFastHLine(0, hy, W, vmix(v.hue + 30, 1, 0.5f));
}

// ------------------------------------------------------------------ PYRAMID: glowing pyramid, rays on the beat, the ankh above
static void drawPyramid(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height(), cx = W / 2;
  float k = vpunch(v, now);
  bool down = v.beatPos == 0;
  int base = H - (H < 120 ? 6 : 14), ph = (int)(H * 0.48f), hw = (int)(ph * 1.15f);
  int ax = cx, ay = base - ph;
  // rays from the apex
  int nr = down ? 24 : 12; float len = (W + H) * (0.25f + 0.55f * k);
  for (int i = 0; i < nr; i++) {
    float a = -3.14159f + 3.14159f * (i + 0.5f) / nr + (v.beatId & 1 ? 0.06f : -0.06f);
    if (sinf(a) > -0.05f) continue;                       // only above the horizon
    s.drawLine(ax, ay, ax + (int)(cosf(a) * len), ay + (int)(sinf(a) * len), vhueCol(v.hue + 20, 0.15f + 0.6f * k));
  }
  // ground and dunes
  s.fillRect(0, base, W, H - base, vhueCol(v.hue + 30, 0.12f));
  // the pyramid: two faces, brick lines, outline
  s.fillTriangle(ax, ay, cx - hw, base, cx, base, vhueCol(v.hue + 30, 0.30f + 0.25f * k));
  s.fillTriangle(ax, ay, cx + hw, base, cx, base, vhueCol(v.hue + 30, 0.16f + 0.15f * k));
  for (int r = 1; r < 7; r++) {
    int y = ay + ph * r / 7, w = hw * r / 7;
    s.drawFastHLine(cx - w, y, w * 2, vhueCol(v.hue + 30, 0.10f + 0.1f * k));
  }
  uint16_t edge = vmix(v.hue, 0.7f + 0.3f * k, (down ? 0.5f : 0.2f) * k);
  s.drawLine(ax, ay, cx - hw, base, edge); s.drawLine(ax, ay, cx + hw, base, edge);
  s.drawLine(ax, ay + 1, cx - hw, base + 1, edge); s.drawLine(ax, ay + 1, cx + hw, base + 1, edge);
  s.fillCircle(ax, ay, 2 + (int)(5 * k), vmix(v.hue, 1, 0.7f * k));   // the capstone
  // the ankh, floating above and bobbing with the beat
  if (H >= 180) {
    int y = ay - 8 - (int)(6 * (1 - vphase(v, now)));
    s.setFont(&vfAnkh); s.setTextDatum(bottom_center); s.setTextColor(vmix(v.hue + 20, 0.55f + 0.45f * k, 0.45f * k));
    s.drawString("\xEE\x80\x80", ax, y);
  }
}

// ------------------------------------------------------------------ FIREWORKS: a burst on every beat, the big one on beat 1
struct Spark { float x, y, vx, vy; uint16_t life, age; uint8_t hue; };
static Spark sparks[220]; static int sparkN = 0; static uint32_t fwBeat = 0xFFFFFFFF, fwLast = 0;
static void drawFireworks(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height();
  float dt = fwLast ? (now - fwLast) / 1000.0f : 0; fwLast = now; if (dt > 0.1f) dt = 0.1f;
  float scale = (W < H ? W : H) / 135.0f;
  if (v.beatId != fwBeat) {
    fwBeat = v.beatId;
    bool down = v.beatPos == 0;
    int n = down ? 70 : 26;
    float bx = W * (0.2f + 0.6f * vrand()), by = H * (0.2f + 0.35f * vrand());
    float sp = (down ? 95 : 60) * scale;
    for (int i = 0; i < n; i++) {
      if (sparkN >= 220) { memmove(sparks, sparks + 1, sizeof(Spark) * 219); sparkN = 219; }
      float a = 6.28318f * i / n + vrand() * 0.2f, m = sp * (0.6f + 0.4f * vrand());
      sparks[sparkN++] = {bx, by, cosf(a) * m, sinf(a) * m, (uint16_t)(down ? 1900 : 1300), 0, (uint8_t)(v.hue + (down ? (i & 1) * 40 : 0))};
    }
  }
  float g = 70 * scale;
  int out = 0;
  for (int i = 0; i < sparkN; i++) {
    Spark& p = sparks[i];
    p.age += (uint16_t)(dt * 1000);
    if (p.age >= p.life) continue;
    float px = p.x, py = p.y;
    p.vx *= 1 - dt * 0.9f; p.vy = p.vy * (1 - dt * 0.9f) + g * dt;
    p.x += p.vx * dt; p.y += p.vy * dt;
    float f = 1 - (float)p.age / p.life;
    uint16_t c = vmix(p.hue, 0.25f + 0.75f * f, f > 0.75f ? (f - 0.75f) * 2.5f : 0);
    s.drawLine((int)px, (int)py, (int)p.x, (int)p.y, c);
    if (f > 0.15f) s.fillRect((int)p.x, (int)p.y, 2, 2, c);
    sparks[out++] = p;
  }
  sparkN = out;
}

// ------------------------------------------------------------------ RAIN: falling hieroglyphs that surge on the beat
static void drawRain(LGFX_Sprite& s, const VizState& v, uint32_t now) {
  int W = s.width(), H = s.height();
  const int CW = 20, CH = 20;
  int cols = W / CW, rows = H / CH + 1;
  static float head[40]; static float spd[40]; static bool init = false; static uint32_t last = 0;
  if (!init) { for (int i = 0; i < 40; i++) { head[i] = vrand() * rows; spd[i] = 0.6f + vrand() * 0.8f; } init = true; }
  float dt = last ? (now - last) / 1000.0f : 0; last = now; if (dt > 0.1f) dt = 0.1f;
  float k = vpunch(v, now), boost = 1 + 3.0f * k * (v.beatPos == 0 ? 1.5f : 1);
  s.setFont(&vfRain); s.setTextDatum(top_center);
  char g[4] = {(char)0xEE, (char)0x80, 0, 0};
  for (int c = 0; c < cols && c < 40; c++) {
    head[c] += dt * spd[c] * v.bpm / 60.0f * 2.2f * boost;
    if (head[c] > rows + 14) { head[c] = -vrand() * 3; spd[c] = 0.6f + vrand() * 0.8f; }
    int hrow = (int)head[c];
    for (int t = 0; t < 14; t++) {
      int r = hrow - t; if (r < 0 || r >= rows) continue;
      uint32_t h = (uint32_t)(c * 73856093u ^ r * 19349663u ^ ((v.beatId + (t == 0 ? 0 : r)) / 2) * 83492791u);
      g[2] = (char)(0x81 + (h >> 7) % 24);
      float f = 1 - t / 14.0f;
      uint16_t col = t == 0 ? vmix(v.hue, 1, 0.55f + 0.45f * k) : vhueCol(v.hue + (c & 7) * 3, (0.15f + 0.85f * f) * (0.6f + 0.4f * k));
      s.setTextColor(col);
      s.drawString(g, c * CW + CW / 2 + (W - cols * CW) / 2, r * CH);
    }
  }
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
    case V_KALEIDO: drawKaleido(s, v, now); break;
    case V_GRID: drawGrid(s, v, now); break;
    case V_PYRAMID: drawPyramid(s, v, now); break;
    case V_FIREWORKS: drawFireworks(s, v, now); break;
    case V_RAIN: drawRain(s, v, now); break;
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
