// Host preview for the Cardputer Beat Commander terminal: renders cardputer_beat_terminal/ui.h screens to .ppm.
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_cardputer.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../cardputer_beat_terminal/ui.h"

M5Canvas cv;
static void save(const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", SW, SH);
  for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
    auto c = cv.readPixelRGB(x, y); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}
int main() {
  initFonts();
  cv.setColorDepth(16); cv.createSprite(SW, SH);
  uint32_t now = 100000;
  View v = {}; v.battery = 78; v.input = "";
  v.link = LS_WAIT; v.bridgeOk = true; draw(cv, v, now); save("0_waiting");
  v.link = LS_LIVE; v.bpm = 126.4f; v.beatPos = 0; v.hue = 200; v.bars = 41; v.rssi = -72; v.rx = 640; v.lost = 1;
  v.flashMs = now - 30; v.flashDown = true; draw(cv, v, now); save("1_live_downbeat");
  v.input = "128.5"; v.flashMs = now - 500; v.beatPos = 2; draw(cv, v, now); save("2_typing");
  v.input = ""; v.mode = M_TAKE; v.link = LS_TAKE; v.bpm = 128.5f; v.beatPos = 1; v.hue = 40; v.sent = 96; v.nudgeMs = -20;
  v.flashMs = now - 60; v.flashDown = false; v.toast = T_TAKE; draw(cv, v, now); save("3_takeover");
  v.toast = T_NONE; v.help = true; draw(cv, v, now); save("4_help");
  return 0;
}
