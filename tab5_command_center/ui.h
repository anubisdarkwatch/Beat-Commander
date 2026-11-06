// Beat Command Center screens for the M5Stack Tab5 (1280x720 landscape).
// The screen is split into four panels, each drawn into its own sprite and pushed only when it changes.
// Shared by the firmware and tools/preview_tab5.cpp (host preview), so it uses only LovyanGFX / M5GFX calls.
#pragma once
#include <math.h>
#include "fonts_tab5.h"

constexpr int SW = 1280, SH = 720;
enum Panel : uint8_t { P_HEAD, P_LEFT, P_CENTER, P_RIGHT, P_COUNT };
struct Rect { int16_t x, y, w, h; bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; } };
const Rect PANEL[P_COUNT] = {{0, 0, 1280, 76}, {0, 76, 336, 644}, {336, 76, 608, 644}, {944, 76, 336, 644}};

enum Mode : uint8_t { M_FOLLOW, M_TAKE };
enum LinkState : uint8_t { LS_NOGW, LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE };
enum Pad : uint8_t { PAD_TAKE, PAD_TAP, PAD_MINUS, PAD_PLUS, PAD_EARLY, PAD_LATE, PAD_COUNT, PAD_NONE = 255 };
// pad rectangles in screen coordinates (the left panel starts at y 76)
const Rect PAD[PAD_COUNT] = {
  {24, 96, 288, 120}, {24, 232, 288, 164}, {24, 432, 136, 104}, {176, 432, 136, 104}, {24, 584, 136, 104}, {176, 584, 136, 104}};
enum Toast : uint8_t { T_NONE, T_TAKE, T_RELEASE, T_FOLLOW_ONLY };

constexpr int HIST_N = 120;
constexpr int DBG_N = 16;                    // debug view: last raw lines from the gateway                  // BPM history: one sample a second, 2 minutes

struct View {
  uint8_t mode, link;
  bool gwOk, bridgeOk, radioOk, conflict;
  const char* gwName;
  uint32_t framesGot, framesSent;
  bool debug; const char (*dbg)[40]; uint32_t dbgCount;            // link frames received over USB / heard by the gateway's radio
  float bpm; uint8_t beatPos, hue; uint32_t bars;
  int rssi; float snr; uint32_t rx, lost, sent, fail; int nudgeMs; bool viaBooth;
  uint32_t flashMs; bool flashDown;
  int battery; bool charging; uint8_t tapCount;
  const float* hist; int histPos;            // ring buffer of HIST_N samples, 0 = no beat that second
  uint8_t pressed;                           // pad held down right now (PAD_NONE if none)
  uint8_t toast;
  const char* note;                          // shown under the ring, e.g. why the Tab5 last restarted
};

// ------------------------------------------------------------------ colours
static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
void hue8(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6, q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}
uint16_t hueCol(uint8_t h, int lvl) { uint8_t r, g, b; hue8(h, r, g, b); return c565(r * lvl / 255, g * lvl / 255, b * lvl / 255); }
// blend from the background colour towards a hue (lvl 0..255)
uint16_t hueGlow(uint8_t h, int lvl) { uint8_t r, g, b; hue8(h, r, g, b); return c565(5 + (r - 5) * lvl / 255, 5 + (g - 5) * lvl / 255, 5 + (b - 5) * lvl / 255); }

const uint16_t C_BG = c565(5, 5, 5), C_CARD = c565(12, 10, 6), C_LINE = c565(48, 38, 16), C_AMB = c565(255, 176, 0),
               C_AMB_D = c565(120, 96, 40), C_GRN = c565(57, 255, 20), C_RED = c565(255, 59, 59), C_TXT = c565(255, 214, 130),
               C_PURP = c565(170, 110, 255), C_FAINT = c565(70, 58, 30);

