// Host preview for the Piglet beat badge: renders piglet_beat_badge/ui.h screens to .ppm (scaled x4).
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_piglet.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../piglet_beat_badge/ui.h"

M5Canvas cv;
static void save(const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  const int K = 4; FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", SW * K, SH * K);
  for (int y = 0; y < SH * K; y++) for (int x = 0; x < SW * K; x++) {
    bool on = cv.readPixel(x / K, y / K) != 0; uint8_t p[3] = {on ? (uint8_t)140 : (uint8_t)4, on ? (uint8_t)220 : (uint8_t)6, on ? (uint8_t)255 : (uint8_t)10};
    fwrite(p, 1, 3, f);
  }
  fclose(f);
}
int main() {
  initFonts();
  cv.setColorDepth(1); cv.createSprite(SW, SH);
  uint32_t now = 100000;
  View v = {}; v.toast = "";
  v.link = LS_WAIT; draw(cv, v, now); save("0_wait");
  v.link = LS_LIVE; v.bpm = 126.4f; v.beatPos = 2; v.bars = 41; v.flashMs = now - 50; v.sig = -58; v.rssi = -74; v.rx = 640; v.lost = 2; v.bridgeOk = true;
  draw(cv, v, now); save("1_live");
  v.beatPos = 0; v.flashDown = true; v.flashMs = now - 30; draw(cv, v, now); save("2_downbeat");
  v.flashDown = false; v.page = PG_STATS; draw(cv, v, now); save("3_stats");
  v.page = PG_BEAT; v.link = LS_TAKE; v.bpm = 128.0f; v.beatPos = 1; v.holdMs = 1200; v.toast = ""; draw(cv, v, now); save("4_take_holding");
  v.holdMs = 0; v.toast = "TAKEOVER"; draw(cv, v, now); save("5_toast");
  return 0;
}
