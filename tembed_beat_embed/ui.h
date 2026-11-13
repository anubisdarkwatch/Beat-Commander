// Beat Embed screens for the LilyGo T-Embed CC1101 (1.9" 320x170 landscape): the BEAT page (follow / take /
// conduct) and the 11 full-screen visuals from viz.h. Shared with tools/preview_embed.cpp (host preview).
#pragma once
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "fonts_dongle.h"
#include "viz.h"

constexpr int SW = 320, SH = 170;
enum Mode : uint8_t { M_FOLLOW, M_TAKE, M_CONDUCT };
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE, LS_NORADIO };
enum Toast : uint8_t { T_NONE, T_TAKE, T_RELEASE, T_CONDUCT, T_FOLLOW, T_ONAIR, T_LOCAL, T_AUTO_ON, T_AUTO_OFF, T_PANEL, T_GAIN };

struct View {
  uint8_t mode, link;
  bool viaBooth, conflict, bridgeOk, onAir, micOk;
  float bpm; uint8_t beatPos, hue; uint32_t bars;
  int rssi; uint32_t rx, lost, sent;
  int nudgeMs;
  uint32_t flashMs; bool flashDown;
  int battery;                         // %, -1 unknown
  float level, conf; int gain;         // conduct: mic level, tempo confidence, gain step
  const float* scope; const bool* scopeOn; int scopePos, scopeN;
  uint8_t toast; int toastVal;
  const char* hint;                    // what letting go of a held button will do
};

static inline uint16_t e565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
const uint16_t E_BG = e565(5, 5, 5), E_CARD = e565(16, 13, 7), E_LINE = e565(48, 38, 16), E_AMB = e565(255, 176, 0),
               E_AMB_D = e565(130, 104, 44), E_GRN = e565(57, 255, 20), E_RED = e565(255, 59, 59), E_TXT = e565(255, 214, 130),
               E_PURP = e565(170, 110, 255), E_FAINT = e565(70, 58, 30);
static uint16_t ehue(uint8_t h, int lvl) { int r, g, b; vhue(h, r, g, b); return e565(r * lvl / 255, g * lvl / 255, b * lvl / 255); }

lgfx::VLWfont efMono; lgfx::PointerWrapper ewMono;
void embedInitFonts() { vizInitFonts(); ewMono.set(F_DMONO, sizeof(F_DMONO)); efMono.loadFont(&ewMono); }
static void etext(LGFX_Sprite& s, const lgfx::IFont* f, const char* str, int x, int y, uint16_t c, textdatum_t d) {
  s.setFont(f); s.setTextDatum(d); s.setTextColor(c); s.drawString(str, x, y);
}

