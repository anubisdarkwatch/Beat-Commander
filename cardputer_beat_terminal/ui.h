// Beat Commander terminal screens for the M5Stack Cardputer / Cardputer ADV (240x135 landscape).
// Shared by the firmware and tools/preview_cardputer.cpp (host preview): LovyanGFX / M5GFX calls only.
#pragma once
#include <math.h>
#include "fonts_cardputer.h"

constexpr int SW = 240, SH = 135;
enum Mode : uint8_t { M_FOLLOW, M_TAKE };
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE, LS_NORADIO };
enum Toast : uint8_t { T_NONE, T_TAKE, T_RELEASE, T_BPM_RANGE, T_FOLLOW_ONLY };

struct View {
  uint8_t mode, link;
  bool bridgeOk, conflict, viaBooth;
  float bpm; uint8_t beatPos, hue; uint32_t bars;
  int rssi; uint32_t rx, lost, sent, fail; int nudgeMs;
  uint32_t flashMs; bool flashDown;
  int battery;
  const char* input;                         // BPM being typed ("" if none)
  uint8_t toast; bool help; uint8_t tapCount;
};

// ------------------------------------------------------------------ colours (same palette as the Tab5)
static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
void hue8(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6, q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}
uint16_t hueCol(uint8_t h, int lvl) { uint8_t r, g, b; hue8(h, r, g, b); return c565(r * lvl / 255, g * lvl / 255, b * lvl / 255); }
uint16_t hueGlow(uint8_t h, int lvl) { uint8_t r, g, b; hue8(h, r, g, b); return c565(5 + (r - 5) * lvl / 255, 5 + (g - 5) * lvl / 255, 5 + (b - 5) * lvl / 255); }
const uint16_t C_BG = c565(5, 5, 5), C_CARD = c565(14, 11, 6), C_LINE = c565(48, 38, 16), C_AMB = c565(255, 176, 0),
               C_AMB_D = c565(130, 104, 44), C_GRN = c565(57, 255, 20), C_RED = c565(255, 59, 59), C_TXT = c565(255, 214, 130),
               C_PURP = c565(170, 110, 255), C_FAINT = c565(70, 58, 30);

lgfx::VLWfont fBpm, fDec, fTitle, fType, fPill, fMono, fMonoL;
lgfx::PointerWrapper wBpm, wDec, wTitle, wType, wPill, wMono, wMonoL;
void initFonts() {
  struct { lgfx::VLWfont* f; lgfx::PointerWrapper* w; const uint8_t* d; size_t n; } t[] = {
    {&fBpm, &wBpm, F_BPM, sizeof(F_BPM)}, {&fDec, &wDec, F_DEC, sizeof(F_DEC)}, {&fTitle, &wTitle, F_TITLE, sizeof(F_TITLE)}, {&fType, &wType, F_TYPE, sizeof(F_TYPE)},
    {&fPill, &wPill, F_PILL, sizeof(F_PILL)}, {&fMono, &wMono, F_MONO, sizeof(F_MONO)}, {&fMonoL, &wMonoL, F_MONOL, sizeof(F_MONOL)}};
  for (auto& x : t) { x.w->set(x.d, x.n); x.f->loadFont(x.w); }
}

void text(LGFX_Sprite& s, const lgfx::IFont* f, const char* str, int x, int y, uint16_t col, textdatum_t d = top_left) {
  s.setFont(f); s.setTextDatum(d); s.setTextColor(col); s.drawString(str, x, y);
}
int textW(LGFX_Sprite& s, const lgfx::IFont* f, const char* str) { s.setFont(f); return s.textWidth(str); }

int flashLevel(const View& v, uint32_t now) {
  uint32_t dt = now - v.flashMs;
  if (v.link == LS_WAIT || v.link == LS_NORADIO || dt > 380) return 0;
  return 255 - (int)dt * 255 / 380;
}

