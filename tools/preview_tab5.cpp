// Host preview for the Tab5 Beat Command Center: renders the screens from tab5_command_center/ui.h to .ppm.
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_tab5.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../tab5_command_center/ui.h"

M5Canvas full, ps[P_COUNT];

static void compose(const View& v, uint32_t now) {
  drawHead(ps[P_HEAD], v); drawLeft(ps[P_LEFT], v); drawCenter(ps[P_CENTER], v, now); drawRight(ps[P_RIGHT], v);
  for (int i = 0; i < P_COUNT; i++) ps[i].pushSprite(&full, PANEL[i].x, PANEL[i].y);
}
static void save(const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", SW, SH);
  for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
    auto c = full.readPixelRGB(x, y); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}

int main() {
  initFonts();
  full.setColorDepth(16); full.createSprite(SW, SH);
  for (int i = 0; i < P_COUNT; i++) { ps[i].setColorDepth(16); ps[i].createSprite(PANEL[i].w, PANEL[i].h); }
  static float hist[HIST_N];
  for (int i = 0; i < HIST_N; i++) hist[i] = i < 20 ? 0 : 124.0f + 2.4f * (i - 20) / 100.0f + 0.15f * sinf(i * 0.7f);
  uint32_t now = 100000;

  View v = {}; v.pressed = PAD_NONE; v.battery = 86; v.gwOk = true; v.gwName = "Beat Gateway v1.1"; v.bridgeOk = true;
  v.radioOk = true; v.hist = hist; v.histPos = 0;
  v.link = LS_NOGW; v.gwOk = false; v.bridgeOk = false; compose(v, now); save("0_no_gateway");
  v.gwOk = true; v.bridgeOk = true;
  v.mode = M_FOLLOW; v.link = LS_LIVE; v.bpm = 126.4f; v.beatPos = 0; v.hue = 200; v.bars = 87; v.flashMs = now - 40; v.flashDown = true;
  v.rssi = -71; v.snr = 9.5f; v.rx = 1348; v.lost = 2;
  compose(v, now); save("1_live_downbeat");
  v.beatPos = 2; v.flashDown = false; v.flashMs = now - 250; v.pressed = PAD_TAKE; compose(v, now); save("2_live_offbeat_press");
  v.pressed = PAD_NONE; v.mode = M_TAKE; v.link = LS_TAKE; v.bpm = 127.0f; v.beatPos = 1; v.hue = 40; v.sent = 312; v.nudgeMs = 20;
  v.flashMs = now - 60; v.tapCount = 3; v.toast = T_TAKE; compose(v, now); save("3_takeover");
  v.toast = T_NONE; v.conflict = true; v.beatPos = 3; v.flashMs = now - 500; v.tapCount = 0; compose(v, now); save("4_clash");
  v = {}; v.pressed = PAD_NONE; v.battery = 54; v.gwOk = true; v.gwName = "Beat Gateway v1.1"; v.link = LS_WAIT; compose(v, now); save("5_waiting");
  return 0;
}
