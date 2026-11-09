// Beat Deck console page for the LilyGo T-Deck / T-Deck Plus (320x240 landscape).
// Shared by the firmware and tools/preview_tdeck.cpp (host preview): LovyanGFX / M5GFX calls only.
// The visualizer page is viz.h (same file as beat_visualizer/), drawn full screen.
#pragma once
#include <math.h>
#include <string.h>
#include "fonts_tdeck.h"
#include "viz.h"

constexpr int SW = 320, SH = 240;
enum Mode : uint8_t { M_FOLLOW, M_TAKE };
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE, LS_NORADIO };
enum Toast : uint8_t { T_NONE, T_TAKE, T_RELEASE, T_BPM_RANGE, T_FOLLOW_ONLY, T_ROLE_ON, T_ROLE_OFF, T_NO_LORA, T_PANEL };

struct View {
  uint8_t mode, link;
  bool bridgeOk, conflict, viaBooth, viaLora;
  bool role, loraOk;                          // role: this deck is the LoRa <-> ESP-NOW bridge
  float bpm; uint8_t beatPos, hue; uint32_t bars;
  int rssi; float snr;                        // last LoRa beat from the conductor (heard here or at a bridge)
  uint32_t rx, lost, sent, fail, relayed; int nudgeMs;
  uint32_t flashMs; bool flashDown;
  int battery;                                // percent, -1 = unknown
  const char* input;                          // BPM being typed ("" if none)
  uint8_t toast; bool help; uint8_t panelMode;
};

// ------------------------------------------------------------------ colours (same palette as the Tab5 / Cardputer)
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