lgfx::VLWfont fBpm, fDec, fTitle, fPad, fH2, fMono, fMonoL, fMsg;
lgfx::PointerWrapper wBpm, wDec, wTitle, wPad, wH2, wMono, wMonoL, wMsg;
void initFonts() {
  struct { lgfx::VLWfont* f; lgfx::PointerWrapper* w; const uint8_t* d; size_t n; } t[] = {
    {&fBpm, &wBpm, F_BPM, sizeof(F_BPM)}, {&fDec, &wDec, F_DEC, sizeof(F_DEC)}, {&fTitle, &wTitle, F_TITLE, sizeof(F_TITLE)},
    {&fPad, &wPad, F_PAD, sizeof(F_PAD)}, {&fH2, &wH2, F_H2, sizeof(F_H2)}, {&fMono, &wMono, F_MONO, sizeof(F_MONO)},
    {&fMonoL, &wMonoL, F_MONOL, sizeof(F_MONOL)}, {&fMsg, &wMsg, F_MSG, sizeof(F_MSG)}};
  for (auto& x : t) { x.w->set(x.d, x.n); x.f->loadFont(x.w); }
}

// ------------------------------------------------------------------ helpers
void text(LGFX_Sprite& s, const lgfx::IFont* f, const char* str, int x, int y, uint16_t col, textdatum_t d = top_left) {
  s.setFont(f); s.setTextDatum(d); s.setTextColor(col); s.drawString(str, x, y);
}
int textW(LGFX_Sprite& s, const lgfx::IFont* f, const char* str) { s.setFont(f); return s.textWidth(str); }

void pill(LGFX_Sprite& s, int cx, int cy, const char* label, uint16_t col, bool solid) {
  int w = textW(s, &fPad, label) + 48, h = 48;
  if (solid) { s.fillRoundRect(cx - w / 2, cy - h / 2, w, h, h / 2, col); text(s, &fPad, label, cx, cy + 2, C_BG, middle_center); }
  else { for (int i = 0; i < 3; i++) s.drawRoundRect(cx - w / 2 + i, cy - h / 2 + i, w - 2 * i, h - 2 * i, h / 2 - i, col);
         text(s, &fPad, label, cx, cy + 2, col, middle_center); }
}

void dot(LGFX_Sprite& s, int x, int y, bool ok) { s.fillCircle(x, y, 7, ok ? C_GRN : C_RED); }

// ------------------------------------------------------------------ header
void drawHead(LGFX_Sprite& s, const View& v) {
  s.fillScreen(C_BG);
  s.drawFastHLine(0, 75, SW, C_LINE);
  text(s, &fTitle, "\xEE\x80\x80", 24, 56, C_AMB, baseline_left);            // U+E000 = the jackal
  text(s, &fTitle, "BEAT COMMAND", 70, 52, C_AMB, baseline_left);
  const char* lbl = "NO GATEWAY"; uint16_t col = C_RED; bool solid = false;
  switch (v.link) {
    case LS_WAIT: lbl = "WAITING"; col = C_AMB_D; break;
    case LS_LIVE: lbl = v.viaBooth ? "BOOTH" : "LIVE"; col = C_GRN; solid = true; break;
    case LS_HOLD: lbl = "HOLD"; col = C_AMB; break;
    case LS_TAKE: lbl = v.conflict ? "CLASH" : "TAKEOVER"; col = v.conflict ? C_RED : C_PURP; solid = true; break;
  }
  pill(s, 640, 38, lbl, col, solid);
  // right side: gateway, bridge, battery
  int x = 1256;
  char b[16];
  if (v.battery >= 0) {
    snprintf(b, sizeof(b), "%d%%", v.battery);
    text(s, &fMono, b, x, 40, v.battery > 20 ? C_TXT : C_RED, middle_right);
    x -= textW(s, &fMono, b) + 12;
    s.drawRoundRect(x - 40, 28, 36, 22, 4, C_AMB_D); s.fillRect(x - 4, 34, 3, 10, C_AMB_D);
    s.fillRect(x - 37, 31, 30 * (v.battery < 0 ? 0 : v.battery > 100 ? 100 : v.battery) / 100, 16, v.charging ? C_GRN : (v.battery > 20 ? C_AMB : C_RED));
    x -= 64;
  }
  text(s, &fMono, "BRIDGE", x, 40, C_TXT, middle_right); dot(s, x - textW(s, &fMono, "BRIDGE") - 16, 39, v.bridgeOk);
  x -= textW(s, &fMono, "BRIDGE") + 52;
  text(s, &fMono, "GATEWAY", x, 40, C_TXT, middle_right); dot(s, x - textW(s, &fMono, "GATEWAY") - 16, 39, v.gwOk);
}

