// Host preview for the T-Embed Beat Embed: renders tembed_beat_embed/ui.h to .ppm.
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_embed.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../tembed_beat_embed/ui.h"

M5Canvas cv;
static void save(const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", SW, SH);
  for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) { auto c = cv.readPixelRGB(x, y); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f); }
  fclose(f);
}
int main() {
  embedInitFonts(); cv.setColorDepth(16); cv.createSprite(SW, SH);
  uint32_t now = 100000;
  View v = {}; v.battery = 76;
  drawBeatPage(cv, v, now); save("0_wait");
  v.link = LS_LIVE; v.bpm = 126.4f; v.beatPos = 0; v.hue = 200; v.bars = 41; v.rssi = -72; v.lost = 0; v.bridgeOk = true;
  v.flashMs = now - 40; v.flashDown = true; drawBeatPage(cv, v, now); save("1_live");
  v.mode = M_TAKE; v.link = LS_TAKE; v.bpm = 128.0f; v.beatPos = 2; v.hue = 40; v.sent = 64; v.flashDown = false; v.flashMs = now - 100;
  v.toast = T_TAKE; drawBeatPage(cv, v, now); drawToast(cv, v); save("2_take");
  static float sc[160]; static bool on[160];
  for (int i = 0; i < 160; i++) { sc[i] = (i % 23 < 2) ? 0.9f : fabsf(sinf(i * 0.7f)) * 0.2f; on[i] = i % 23 == 0; }
  v = {}; v.mode = M_CONDUCT; v.micOk = true; v.onAir = true; v.bpm = 124.6f; v.beatPos = 1; v.hue = 120; v.conf = 0.34f; v.gain = 1; v.sent = 210;
  v.bridgeOk = true; v.battery = 76; v.scope = sc; v.scopeOn = on; v.scopeN = 160; v.flashMs = now - 60;
  drawBeatPage(cv, v, now); save("3_conduct");
  v.hint = "LET GO: AUTO ON"; drawBeatPage(cv, v, now); drawToast(cv, v); save("4_hint");
  return 0;
}
