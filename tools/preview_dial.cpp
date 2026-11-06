// Host preview for the M5Dial Beat Dial: renders each screen from m5dial_beat_dial/ui.h to a .ppm file.
// Build (Linux, needs M5GFX source + SDL2 headers): see tools/build_preview.sh
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <M5GFX.h>
#define FW_VERSION "v1.0"
M5Canvas cv;
bool lockOn = true;
#include "../m5dial_beat_dial/ui.h"

static void save(const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n240 240\n255\n");
  for (int y = 0; y < 240; y++) for (int x = 0; x < 240; x++) {
    auto c = cv.readPixelRGB(x, y); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}

int main() {
  cv.setColorDepth(16); cv.createSprite(240, 240); initColors();
  View v = {}; uint32_t now = 100000;
  splash(); save("0_splash");
  page("ENROLL", COL_PURP, "Scan your implant", "press knob = no lock", ""); save("1_enroll");
  v.mode = M_FOLLOW; v.link = LS_WAIT; v.bridgeOk = true; v.radioOk = true; v.bpm = 0; v.beatPos = 3; v.hue = 160;
  drawFrame(v, now); save("2_wait");
  v.link = LS_LIVE; v.bpm = 128.0; v.beatPos = 0; v.flashDown = true; v.flashMs = now - 40; v.hue = 200;
  v.rx = 412; v.rssi = -84; v.lost = 0;
  drawFrame(v, now); save("3_live_downbeat");
  v.beatPos = 2; v.flashDown = false; v.flashMs = now - 150; drawFrame(v, now); save("4_live_beat3");
  v.toast = T_SCAN; drawFrame(v, now); save("5_scan");
  v.toast = T_DENIED; drawFrame(v, now); save("6_denied");
  v.toast = T_NONE; v.mode = M_TAKE; v.link = LS_TAKE; v.bpm = 127.6; v.beatPos = 1; v.flashMs = now - 20; v.hue = 40;
  v.sent = 96; drawFrame(v, now); save("7_take");
  v.nudgeMs = -20; drawFrame(v, now); save("8_take_nudge");
  v.nudgeMs = 0; v.conflict = true; drawFrame(v, now); save("9_conflict");
  v = {}; v.mode = M_FOLLOW; v.link = LS_LIVE; v.viaBooth = true; v.bridgeOk = true; v.bpm = 124.0; v.beatPos = 3;
  v.hue = 100; v.flashMs = now - 60; v.rx = 50; drawFrame(v, now); save("10_following_booth");
  v.bridgeOk = false; v.link = LS_WAIT; drawFrame(v, now); save("11_no_link");
  return 0;
}