lgfx::VLWfont fMono, fMonoL, fType, fTitle, fJack;
lgfx::PointerWrapper wMono, wMonoL, wType, wTitle, wJack;
void initFonts() {
  vizInitFonts();                             // vfBig (BPM), vfDec, vfLbl (header, pills)
  wMono.set(F_TMONO, sizeof(F_TMONO)); fMono.loadFont(&wMono);
  wMonoL.set(F_TMONOL, sizeof(F_TMONOL)); fMonoL.loadFont(&wMonoL);
  wType.set(F_TTYPE, sizeof(F_TTYPE)); fType.loadFont(&wType);
  wTitle.set(F_TTITLE, sizeof(F_TTITLE)); fTitle.loadFont(&wTitle);
  wJack.set(F_TJACK, sizeof(F_TJACK)); fJack.loadFont(&wJack);
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

void pill(LGFX_Sprite& s, const char* lbl, int xr, int y, uint16_t col, bool solid) {
  int pw = textW(s, &vfLbl, lbl) + 16;
  if (solid) { s.fillRoundRect(xr - pw, y, pw, 18, 9, col); text(s, &vfLbl, lbl, xr - pw / 2, y + 9, C_BG, middle_center); }
  else { s.drawRoundRect(xr - pw, y, pw, 18, 9, col); text(s, &vfLbl, lbl, xr - pw / 2, y + 9, col, middle_center); }
}

void draw(LGFX_Sprite& s, const View& v, uint32_t now) {
  bool active = v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE;
  int lvl = flashLevel(v, now);
  s.fillScreen(lvl && v.flashDown ? hueGlow(v.hue, lvl * 22 / 100) : C_BG);

  // header
  text(s, &vfLbl, "BEAT COMMANDER", 6, 13, C_AMB, middle_left);
  text(s, &fMono, "DECK", 8 + textW(s, &vfLbl, "BEAT COMMANDER") + 6, 13, C_AMB_D, middle_left);
  const char* lbl = "WAIT"; uint16_t col = C_AMB_D; bool solid = false;
  switch (v.link) {
    case LS_LIVE: lbl = v.viaBooth ? "BOOTH" : "LIVE"; col = C_GRN; solid = true; break;
    case LS_HOLD: lbl = "HOLD"; col = C_AMB; break;
    case LS_TAKE: lbl = v.conflict ? "CLASH" : "TAKE"; col = v.conflict ? C_RED : C_PURP; solid = true; break;
    case LS_NORADIO: lbl = "ERR"; col = C_RED; break;
  }
  pill(s, lbl, SW - 6, 4, col, solid);
  s.drawFastHLine(0, 27, SW, C_LINE);

  // BPM (or what's being typed)
  char ip[8], dp[8], b[24];
  uint16_t numCol = active ? (lvl > 170 ? 0xFFFF : C_TXT) : C_FAINT;
  if (v.input && v.input[0]) {
    snprintf(b, sizeof(b), "%s_", v.input);
    text(s, &fMono, "NEW BPM", 8, 42, C_PURP, middle_left);
    text(s, &fType, b, 6, 112, C_PURP, baseline_left);
  } else {
    if (active && v.bpm > 0) { int t = (int)lroundf(v.bpm * 10); snprintf(ip, sizeof(ip), "%d", t / 10); snprintf(dp, sizeof(dp), ".%d", t % 10); }
    else { strcpy(ip, "---"); dp[0] = 0; }
    int wi = textW(s, &vfBig, ip);
    text(s, &vfBig, ip, 4, 118, numCol, baseline_left);
    if (dp[0]) text(s, &vfDec, dp, 8 + wi, 118, numCol, baseline_left);
    if (active) { snprintf(b, sizeof(b), "BAR %lu", (unsigned long)v.bars); text(s, &fMono, b, 8, 42, C_AMB_D, middle_left); }
    text(s, &fMono, "BPM", 182, 42, C_FAINT, middle_right);
  }

  // stats column
  int y = 42;
  auto row = [&](const char* k, const char* val, uint16_t c) {
    text(s, &fMono, k, 198, y, C_AMB_D, middle_left); text(s, &fMono, val, SW - 6, y, c, middle_right); y += 17;
  };
  const char* src = v.link == LS_TAKE ? "DECK" : v.link == LS_HOLD ? "FLYWHEEL" : active ? (v.viaBooth ? "BOOTH" : "CONDUCTOR") : "-";
  row("SRC", src, v.link == LS_TAKE ? C_PURP : C_TXT);
  row("VIA", v.link == LS_TAKE ? "LORA" : active ? (v.viaLora ? "LORA" : "ESP-NOW") : "-", active ? C_TXT : C_FAINT);
  if (v.rssi) snprintf(b, sizeof(b), "%d", v.rssi); else strcpy(b, "-");
  row("RSSI", b, v.rssi == 0 ? C_FAINT : v.rssi > -90 ? C_GRN : v.rssi > -105 ? C_AMB : C_RED);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.lost); row("LOST", b, v.lost ? C_AMB : C_TXT);
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.sent); row("SENT", b, v.mode == M_TAKE ? (v.fail > 2 ? C_RED : C_GRN) : C_FAINT);
  snprintf(b, sizeof(b), "%+d", v.nudgeMs); row("NUDGE", b, v.mode == M_TAKE ? C_TXT : C_FAINT);
  row("LORA", !v.loraOk ? "ERR" : v.role ? "BRIDGE" : "LISTEN", !v.loraOk ? C_RED : v.role ? C_GRN : C_TXT);
  s.drawFastVLine(190, 32, 124, C_LINE);

  // beat bars
  for (int i = 0; i < 4; i++) {
    int x = 6 + i * 78, w = 74;
    bool on = active && v.beatPos == i;
    uint16_t c = on ? hueCol(v.hue, 120 + lvl * 135 / 255) : C_CARD;
    s.fillRoundRect(x, 164, w, 20, 6, c);
    if (i == 0 && !on) s.drawRoundRect(x, 164, w, 20, 6, active ? C_AMB_D : C_LINE);
  }

  // what's happening, battery
  const char* who = "";
  switch (v.link) {
    case LS_LIVE: who = v.viaBooth ? "following the booth" : "following the conductor"; break;
    case LS_HOLD: who = "link quiet, holding time"; break;
    case LS_TAKE: who = v.conflict ? "another conductor is on air!" : "you have the beat"; break;
    case LS_WAIT: who = v.role ? "bridge on, listening on LoRa" : v.bridgeOk ? "bridge OK, waiting for a beat" : "listening on LoRa + ESP-NOW"; break;
    case LS_NORADIO: who = "radios failed to start"; break;
  }
  text(s, &fMono, who, 6, 200, v.link == LS_TAKE ? (v.conflict ? C_RED : C_PURP) : C_AMB_D, middle_left);
  if (v.battery >= 0) { snprintf(b, sizeof(b), "%d%%", v.battery); text(s, &fMono, b, SW - 6, 200, v.battery > 20 ? C_AMB_D : C_RED, middle_right); }

  // key hints
  s.fillRect(0, 216, SW, 24, C_CARD);
  const char* hint = v.input && v.input[0] ? "ENTER take  DEL fix  X cancel"
                   : v.mode == M_TAKE ? "ball bpm/nudge   R free   H help"
                                      : "T take  SPACE tap  V viz  H help";
  text(s, &fMono, hint, SW / 2, 228, C_AMB_D, middle_center);

  // toast
  if (v.toast != T_NONE) {
    char m[24];
    switch (v.toast) {
      case T_TAKE: strcpy(m, "TAKEOVER"); break;
      case T_RELEASE: strcpy(m, "RELEASED"); break;
      case T_BPM_RANGE: strcpy(m, "60-200 BPM"); break;
      case T_FOLLOW_ONLY: strcpy(m, "TAKE OVER FIRST"); break;
      case T_ROLE_ON: strcpy(m, "BRIDGE ON"); break;
      case T_ROLE_OFF: strcpy(m, "BRIDGE OFF"); break;
      case T_NO_LORA: strcpy(m, "NO LORA RADIO"); break;
      default: snprintf(m, sizeof(m), "DISPLAY %d / 8", v.panelMode + 1); break;
    }
    uint16_t c = v.toast == T_TAKE ? C_PURP : (v.toast == T_RELEASE || v.toast == T_ROLE_ON) ? C_GRN : v.toast == T_NO_LORA ? C_RED : C_AMB;
    int w = textW(s, &fMonoL, m) + 32;
    s.fillRoundRect(SW / 2 - w / 2, 64, w, 34, 17, C_BG); s.drawRoundRect(SW / 2 - w / 2, 64, w, 34, 17, c);
    text(s, &fMonoL, m, SW / 2, 81, c, middle_center);
  }

  // help
  if (v.help) {
    s.fillRoundRect(6, 31, SW - 12, 180, 10, C_BG); s.drawRoundRect(6, 31, SW - 12, 180, 10, C_AMB);
    const char* lines[] = {"ball click / T  take or release", "SPACE           tap tempo",
                           "alt 128 ENTER   take at a BPM", "ball up / down  tempo -/+ 0.1",
                           "ball left/right nudge 10 ms", "V visuals  M mode  A auto",
                           "G bridge role   D display fix", "touch: swap page    H close"};
    for (int i = 0; i < 8; i++) text(s, &fMono, lines[i], 18, 46 + i * 20, i == 7 ? C_AMB_D : C_TXT, middle_left);
  }
}