// ------------------------------------------------------------------ pads (left)
void padBox(LGFX_Sprite& s, const Rect& r, bool on, bool pressed, uint16_t col) {
  int x = r.x - PANEL[P_LEFT].x, y = r.y - PANEL[P_LEFT].y;
  if (pressed) s.fillRoundRect(x, y, r.w, r.h, 20, col);
  else {
    s.fillRoundRect(x, y, r.w, r.h, 20, C_CARD);
    for (int i = 0; i < 2; i++) s.drawRoundRect(x + i, y + i, r.w - 2 * i, r.h - 2 * i, 20 - i, on ? col : C_LINE);
  }
}
void padLabel(LGFX_Sprite& s, const Rect& r, const char* main, const char* sub, bool on, bool pressed, uint16_t col) {
  int x = r.x - PANEL[P_LEFT].x + r.w / 2, y = r.y - PANEL[P_LEFT].y + r.h / 2;
  uint16_t tc = pressed ? C_BG : (on ? col : C_FAINT);
  text(s, &fPad, main, x, sub ? y - 12 : y + 2, tc, middle_center);
  if (sub) text(s, &fMono, sub, x, y + 30, pressed ? C_BG : (on ? C_AMB_D : C_FAINT), middle_center);
}
void arrow(LGFX_Sprite& s, const Rect& r, int dir, bool on, bool pressed) {
  int cx = r.x - PANEL[P_LEFT].x + r.w / 2, cy = r.y - PANEL[P_LEFT].y + r.h / 2 - 10;
  uint16_t c = pressed ? C_BG : (on ? C_AMB : C_FAINT);
  s.fillTriangle(cx + dir * 22, cy, cx - dir * 14, cy - 22, cx - dir * 14, cy + 22, c);
}

void drawLeft(LGFX_Sprite& s, const View& v) {
  s.fillScreen(C_BG);
  bool take = v.mode == M_TAKE, live = v.link != LS_NOGW;
  // TAKE / RELEASE
  uint16_t tc = take ? C_GRN : C_AMB;
  padBox(s, PAD[PAD_TAKE], live, v.pressed == PAD_TAKE, tc);
  padLabel(s, PAD[PAD_TAKE], take ? "RELEASE" : "TAKE", take ? "hand the beat back" : "drive the rig from here", live, v.pressed == PAD_TAKE, tc);
  // TAP
  padBox(s, PAD[PAD_TAP], live, v.pressed == PAD_TAP, C_AMB);
  padLabel(s, PAD[PAD_TAP], "TAP", take ? "tap tempo" : "tap = take over", live, v.pressed == PAD_TAP, C_AMB);
  for (int i = 0; i < 4; i++) {
    int x = PAD[PAD_TAP].x + PAD[PAD_TAP].w / 2 - 45 + i * 30, y = PAD[PAD_TAP].y - PANEL[P_LEFT].y + PAD[PAD_TAP].h - 26;
    if (i < v.tapCount) s.fillCircle(x, y, 7, v.pressed == PAD_TAP ? C_BG : C_AMB); else s.drawCircle(x, y, 7, v.pressed == PAD_TAP ? C_BG : C_LINE);
  }
  // - / +
  for (int p = PAD_MINUS; p <= PAD_PLUS; p++) {
    padBox(s, PAD[p], take, v.pressed == p, C_AMB);
    padLabel(s, PAD[p], p == PAD_MINUS ? "-" : "+", "0.1 BPM", take, v.pressed == p, C_AMB);
  }
  // nudge
  for (int p = PAD_EARLY; p <= PAD_LATE; p++) {
    padBox(s, PAD[p], take, v.pressed == p, C_AMB);
    arrow(s, PAD[p], p == PAD_EARLY ? -1 : 1, take, v.pressed == p);
    int x = PAD[p].x - PANEL[P_LEFT].x + PAD[p].w / 2, y = PAD[p].y - PANEL[P_LEFT].y + PAD[p].h / 2 + 30;
    text(s, &fMono, p == PAD_EARLY ? "-10 ms" : "+10 ms", x, y, v.pressed == p ? C_BG : (take ? C_AMB_D : C_FAINT), middle_center);
  }
  text(s, &fH2, "TEMPO", 168, PAD[PAD_MINUS].y - PANEL[P_LEFT].y - 18, take ? C_AMB_D : C_FAINT, middle_center);
  text(s, &fH2, "NUDGE", 168, PAD[PAD_EARLY].y - PANEL[P_LEFT].y - 18, take ? C_AMB_D : C_FAINT, middle_center);
  s.drawFastVLine(335, 0, s.height(), C_LINE);
}

