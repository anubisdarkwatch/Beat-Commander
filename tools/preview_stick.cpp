// Host preview for the StickS3 Pocket Conductor: renders each screen from sticks3_pocket_conductor/ui.h to .ppm.
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_stick.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#define FW_VERSION "v1.0"
M5Canvas cv;
#include "../sticks3_pocket_conductor/ui.h"

static void save(const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", SW, SH);
  for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) {
    auto c = cv.readPixelRGB(x, y); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}

int main() {
  cv.setColorDepth(16); cv.createSprite(SW, SH); initColors();
  static float sc[135]; static bool on[135];
  for (int i = 0; i < 135; i++) { float ph = fmodf(i, 29.3f); sc[i] = ph < 2 ? 0.9f : 0.08f + 0.05f * sinf(i); on[i] = ph < 1; }
  uint32_t now = 100000;
  splash(); save("0_splash");
  View v = {}; v.mode = MODE_CONDUCT; v.micOk = true; v.bridgeOk = true; v.battery = 78; v.gain = 1;
  v.scope = sc; v.scopeOn = on; v.scopePos = 0; v.scopeN = 135; v.level = 0.09f;
  v.bpm = 126.4f; v.beatPos = 0; v.hue = 200; v.flashMs = now - 30; v.flashDown = true;
  drawFrame(v, now); save("1_local_downbeat");
  v.onAir = true; v.sent = 312; v.beatPos = 2; v.flashDown = false; v.flashMs = now - 120;
  drawFrame(v, now); save("2_on_air");
  v.conflict = true; drawFrame(v, now); save("3_clash");
  v.conflict = false; v.manual = true; v.toastGain = true; v.gain = 2; drawFrame(v, now); save("4_gain");
  v = {}; v.mode = MODE_FOLLOW; v.bridgeOk = true; v.battery = 64; v.link = LS_LIVE; v.rssi = -82; v.lost = 1;
  v.bpm = 128.0f; v.beatPos = 0; v.hue = 120; v.flashMs = now - 20; v.flashDown = true;
  drawFrame(v, now); save("5_follow_live");
  v.viaBooth = true; v.beatPos = 3; v.flashDown = false; v.flashMs = now - 200; drawFrame(v, now); save("6_follow_booth");
  v.bridgeOk = false; v.link = LS_WAIT; v.bpm = 0; drawFrame(v, now); save("7_no_link");
  return 0;
}