void drawBeatPage(LGFX_Sprite& s, const View& v, uint32_t now) {
  bool conduct = v.mode == M_CONDUCT;
  bool active = conduct ? v.micOk : (v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE);
  uint32_t dt = now - v.flashMs;
  int lvl = active && dt < 380 ? 255 - (int)dt * 255 / 380 : 0;
  if (lvl && v.flashDown) { int r, g, b; vhue(v.hue, r, g, b); int k = lvl * 22 / 100; s.fillScreen(e565(5 + (r - 5) * k / 255, 5 + (g - 5) * k / 255, 5 + (b - 5) * k / 255)); }
  else s.fillScreen(E_BG);

  // header
  etext(s, &vfLbl, "BEAT EMBED", 6, 11, E_AMB, middle_left);
  s.setFont(&vfLbl); int tw = s.textWidth("BEAT EMBED");
  etext(s, &efMono, conduct ? "CONDUCT" : v.mode == M_TAKE ? "COMMAND" : "FOLLOW", 6 + tw + 10, 11, conduct ? E_GRN : v.mode == M_TAKE ? E_PURP : E_AMB_D, middle_left);
  const char* lbl; uint16_t col; bool solid;
  if (conduct) { lbl = !v.micOk ? "NO MIC" : v.onAir ? (v.conflict ? "CLASH" : "ON AIR") : "LOCAL"; col = !v.micOk || v.conflict ? E_RED : v.onAir ? E_RED : E_AMB_D; solid = v.onAir && v.micOk; }
  else switch (v.link) {
    case LS_LIVE: lbl = v.viaBooth ? "BOOTH" : "LIVE"; col = E_GRN; solid = true; break;
    case LS_HOLD: lbl = "HOLD"; col = E_AMB; solid = false; break;
    case LS_TAKE: lbl = v.conflict ? "CLASH" : "TAKE"; col = v.conflict ? E_RED : E_PURP; solid = true; break;
    case LS_NORADIO: lbl = "ERR"; col = E_RED; solid = false; break;
    default: lbl = "WAIT"; col = E_AMB_D; solid = false;
  }
  s.setFont(&vfLbl); int pw = s.textWidth(lbl) + 16;
  if (solid) { s.fillRoundRect(SW - pw - 4, 2, pw, 18, 9, col); etext(s, &vfLbl, lbl, SW - 4 - pw / 2, 11, E_BG, middle_center); }
  else { s.drawRoundRect(SW - pw - 4, 2, pw, 18, 9, col); etext(s, &vfLbl, lbl, SW - 4 - pw / 2, 11, col, middle_center); }
  s.drawFastHLine(0, 23, SW, E_LINE);

  // BPM
  char ip[8], dp[6], b[24];
  if (active && v.bpm > 0) { int t = (int)lroundf(v.bpm * 10); snprintf(ip, sizeof(ip), "%d", t / 10); snprintf(dp, sizeof(dp), ".%d", t % 10); }
  else { strcpy(ip, "---"); dp[0] = 0; }
  uint16_t nc = active ? (lvl > 170 ? 0xFFFF : E_TXT) : E_FAINT;
  s.setFont(&vfBig); int wi = s.textWidth(ip);
  etext(s, &vfBig, ip, 4, 104, nc, baseline_left);
  if (dp[0]) etext(s, &vfDec, dp, 8 + wi, 104, nc, baseline_left);

  // right column
  int y = 34;
  auto row = [&](const char* k, const char* val, uint16_t c) {
    etext(s, &efMono, k, 210, y, E_AMB_D, middle_left); etext(s, &efMono, val, SW - 6, y, c, middle_right); y += 16;
  };
  if (conduct) {
    static const char* const G[4] = {"CLUB", "LOUD", "QUIET", "MAX"};
    row("MIC", v.micOk ? G[v.gain & 3] : "FAILED", v.micOk ? E_TXT : E_RED);
    snprintf(b, sizeof(b), "%d%%", (int)(v.conf * 100)); row("LOCK", b, v.conf > 0.2f ? E_GRN : v.conf > 0.08f ? E_AMB : E_FAINT);
    snprintf(b, sizeof(b), "%lu", (unsigned long)v.sent); row("SENT", b, v.onAir ? E_GRN : E_FAINT);
    row("BRIDGE", v.bridgeOk ? "OK" : "-", v.bridgeOk ? E_GRN : E_FAINT);
  } else {
    row("SRC", v.link == LS_TAKE ? "EMBED" : v.link == LS_HOLD ? "FLYWHEEL" : v.link == LS_LIVE ? (v.viaBooth ? "BOOTH" : "CONDUCTOR") : "-", E_TXT);
    if (v.rssi) snprintf(b, sizeof(b), "%d", v.rssi); else strcpy(b, "-");
    row("RSSI", b, v.rssi ? E_TXT : E_FAINT);
    if (v.mode == M_TAKE) { snprintf(b, sizeof(b), "%lu", (unsigned long)v.sent); row("SENT", b, E_GRN); }
    else { snprintf(b, sizeof(b), "%lu", (unsigned long)v.lost); row("LOST", b, v.lost ? E_AMB : E_TXT); }
    row("BRIDGE", v.bridgeOk ? "OK" : "-", v.bridgeOk ? E_GRN : E_FAINT);
  }
  if (v.battery >= 0) { snprintf(b, sizeof(b), "%d%%", v.battery); row("BAT", b, v.battery > 20 ? E_TXT : E_RED); }
  s.drawFastVLine(202, 28, 86, E_LINE);

  // conduct: onset scope; follow: nothing in that band
  if (conduct && v.scope) {
    for (int i = 0; i < v.scopeN; i++) {
      int idx = (v.scopePos + i) % v.scopeN, x = 4 + i * (SW - 8) / v.scopeN;
      int h = (int)(v.scope[idx] * 18);
      if (h) s.drawFastVLine(x, 136 - h, h, v.scopeOn[idx] ? E_GRN : E_AMB_D);
    }
    s.drawFastHLine(4, 137, SW - 8, E_LINE);
  }
  // beat boxes
  for (int i = 0; i < 4; i++) {
    int x = 4 + i * 79, w = 75;
    bool on = active && v.beatPos == i;
    s.fillRoundRect(x, 140, w, 12, 5, on ? ehue(v.hue, 120 + lvl * 135 / 255) : E_CARD);
    if (!on) s.drawRoundRect(x, 140, w, 12, 5, i == 0 && active ? E_AMB_D : E_LINE);   // outlines: the panel crushes the dark fill
  }
  // controls
  const char* h = conduct ? "PRESS on air  KEY tap  TURN page" : v.mode == M_TAKE ? "TURN tempo  PRESS free  KEY tap" : "PRESS take  KEY tap  TURN page";
  etext(s, &efMono, h, SW / 2, 162, E_AMB_D, middle_center);
}

void drawToast(LGFX_Sprite& s, const View& v) {
  const char* m = v.hint;
  char b[24];
  if (!m && v.toast != T_NONE) {
    static const char* const G[4] = {"CLUB", "LOUD ROOM", "QUIET ROOM", "MAX"};
    switch (v.toast) {
      case T_TAKE: m = "TAKEOVER"; break;          case T_RELEASE: m = "RELEASED"; break;
      case T_CONDUCT: m = "CONDUCT: MIC"; break;   case T_FOLLOW: m = "FOLLOW"; break;
      case T_ONAIR: m = "ON AIR"; break;           case T_LOCAL: m = "LOCAL"; break;
      case T_AUTO_ON: m = "AUTO ON"; break;        case T_AUTO_OFF: m = "AUTO OFF"; break;
      case T_GAIN: snprintf(b, sizeof(b), "GAIN %s", G[v.toastVal & 3]); m = b; break;
      default: snprintf(b, sizeof(b), "DISPLAY %d / 8", v.toastVal + 1); m = b; break;
    }
  }
  if (!m) return;
  s.setFont(&vfLbl); int w = s.textWidth(m) + 28;
  s.fillRoundRect(SW / 2 - w / 2, 62, w, 30, 15, E_BG); s.drawRoundRect(SW / 2 - w / 2, 62, w, 30, 15, E_AMB);
  etext(s, &vfLbl, m, SW / 2, 77, E_AMB, middle_center);
}
