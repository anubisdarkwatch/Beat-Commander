// Host preview for the T-Dongle beat screen: renders tdongle_beat_gateway/dongle_ui.h pages to .ppm (x3).
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_dongle.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../tdongle_beat_gateway/dongle_ui.h"

M5Canvas cv;
static void save(const char* name) {
  const int K = 3; char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", DW * K, DH * K);
  for (int y = 0; y < DH * K; y++) for (int x = 0; x < DW * K; x++) {
    auto c = cv.readPixelRGB(x / K, y / K); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}
int main() {
  dongleInitFonts();
  cv.setColorDepth(16); cv.createSprite(DW, DH);
  uint32_t now = 100000;
  DView v = {}; v.gateway = true;
  dongleDraw(cv, v, now); save("0_wait");
  v.st = D_LOCK; v.src = DS_AIR; v.bpm10 = 1264; v.beatPos = 0; v.hue = 200; v.beatMs = now - 40; v.beatId = 9; v.bars = 41;
  v.clockRun = true; v.rssi = -74; v.beats = 640; v.frames = 2210; v.bridge = 1;
  dongleDraw(cv, v, now); save("1_gateway_live");
  v.gateway = false; v.src = DS_BOOTH; v.beatPos = 2; v.beatMs = now - 200; dongleDraw(cv, v, now); save("2_light_booth");
  v.page = P_STATS; dongleDraw(cv, v, now); save("3_stats");
  const char* nm[] = {"4_pulse", "5_tunnel", "6_bars", "7_warp", "8_plasma", "4k_kaleido", "4l_grid", "4m_pyramid", "4n_fireworks", "4o_rain"};
  for (int p = P_PULSE; p < P_COUNT; p++) {
    v.page = p; v.beatPos = 0;
    for (int i = 0; i < 20; i++) { v.beatMs = now - 400 + i * 20; dongleDraw(cv, v, now - 400 + i * 20); }
    v.beatMs = now - 60; dongleDraw(cv, v, now); save(nm[p - P_PULSE]);
  }
  v.page = P_PULSE; v.st = D_WAIT; dongleDraw(cv, v, now); save("9_viz_wait");
  v.page = P_BEAT; v.st = D_LOCK; v.hint = "LET GO: AUTO ON"; dongleDraw(cv, v, now); save("10_hint");
  return 0;
}