// ------------------------------------------------------------------ center: ring, BPM, history
constexpr int RCX = 304, RCY = 292, R_OUT = 248, R_IN = 220;

int flashLevel(const View& v, uint32_t now) {
  uint32_t dt = now - v.flashMs;
  if (v.link == LS_NOGW || v.link == LS_WAIT || dt > 420) return 0;
  return 255 - (int)dt * 255 / 420;
}

void drawCenter(LGFX_Sprite& s, const View& v, uint32_t now) {
  s.fillScreen(C_BG);
  bool active = v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE;
  int lvl = flashLevel(v, now);
  // glow disc behind the number
  if (lvl > 0) s.fillCircle(RCX, RCY, R_IN - 14, hueGlow(v.hue, (v.flashDown ? lvl : lvl * 45 / 100) * 28 / 100));
  // beat ring: beat 1 at the top, clockwise
  for (int i = 0; i < 4; i++) {
    float a0 = -90 + i * 90 - 40, a1 = -90 + i * 90 + 40;
    if (a0 < 0) { a0 += 360; a1 += 360; }
    bool on = active && v.beatPos == i;
    uint16_t c = on ? hueCol(v.hue, 120 + lvl * 135 / 255) : (active ? C_LINE : c565(28, 24, 14));
    s.fillArc(RCX, RCY, R_IN, R_OUT, a0, a1, c);
    if (i == 0) s.fillArc(RCX, RCY, R_OUT + 8, R_OUT + 14, a0 + 20, a1 - 20, active ? C_AMB : C_LINE);   // marks beat 1
  }
  // BPM
  char ip[8], dp[8];
  uint16_t numCol = active ? (lvl > 160 ? 0xFFFF : C_TXT) : C_FAINT;
  if (active && v.bpm > 0) { int t = (int)lroundf(v.bpm * 10); snprintf(ip, sizeof(ip), "%d", t / 10); snprintf(dp, sizeof(dp), ".%d", t % 10); }
  else { strcpy(ip, "---"); dp[0] = 0; }
  int wi = textW(s, &fBpm, ip), wd = dp[0] ? textW(s, &fDec, dp) : 0, x0 = RCX - (wi + wd) / 2;
  text(s, &fBpm, ip, x0, RCY + 50, numCol, baseline_left);
  if (dp[0]) text(s, &fDec, dp, x0 + wi + 2, RCY + 50, numCol, baseline_left);
  text(s, &fH2, "BPM", RCX, RCY + 84, C_AMB_D, middle_center);
  const char* who = "";
  switch (v.link) {
    case LS_LIVE: who = v.viaBooth ? "FOLLOWING THE BOOTH" : "FOLLOWING THE W10"; break;
    case LS_HOLD: who = "HOLDING TIME, LINK QUIET"; break;
    case LS_TAKE: who = "TAB5 IS DRIVING"; break;
    case LS_WAIT: who = "WAITING FOR A BEAT"; break;
    default: break;
  }
  text(s, &fMono, who, RCX, RCY + 122, v.link == LS_TAKE ? C_PURP : C_AMB_D, middle_center);
  if (v.note) text(s, &fMono, v.note, RCX, RCY + 152, C_RED, middle_center);
  if (active) {
    char b[24]; snprintf(b, sizeof(b), "BAR %lu", (unsigned long)v.bars); text(s, &fH2, b, RCX, RCY - 112, C_AMB_D, middle_center);
  }
  // no gateway: say what to do
  if (v.link == LS_NOGW) {
    s.fillRoundRect(64, RCY - 70, 480, 140, 18, C_CARD); s.drawRoundRect(64, RCY - 70, 480, 140, 18, C_RED);
    text(s, &fMsg, "PLUG IN", RCX, RCY - 30, C_AMB, middle_center);
    text(s, &fMono, "the Beat Gateway (XIAO) into the", RCX, RCY + 14, C_TXT, middle_center);
    text(s, &fMono, "Tab5's USB-A port", RCX, RCY + 42, C_TXT, middle_center);
  }
  // BPM history (last 2 minutes)
  int gx = 24, gy = 560, gw = 560, gh = 56;
  s.drawRoundRect(gx - 8, gy - 26, gw + 16, gh + 40, 10, C_LINE);
  text(s, &fMono, "TEMPO, LAST 2 MIN", gx, gy - 14, C_AMB_D, middle_left);
  float lo = 1e9, hi = -1e9; int n = 0;
  if (v.hist) for (int i = 0; i < HIST_N; i++) { float b = v.hist[i]; if (b > 0) { lo = fminf(lo, b); hi = fmaxf(hi, b); n++; } }
  if (n) {
    char b[32]; snprintf(b, sizeof(b), "%.1f - %.1f", lo, hi); text(s, &fMono, b, gx + gw, gy - 14, C_AMB_D, middle_right);
    float mid = (lo + hi) / 2, span = fmaxf(hi - lo, 2.0f);
    int px = -1, py = 0;
    for (int i = 0; i < HIST_N; i++) {
      float b = v.hist[(v.histPos + i) % HIST_N];
      int x = gx + i * (gw - 1) / (HIST_N - 1);
      if (b <= 0) { px = -1; continue; }
      int y = gy + gh / 2 - (int)((b - mid) / span * (gh - 8));
      if (px >= 0) { s.drawLine(px, py, x, y, C_AMB); s.drawLine(px, py + 1, x, y + 1, C_AMB); }
      px = x; py = y;
    }
  } else text(s, &fMono, "no beats yet", gx + gw / 2, gy + gh / 2, C_FAINT, middle_center);
  // toast
  if (v.toast != T_NONE) {
    const char* m = v.toast == T_TAKE ? "TAKEOVER" : v.toast == T_RELEASE ? "RELEASED" : "TAKE OVER FIRST";
    uint16_t c = v.toast == T_TAKE ? C_PURP : v.toast == T_RELEASE ? C_GRN : C_AMB;
    int w = textW(s, &fMsg, m) + 64;
    s.fillRoundRect(RCX - w / 2, 40, w, 64, 32, C_BG); s.drawRoundRect(RCX - w / 2, 40, w, 64, 32, c);
    text(s, &fMsg, m, RCX, 74, c, middle_center);
  }
}