// ------------------------------------------------------------------ splash
// t = ms since the splash started. Rings pulse out of the jackal at 120 BPM, the title fades in, the green line
// fills over SPLASH_MS and the start-up checks tick in underneath (state -1 = checking, 0 = failed, 1 = OK).
constexpr uint32_t SPLASH_MS = 4000;
static uint16_t lerp565(uint16_t a, uint16_t b, int k) {      // k 0..255
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  return ((ar + (br - ar) * k / 255) << 11) | ((ag + (bg - ag) * k / 255) << 5) | (ab + (bb - ab) * k / 255);
}
void drawSplash(LGFX_Sprite& s, uint32_t t, const char* const* names, const int8_t* state, int n, const char* ver) {
  s.fillScreen(0);
  const int cx = SW / 2, cy = 58;
  // rings: one per beat, expanding and fading
  for (int i = 0; i < 4; i++) {
    int age = (int)(t % 500) + i * 500;
    if (age > 2000 || (int)t < age) continue;
    int r = 30 + age * 160 / 2000, k = 255 - age * 255 / 2000;
    s.drawCircle(cx, cy, r, lerp565(0, C_AMB, k * 3 / 5));
    s.drawCircle(cx, cy, r + 1, lerp565(0, C_AMB, k / 4));
  }
  // jackal: fades in, then glows on every beat
  int in = t < 700 ? (int)t * 255 / 700 : 255;
  int beat = (int)(t % 500), glow = beat < 160 ? 255 - beat * 255 / 160 : 0;
  text(s, &fJack, "\xEE\x80\x80", cx + 4, cy + 46 - 2, lerp565(0, lerp565(C_AMB, 0xFFFF, glow / 3), in), baseline_center);
  // title
  int ti = t < 400 ? 0 : t < 1200 ? ((int)t - 400) * 255 / 800 : 255;
  text(s, &fTitle, "BEAT COMMANDER", cx, 140, lerp565(0, C_AMB, ti), middle_center);
  char b[32]; snprintf(b, sizeof(b), "BEAT DECK  v%s", ver);
  text(s, &fMono, b, cx, 165, lerp565(0, C_AMB_D, ti), middle_center);
  // progress line
  int w = (int)((t > SPLASH_MS ? SPLASH_MS : t) * 260 / SPLASH_MS);
  s.drawFastHLine(cx - 130, 182, 260, C_LINE);
  s.fillRect(cx - 130, 181, w, 3, C_GRN);
  // start-up checks, two columns
  for (int i = 0; i < n; i++) {
    int x = i % 2 ? cx + 10 : 12, y = 204 + (i / 2) * 20;
    text(s, &fMono, names[i], x, y, C_AMB_D, middle_left);
    const char* st = state[i] < 0 ? (t / 200 % 3 == 0 ? ".  " : t / 200 % 3 == 1 ? ".. " : "...") : state[i] ? "OK" : "--";
    text(s, &fMono, st, i % 2 ? SW - 12 : cx - 10, y, state[i] < 0 ? C_FAINT : state[i] ? C_GRN : C_RED, middle_right);
  }
}
