// Tiny band renderer: the screen is drawn in full-width horizontal bands (BAND_H rows at a time),
// so the whole 320x240 UI needs only ~15 KB of RAM. Every primitive takes screen coordinates and
// clips itself to the current band. Plain C++ so the same code renders previews on a PC.
#pragma once
#include <stdint.h>
#include <string.h>
#include "fonts.h"

#ifndef SCR_W
#define SCR_W 320
#define SCR_H 240
#endif
#define BAND_H 24

#ifdef ARDUINO
#include <pgmspace.h>
#define RD8(p) pgm_read_byte(p)
#else
#define RD8(p) (*(const uint8_t*)(p))
#endif

struct Band { uint16_t px[SCR_W * BAND_H]; int y0, h; };
static Band band;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }

// brand palette
static const uint16_t C_BLACK = 0x0000;
static const uint16_t C_AMB   = rgb(255, 176, 0);     // #ffb000
static const uint16_t C_GRN   = rgb(57, 255, 20);     // #39ff14
static const uint16_t C_RED   = rgb(255, 40, 40);
static const uint16_t C_WHITE = rgb(255, 255, 255);
static const uint16_t C_DIM   = rgb(90, 70, 20);      // dim amber for frames
static const uint16_t C_GREY  = rgb(110, 110, 110);
static const uint16_t C_PANEL = rgb(14, 10, 2);

static inline uint16_t blend(uint16_t bg, uint16_t fg, uint8_t a) {   // a: 0..255
  uint32_t a5 = (a + 4) >> 3;                         // 0..32
  if (a5 >= 32) return fg;
  if (a5 == 0) return bg;
  uint32_t rb = ((fg & 0xF81F) * a5 + (bg & 0xF81F) * (32 - a5)) >> 5;
  uint32_t g  = ((fg & 0x07E0) * a5 + (bg & 0x07E0) * (32 - a5)) >> 5;
  return (rb & 0xF81F) | (g & 0x07E0);
}
static inline uint16_t scale(uint16_t c, uint8_t a) { return blend(0, c, a); }

static inline bool bandHas(int y, int h) { return y < band.y0 + band.h && y + h > band.y0; }

static void fillRect(int x, int y, int w, int h, uint16_t c) {
  int x0 = x < 0 ? 0 : x, x1 = x + w > SCR_W ? SCR_W : x + w;
  int y0 = y < band.y0 ? band.y0 : y, y1 = y + h > band.y0 + band.h ? band.y0 + band.h : y + h;
  for (int yy = y0; yy < y1; yy++) {
    uint16_t* p = band.px + (yy - band.y0) * SCR_W;
    for (int xx = x0; xx < x1; xx++) p[xx] = c;
  }
}
static inline void hline(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }
static inline void vline(int x, int y, int h, uint16_t c) { fillRect(x, y, 1, h, c); }
static void rect(int x, int y, int w, int h, uint16_t c) { hline(x, y, w, c); hline(x, y + h - 1, w, c); vline(x, y, h, c); vline(x + w - 1, y, h, c); }

// HUD corner brackets
static void brackets(int x, int y, int w, int h, int len, uint16_t c) {
  hline(x, y, len, c); vline(x, y, len, c);
  hline(x + w - len, y, len, c); vline(x + w - 1, y, len, c);
  hline(x, y + h - 1, len, c); vline(x, y + h - len, len, c);
  hline(x + w - len, y + h - 1, len, c); vline(x + w - 1, y + h - len, len, c);
}

// CRT scanlines: darken every other row of a rect
static void scanlines(int x, int y, int w, int h, uint8_t keep) {
  int y0 = y < band.y0 ? band.y0 : y, y1 = y + h > band.y0 + band.h ? band.y0 + band.h : y + h;
  for (int yy = y0; yy < y1; yy++) {
    if (yy & 1) continue;
    uint16_t* p = band.px + (yy - band.y0) * SCR_W;
    for (int xx = x; xx < x + w && xx < SCR_W; xx++) p[xx] = scale(p[xx], keep);
  }
}

// 4-bit alpha bitmap (rows packed 2 px/byte)
static void alpha4(const uint8_t* bmp, int w, int h, int x, int y, uint16_t c) {
  if (!bandHas(y, h)) return;
  int stride = (w + 1) / 2;
  int r0 = band.y0 - y; if (r0 < 0) r0 = 0;
  int r1 = band.y0 + band.h - y; if (r1 > h) r1 = h;
  for (int r = r0; r < r1; r++) {
    uint16_t* p = band.px + (y + r - band.y0) * SCR_W;
    const uint8_t* row = bmp + r * stride;
    for (int cx = 0; cx < w; cx++) {
      int sx = x + cx; if (sx < 0 || sx >= SCR_W) continue;
      uint8_t v = RD8(row + (cx >> 1)); v = (cx & 1) ? (v & 0x0F) : (v >> 4);
      if (v) p[sx] = blend(p[sx], c, v * 17);
    }
  }
}

static int textW(const Font& f, const char* s) {
  int w = 0;
  for (; *s; s++) {
    uint8_t ch = (uint8_t)*s; if (ch < f.first || ch > f.last) continue;
    w += f.g[ch - f.first].adv;
  }
  return w;
}
// x = left edge, y = baseline; returns the x after the text
static int text(const Font& f, const char* s, int x, int y, uint16_t c) {
  for (; *s; s++) {
    uint8_t ch = (uint8_t)*s; if (ch < f.first || ch > f.last) continue;
    const Glyph& g = f.g[ch - f.first];
    if (g.w) alpha4(f.bmp + g.off, g.w, g.h, x + g.xo, y + g.yo, c);
    x += g.adv;
  }
  return x;
}
static inline void textC(const Font& f, const char* s, int cx, int y, uint16_t c) { text(f, s, cx - textW(f, s) / 2, y, c); }
static inline void textR(const Font& f, const char* s, int rx, int y, uint16_t c) { text(f, s, rx - textW(f, s), y, c); }

// hue 0..255 -> RGB565 at value v (0..1), saturation s
static uint16_t hsv(uint8_t h, float s, float v) {
  float hh = h / 255.0f * 6.0f; int i = (int)hh; float f = hh - i;
  float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  float r, g, b;
  switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; }
  return rgb(r * 255, g * 255, b * 255);
}

// Platform hook: push the current band to the panel at row band.y0.
void panelPushBand();

// Repaint rows [y, y+h) by calling draw() once per band.
template <class F> static void paint(int y, int h, F draw) {
  for (int by = y; by < y + h; by += BAND_H) {
    band.y0 = by; band.h = (y + h - by) < BAND_H ? (y + h - by) : BAND_H;
    draw();
    panelPushBand();
  }
}
