// T-Dongle beat screen (0.96" 160x80): BEAT and STATS pages, plus ten full-screen visuals from viz.h.
// Shared by tdongle_beat_gateway.ino (T-Dongle S3 Beat Gateway and T-Dongle C5 Beat Light) and tools/preview_dongle.cpp.
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fonts_dongle.h"
#include "viz.h"

constexpr int DW = 160, DH = 80;
enum DPage : uint8_t { P_BEAT, P_STATS, P_PULSE, P_TUNNEL, P_BARS, P_WARP, P_PLASMA, P_KALEIDO, P_GRID, P_PYRAMID, P_FIREWORKS, P_RAIN, P_COUNT };
static const uint8_t PAGE_VIZ[P_COUNT] = {0, 0, V_PULSE, V_TUNNEL, V_BARS, V_STARS, V_PLASMA, V_KALEIDO, V_GRID, V_PYRAMID, V_FIREWORKS, V_RAIN};
enum DState : uint8_t { D_WAIT, D_LOCK, D_FREE };
enum DSrc : uint8_t { DS_NONE, DS_AIR, DS_BOOTH };

struct DView {
  bool gateway;               // S3 Beat Gateway (MIDI) or C5 Beat Light (feed only)
  uint8_t page, st, src;
  uint16_t bpm10; uint8_t beatPos, hue;
  uint32_t beatMs, beatId, bars;
  bool clockRun;              // MIDI clock running (gateway)
  int rssi;
  uint32_t beats, frames;
  uint8_t bridge;             // 0 not heard, 1 OK, 2 bridge up but its LoRa radio failed
  bool autoMode; uint32_t labelUntil;
  const char* hint;
};

static inline uint16_t d565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t D_BG = d565(5, 5, 5), D_CARD = d565(16, 13, 7), D_LINE = d565(48, 38, 16), D_AMB = d565(255, 176, 0),
               D_AMB_D = d565(130, 104, 44), D_GRN = d565(57, 255, 20), D_RED = d565(255, 59, 59), D_TXT = d565(255, 214, 130),
               D_PURP = d565(170, 110, 255), D_FAINT = d565(70, 58, 30);

lgfx::VLWfont dfBpm, dfDec, dfMono;
lgfx::PointerWrapper dwBpm, dwDec, dwMono;
void dongleInitFonts() {
  vizInitFonts();
  dwBpm.set(F_DBPM, sizeof(F_DBPM)); dfBpm.loadFont(&dwBpm);
  dwDec.set(F_DDEC, sizeof(F_DDEC)); dfDec.loadFont(&dwDec);
  dwMono.set(F_DMONO, sizeof(F_DMONO)); dfMono.loadFont(&dwMono);
}
static void dtext(LGFX_Sprite& s, const lgfx::IFont* f, const char* str, int x, int y, uint16_t c, textdatum_t d) {
  s.setFont(f); s.setTextDatum(d); s.setTextColor(c); s.drawString(str, x, y);
}
static uint16_t dhue(uint8_t h, int lvl) { int r, g, b; vhue(h, r, g, b); return d565(r * lvl / 255, g * lvl / 255, b * lvl / 255); }