void draw(LGFX_Sprite& s, const View& v, uint32_t now) {
  bool active = v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE;
  int lvl = flashLevel(v, now);
  s.fillScreen(lvl && v.flashDown ? hueGlow(v.hue, lvl * 22 / 100) : C_BG);

  // header
  text(s, &fTitle, "\xEE\x80\x80", 4, 15, C_AMB, baseline_left);
  text(s, &fTitle, "BEAT COMMANDER", 24, 14, C_AMB, baseline_left);
  const char* lbl = "WAIT"; uint16_t col = C_AMB_D; bool solid = false;
  switch (v.link) {
    case LS_LIVE: lbl = v.viaBooth ? "BOOTH" : "LIVE"; col = C_GRN; solid = true; break;
    case LS_HOLD: lbl = "HOLD"; col = C_AMB; break;
    case LS_TAKE: lbl = v.conflict ? "CLASH" : "TAKE"; col = v.conflict ? C_RED : C_PURP; solid = true; break;
    case LS_NORADIO: lbl = "ERR"; col = C_RED; break;
  }
  int pw = textW(s, &fPill, lbl) + 14;
  if (solid) { s.fillRoundRect(SW - 4 - pw, 2, pw, 15, 7, col); text(s, &fPill, lbl, SW - 4 - pw / 2, 10, C_BG, middle_center); }
  else { s.drawRoundRect(SW - 4 - pw, 2, pw, 15, 7, col); text(s, &fPill, lbl, SW - 4 - pw / 2, 10, col, middle_center); }
  s.drawFastHLine(0, 20, SW, C_LINE);

  // BPM (or what's being typed)
  char ip[8], dp[8];
  uint16_t numCol = active ? (lvl > 170 ? 0xFFFF : C_TXT) : C_FAINT;
  if (v.input && v.input[0]) {
    char b[16]; snprintf(b, sizeof(b), "%s_", v.input);
    text(s, &fMono, "NEW BPM", 6, 34, C_PURP, middle_left);
    text(s, &fType, b, 4, 76, C_PURP, baseline_left);
  } else {
    if (active && v.bpm > 0) { int t = (int)lroundf(v.bpm * 10); snprintf(ip, sizeof(ip), "%d", t / 10); snprintf(dp, sizeof(dp), ".%d", t % 10); }
    else { strcpy(ip, "---"); dp[0] = 0; }
    int wi = textW(s, &fBpm, ip);
    text(s, &fBpm, ip, 4, 80, numCol, baseline_left);
    if (dp[0]) text(s, &fDec, dp, 6 + wi, 80, numCol, baseline_left);
    if (active) { char b[16]; snprintf(b, sizeof(b), "BAR %lu", (unsigned long)v.bars); text(s, &fMono, b, 6, 32, C_AMB_D, middle_left); }
  }

  // stats column
  char b[20]; int y = 32;
  auto row = [&](const char* k, const char* val, uint16_t c) {
    text(s, &fMono, k, 164, y, C_AMB_D, middle_left); text(s, &fMono, val, 236, y, c, middle_right); y += 14;
  };
  row("SRC", v.link == LS_TAKE ? "CARD" : active ? (v.viaBooth ? "BOOTH" : "W10") : "-", v.link == LS_TAKE ? C_PURP : C_TXT);
  if (v.rssi) snprintf(b, sizeof(b), "%d", v.rssi); else strcpy(b, "-");
  row("RSSI", b, v.rssi == 0 ? C_FAINT : v.rssi > -90 ? C_GRN : v.rssi > -105 ? C_AMB : C_RED);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.lost); row("LOST", b, v.lost ? C_AMB : C_TXT);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.sent); row("SENT", b, v.mode == M_TAKE ? (v.fail > 2 ? C_RED : C_GRN) : C_FAINT);
  snprintf(b, sizeof(b), "%+d", v.nudgeMs); row("NUDGE", b, v.mode == M_TAKE ? C_TXT : C_FAINT);
  s.drawFastVLine(158, 24, 70, C_LINE);

  // beat bars
  for (int i = 0; i < 4; i++) {
    int x = 4 + i * 59, w = 54;
    bool on = active && v.beatPos == i;
    uint16_t c = on ? hueCol(v.hue, 120 + lvl * 135 / 255) : C_CARD;
    s.fillRoundRect(x, 92, w, 12, 4, c);
    if (i == 0 && !on) s.drawRoundRect(x, 92, w, 12, 4, active ? C_AMB_D : C_LINE);
  }

  // what's happening + key hints
  const char* who = "";
  switch (v.link) {
    case LS_LIVE: who = v.viaBooth ? "following the booth" : "following the W10"; break;
    case LS_HOLD: who = "link quiet, holding time"; break;
    case LS_TAKE: who = v.conflict ? "another conductor is on air!" : "you have the beat"; break;
    case LS_WAIT: who = v.bridgeOk ? "bridge OK, waiting for a beat" : "waiting for the bridge"; break;
    case LS_NORADIO: who = "ESP-NOW failed to start"; break;
  }
  text(s, &fMono, who, 4, 114, v.link == LS_TAKE ? (v.conflict ? C_RED : C_PURP) : C_AMB_D, middle_left);
  if (v.battery >= 0) { snprintf(b, sizeof(b), "%d%%", v.battery); text(s, &fMono, b, 236, 114, v.battery > 20 ? C_AMB_D : C_RED, middle_right); }
  s.fillRect(0, 122, SW, 13, C_CARD);
  const char* hint = v.input && v.input[0] ? "ENTER take   DEL fix   ` cancel"
                   : v.mode == M_TAKE ? "-= bpm  [] nudge  R free  H"
                                      : "T take  SPACE tap  H help";
  text(s, &fMono, hint, SW / 2, 129, C_AMB_D, middle_center);

  // toast
  if (v.toast != T_NONE) {
    const char* m = v.toast == T_TAKE ? "TAKEOVER" : v.toast == T_RELEASE ? "RELEASED" : v.toast == T_BPM_RANGE ? "60-200 BPM" : "TAKE OVER FIRST";
    uint16_t c = v.toast == T_TAKE ? C_PURP : v.toast == T_RELEASE ? C_GRN : C_AMB;
    int w = textW(s, &fMonoL, m) + 24;
    s.fillRoundRect(SW / 2 - w / 2, 46, w, 26, 13, C_BG); s.drawRoundRect(SW / 2 - w / 2, 46, w, 26, 13, c);
    text(s, &fMonoL, m, SW / 2, 59, c, middle_center);
  }

  // help
  if (v.help) {
    s.fillRoundRect(6, 24, SW - 12, 96, 8, C_BG); s.drawRoundRect(6, 24, SW - 12, 96, 8, C_AMB);
    const char* lines[] = {"T / G0     take or release", "SPACE      tap tempo", "128 ENTER  take at a BPM",
                           "- =        tempo -/+ 0.1", "fn - =     tempo -/+ 1", "[ ]        nudge 10 ms",
                           "H          close"};
    for (int i = 0; i < 7; i++) text(s, &fMono, lines[i], 14, 34 + i * 13, i == 6 ? C_AMB_D : C_TXT, middle_left);
  }
}