// ------------------------------------------------------------------ right: stats
void stat(LGFX_Sprite& s, int y, const char* label, const char* val, uint16_t col) {
  text(s, &fMono, label, 24, y, C_AMB_D, middle_left);
  text(s, &fMonoL, val, 312, y, col, middle_right);
  s.drawFastHLine(24, y + 30, 288, C_LINE);
}

void drawRight(LGFX_Sprite& s, const View& v) {
  s.fillScreen(C_BG);
  s.drawFastVLine(0, 0, s.height(), C_LINE);
  if (v.debug && v.dbg) {                            // raw frames, newest at the bottom (hold 1 s to close)
    text(s, &fH2, "LINK FRAMES", 24, 20, C_AMB_D, middle_left);
    int n = v.dbgCount < (uint32_t)DBG_N ? v.dbgCount : DBG_N;
    for (int i = 0; i < n; i++) {
      const char* l = v.dbg[(v.dbgCount - n + i) % DBG_N];
      text(s, &fMono, l, 16, 56 + i * 36, l[0] ? C_TXT : C_FAINT, middle_left);
    }
    return;
  }
  bool take = v.mode == M_TAKE;
  char b[32];
  const char* src = v.link == LS_TAKE ? "TAB5" : (v.link == LS_LIVE || v.link == LS_HOLD) ? (v.viaBooth ? "BOOTH" : "W10") : "-";
  int y = 44;
  stat(s, y, "SOURCE", src, v.link == LS_TAKE ? C_PURP : C_TXT); y += 72;
  if (v.rssi) snprintf(b, sizeof(b), "%d dBm", v.rssi); else strcpy(b, "-");
  stat(s, y, "RSSI", b, v.rssi == 0 ? C_FAINT : v.rssi > -90 ? C_GRN : v.rssi > -105 ? C_AMB : C_RED); y += 72;
  if (v.rssi) snprintf(b, sizeof(b), "%+.1f dB", v.snr); else strcpy(b, "-");
  stat(s, y, "SNR", b, C_TXT); y += 72;
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.rx); stat(s, y, "BEATS IN", b, C_TXT); y += 72;
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.lost); stat(s, y, "LOST", b, v.lost ? C_AMB : C_TXT); y += 72;
  snprintf(b, sizeof(b), "%lu", (unsigned long)v.sent); stat(s, y, "SENT", b, take ? (v.fail > 2 ? C_RED : C_GRN) : C_FAINT); y += 72;
  snprintf(b, sizeof(b), "%+d ms", v.nudgeMs); stat(s, y, "NUDGE", b, take ? C_TXT : C_FAINT); y += 72;
  // gateway card
  s.fillRoundRect(16, 548, 304, 80, 14, C_CARD); s.drawRoundRect(16, 548, 304, 80, 14, v.gwOk ? C_LINE : C_RED);
  text(s, &fH2, "GATEWAY", 36, 572, C_AMB_D, middle_left);
  text(s, &fMono, v.gwOk ? (v.gwName && v.gwName[0] ? v.gwName : "connected") : "not plugged in", 36, 604, v.gwOk ? C_TXT : C_RED, middle_left);
  if (take && !v.bridgeOk) text(s, &fMono, "no bridge", 300, 572, C_RED, middle_right);
  else if (v.gwOk && v.framesSent) {                 // radio frames vs frames that made it over USB
    char b[24]; snprintf(b, sizeof(b), "%lu/%lu", (unsigned long)v.framesGot, (unsigned long)v.framesSent);
    text(s, &fMono, b, 300, 572, v.framesSent > v.framesGot + 5 ? C_AMB : C_AMB_D, middle_right);
  }
}

