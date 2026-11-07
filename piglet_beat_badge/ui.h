// Beat badge screens for the Piglet (XIAO ESP32-C5 + 128x64 SSD1306 OLED). 1-bit drawing.
// Shared by the firmware and tools/preview_piglet.cpp (host preview): LovyanGFX / M5GFX calls only.
#pragma once
#include <math.h>
#include "fonts_piglet.h"

constexpr int SW = 128, SH = 64;
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE, LS_NORADIO };
enum Page : uint8_t { PG_BEAT, PG_STATS, PG_COUNT };

struct View {
  uint8_t link, page;
  bool viaBooth, conflict, bridgeOk;
  float bpm; uint8_t beatPos; uint32_t bars;
  int sig;                                   // ESP-NOW signal from the bridge (dBm), 0 = none yet
  int rssi;                                  // LoRa RSSI the bridge reported for the W10, 0 = none
  uint32_t rx, lost, sent;
  uint32_t flashMs; bool flashDown;
  uint8_t tapCount; uint32_t holdMs;         // button held this long (for the TAKE/RELEASE progress bar)
  const char* toast;
};

lgfx::VLWfont fBpm, fDec, fHead;
lgfx::PointerWrapper wBpm, wDec, wHead;
void initFonts() {
  wBpm.set(F_BPM, sizeof(F_BPM)); fBpm.loadFont(&wBpm);
  wDec.set(F_DEC, sizeof(F_DEC)); fDec.loadFont(&wDec);
  wHead.set(F_HEAD, sizeof(F_HEAD)); fHead.loadFont(&wHead);
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void txt(LGFX_Sprite& s, const lgfx::IFont* f, const char* str, int x, int y, int col, textdatum_t d) {
  s.setFont(f); s.setTextDatum(d); s.setTextColor(col); s.drawString(str, x, y);
}

int flashAge(const View& v, uint32_t now) { return (int)(now - v.flashMs); }

void draw(LGFX_Sprite& s, const View& v, uint32_t now) {
  bool active = v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE;
  bool invert = active && v.flashDown && flashAge(v, now) < 90;   // whole screen flashes on beat 1
  int fg = invert ? 0 : 1, bg = invert ? 1 : 0;
  s.fillScreen(bg);

  // header: label, status pill
  const char* lbl = "WAIT";
  switch (v.link) {
    case LS_LIVE: lbl = v.viaBooth ? "BOOTH" : "LIVE"; break;
    case LS_HOLD: lbl = "HOLD"; break;
    case LS_TAKE: lbl = v.conflict ? "CLASH" : "TAKE"; break;
    case LS_NORADIO: lbl = "ERR"; break;
  }
  s.setFont(&fHead); int pw = s.textWidth(lbl) + 8;
  bool solid = v.link == LS_LIVE || v.link == LS_TAKE;
  if (solid) { s.fillRoundRect(SW - pw, 0, pw, 12, 3, fg); txt(s, &fHead, lbl, SW - pw / 2, 10, bg, baseline_center); }
  else { s.drawRoundRect(SW - pw, 0, pw, 12, 3, fg); txt(s, &fHead, lbl, SW - pw / 2, 10, fg, baseline_center); }
  s.setFont(&fonts::Font0); s.setTextDatum(baseline_left); s.setTextColor(fg);
  char b[24];
  if (v.page == PG_BEAT && active) { snprintf(b, sizeof(b), "BAR %lu", (unsigned long)v.bars); s.drawString(b, 0, 9); }
  else s.drawString("BEAT BADGE", 0, 9);

  if (v.page == PG_BEAT) {
    // BPM
    char ip[8], dp[6];
    if (active && v.bpm > 0) { int t = (int)lroundf(v.bpm * 10); snprintf(ip, sizeof(ip), "%d", t / 10); snprintf(dp, sizeof(dp), ".%d", t % 10); }
    else { strcpy(ip, "---"); dp[0] = 0; }
    s.setFont(&fBpm); int wi = s.textWidth(ip); s.setFont(&fDec); int wd = dp[0] ? s.textWidth(dp) : 0;
    int x0 = (SW - wi - wd - 2) / 2;
    txt(s, &fBpm, ip, x0, 50, fg, baseline_left);
    if (dp[0]) txt(s, &fDec, dp, x0 + wi + 2, 50, fg, baseline_left);
    // beat boxes
    for (int i = 0; i < 4; i++) {
      int x = i * 32 + 1;
      bool on = active && v.beatPos == i && flashAge(v, now) < 300;
      if (on) s.fillRect(x, 56, 30, 8, fg); else s.drawRect(x, 56, 30, 8, fg);
    }
  } else {
    // stats page: walk the room with it to check coverage
    s.setFont(&fonts::Font0); s.setTextDatum(top_left); s.setTextColor(fg);
    auto row = [&](int y, const char* k, const char* val) { s.drawString(k, 0, y); s.setTextDatum(top_right); s.drawString(val, SW, y); s.setTextDatum(top_left); };
    if (v.sig) snprintf(b, sizeof(b), "%d dBm", v.sig); else strcpy(b, "-");
    row(16, "SIGNAL", b);
    if (v.rssi) snprintf(b, sizeof(b), "%d dBm", v.rssi); else strcpy(b, "-");
    row(26, "W10@BRIDGE", b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)v.rx); row(36, "BEATS IN", b);
    snprintf(b, sizeof(b), "%lu", (unsigned long)v.lost); row(46, "LOST", b);
    s.drawString(v.bridgeOk ? "bridge OK" : "no bridge", 0, 56);
    // signal bar
    int lvl = v.sig ? clampi((v.sig + 95) * 40 / 55, 0, 40) : 0;
    s.drawRect(SW - 42, 56, 42, 8, fg); s.fillRect(SW - 41, 57, lvl, 6, fg);
  }

  // hold-to-take progress and toasts
  if (v.holdMs > 300) {
    int w = clampi((int)(v.holdMs - 300) * SW / 1700, 0, SW);
    s.fillRect(0, 13, w, 2, fg);                   // under the header
  }
  if (v.toast && v.toast[0]) {
    s.setFont(&fHead); int w = s.textWidth(v.toast) + 12;
    s.fillRoundRect(SW / 2 - w / 2, 22, w, 18, 4, bg); s.drawRoundRect(SW / 2 - w / 2, 22, w, 18, 4, fg);
    txt(s, &fHead, v.toast, SW / 2, 35, fg, baseline_center);
  }
}