static void drawBeatPage(LGFX_Sprite& s, const DView& v, uint32_t now) {
  bool active = v.st != D_WAIT;
  uint32_t dt = now - v.beatMs;
  int lvl = active && dt < 380 ? 255 - (int)dt * 255 / 380 : 0;
  if (lvl && v.beatPos == 0) { int r, g, b; vhue(v.hue, r, g, b); int k = lvl * 22 / 100; s.fillScreen(d565(5 + (r - 5) * k / 255, 5 + (g - 5) * k / 255, 5 + (b - 5) * k / 255)); }
  else s.fillScreen(D_BG);
  // header: name, status pill
  const char* lbl = v.st == D_WAIT ? "WAIT" : v.st == D_FREE ? "HOLD" : v.src == DS_BOOTH ? "BOOTH" : "LIVE";
  uint16_t col = v.st == D_WAIT ? D_AMB_D : v.st == D_FREE ? D_AMB : D_GRN;
  s.setFont(&vfLbl); int pw = s.textWidth(lbl) + 10;
  const char* title = v.gateway ? "GATEWAY" : "BEAT LIGHT";
  bool fits = s.textWidth(title) + 6 < DW - pw - 1;
  dtext(s, fits ? (const lgfx::IFont*)&vfLbl : (const lgfx::IFont*)&dfMono, title, 3, 8, D_AMB, middle_left);
  if (v.st == D_LOCK) { s.fillRoundRect(DW - pw - 1, 0, pw, 15, 7, col); dtext(s, &vfLbl, lbl, DW - 1 - pw / 2, 8, D_BG, middle_center); }
  else { s.drawRoundRect(DW - pw - 1, 0, pw, 15, 7, col); dtext(s, &vfLbl, lbl, DW - 1 - pw / 2, 8, col, middle_center); }
  // BPM
  char ip[8], dp[6];
  if (active && v.bpm10) { snprintf(ip, sizeof(ip), "%d", v.bpm10 / 10); snprintf(dp, sizeof(dp), ".%d", v.bpm10 % 10); }
  else { strcpy(ip, "---"); dp[0] = 0; }
  uint16_t nc = active ? (lvl > 170 ? 0xFFFF : D_TXT) : D_FAINT;
  s.setFont(&dfBpm); int wi = s.textWidth(ip);
  dtext(s, &dfBpm, ip, 3, 57, nc, baseline_left);
  if (dp[0]) dtext(s, &dfDec, dp, 5 + wi, 57, nc, baseline_left);
  // right column: MIDI / feed and source
  char b[16];
  if (v.gateway) dtext(s, &dfMono, v.clockRun ? "MIDI >" : "MIDI .", DW - 3, 27, v.clockRun ? D_GRN : D_FAINT, middle_right);
  else dtext(s, &dfMono, "FEED", DW - 3, 27, D_AMB_D, middle_right);
  if (active) { snprintf(b, sizeof(b), "BAR %lu", (unsigned long)(v.bars % 1000)); dtext(s, &dfMono, b, DW - 3, 43, D_AMB_D, middle_right); }
  // beat boxes
  for (int i = 0; i < 4; i++) {
    int x = 2 + i * 40, w = 36;
    bool on = active && v.beatPos == i;
    s.fillRoundRect(x, 66, w, 12, 4, on ? dhue(v.hue, 120 + lvl * 135 / 255) : D_CARD);
    if (i == 0 && !on) s.drawRoundRect(x, 66, w, 12, 4, active ? D_AMB_D : D_LINE);
  }
}

static void drawStatsPage(LGFX_Sprite& s, const DView& v, uint32_t now) {
  s.fillScreen(D_BG);
  char b[20]; int y = 7;
  auto row = [&](const char* k, const char* val, uint16_t c) {
    dtext(s, &dfMono, k, 3, y, D_AMB_D, middle_left); dtext(s, &dfMono, val, DW - 3, y, c, middle_right); y += 16;
  };
  row("SOURCE", v.src == DS_AIR ? "CONDUCTOR" : v.src == DS_BOOTH ? "BOOTH" : "-", v.src ? D_TXT : D_FAINT);
  if (v.rssi) snprintf(b, sizeof(b), "%d dBm", v.rssi); else strcpy(b, "-");
  row("LORA RSSI", b, v.rssi == 0 ? D_FAINT : v.rssi > -90 ? D_GRN : v.rssi > -105 ? D_AMB : D_RED);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.beats); row("BEATS", b, D_TXT);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.frames); row("LINK FRAMES", b, D_TXT);
  row("BRIDGE", v.bridge == 1 ? "OK" : v.bridge == 2 ? "NO LORA" : "-", v.bridge == 1 ? D_GRN : v.bridge == 2 ? D_RED : D_FAINT);
}

void dongleDraw(LGFX_Sprite& s, const DView& v, uint32_t now) {
  if (v.page == P_BEAT) drawBeatPage(s, v, now);
  else if (v.page == P_STATS) drawStatsPage(s, v, now);
  else {
    VizState z = {};
    z.bpm = v.bpm10 ? v.bpm10 / 10.0f : 120; z.beatMs = v.beatMs; z.beatId = v.beatId; z.beatPos = v.beatPos; z.hue = v.hue;
    z.bars = v.bars; z.src = v.st == D_WAIT ? SRC_DEMO : v.src == DS_BOOTH ? SRC_BOOTH : SRC_W10;
    z.mode = PAGE_VIZ[v.page]; z.autoMode = v.autoMode; z.labelUntil = v.labelUntil; z.hud = true; z.hint = v.hint;
    vizDraw(s, z, now);
    return;
  }
  if (v.hint || (int32_t)(v.labelUntil - now) > 0) {        // page name / gesture hint on the info pages
    const char* m = v.hint ? v.hint : v.page == P_BEAT ? "BEAT" : "STATS";
    s.setFont(&vfLbl); int w = s.textWidth(m) + 16;
    s.fillRoundRect(DW / 2 - w / 2, 28, w, 22, 11, D_BG); s.drawRoundRect(DW / 2 - w / 2, 28, w, 22, 11, D_AMB);
    dtext(s, &vfLbl, m, DW / 2, 39, D_AMB, middle_center);
  }
}