// a cheap fingerprint of what each static panel shows, to skip redraws when nothing changed
uint32_t mix(uint32_t h, uint32_t x) { return (h ^ x) * 16777619u; }
uint32_t hashHead(const View& v) { uint32_t h = 2166136261u; h = mix(h, v.link); h = mix(h, v.viaBooth); h = mix(h, v.conflict); h = mix(h, v.gwOk);
  h = mix(h, v.bridgeOk); h = mix(h, v.battery); h = mix(h, v.charging); return h; }
uint32_t hashLeft(const View& v) { uint32_t h = 2166136261u; h = mix(h, v.mode); h = mix(h, v.link == LS_NOGW); h = mix(h, v.pressed);
  h = mix(h, v.tapCount); return h; }
uint32_t hashRight(const View& v) { uint32_t h = 2166136261u; h = mix(h, v.mode); h = mix(h, v.link); h = mix(h, v.viaBooth); h = mix(h, v.rssi);
  h = mix(h, (uint32_t)lroundf(v.snr * 4)); h = mix(h, v.rx); h = mix(h, v.lost); h = mix(h, v.sent); h = mix(h, v.fail > 2);
  h = mix(h, v.nudgeMs); h = mix(h, v.gwOk); h = mix(h, v.bridgeOk); h = mix(h, (uint32_t)(uintptr_t)v.gwName);
  h = mix(h, v.framesSent); h = mix(h, v.debug); if (v.debug) h = mix(h, v.dbgCount); return h; }
