// Host preview for the Heltec V3 beat screen: renders heltec_v3_beat_follower/oled_viz.h to .ppm (scaled x4),
// through the same SSD1306 page conversion the firmware uses (so the bit order is checked too).
// Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_oled.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../heltec_v3_beat_follower/oled_viz.h"

M5Canvas cv;
static void save(const char* name) {
  uint8_t pages[8][128];
  for (int p = 0; p < 8; p++) oledPage(cv, p, pages[p]);
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  const int K = 4; FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", OW * K, OH * K);
  for (int y = 0; y < OH * K; y++) for (int x = 0; x < OW * K; x++) {
    int px = x / K, py = y / K; bool on = pages[py / 8][px] >> (py & 7) & 1;
    uint8_t c[3] = {on ? (uint8_t)150 : (uint8_t)4, on ? (uint8_t)225 : (uint8_t)6, on ? (uint8_t)255 : (uint8_t)10};
    fwrite(c, 1, 3, f);
  }
  fclose(f);
}
int main() {
  oledInitFonts();
  cv.setColorDepth(1); cv.createSprite(OW, OH);
  uint32_t now = 100000;
  OState v = {}; v.link = OL_LIVE; v.bpm10 = 1264; v.beatPos = 0; v.beatMs = now - 40; v.beatId = 77; v.bars = 41;
  v.rssi = -74; v.snr4 = 38; v.pkts = 640; v.lost = 1;
  const char* names[] = {"0_beat", "1_pulse", "2_bars", "3_tunnel", "4_warp", "5_scope", "6_stats"};
  for (int m = 0; m < O_COUNT; m++) {
    v.mode = m;
    for (int i = 0; i < 20; i++) oledDraw(cv, v, now - 400 + i * 20);   // let the animated modes settle
    oledDraw(cv, v, now); save(names[m]);
  }
  v.mode = O_TUNNEL; v.labelUntil = now + 500; v.autoMode = true; oledDraw(cv, v, now); save("7_label");
  v.labelUntil = 0; v.link = OL_WAIT; v.mode = O_PULSE; oledDraw(cv, v, now); save("8_wait");
  v.bridge = true; v.link = OL_BOOTH; v.mode = O_STATS; v.pkts = 1820; v.sent = 96; v.clients = 3; oledDraw(cv, v, now); save("9_bridge_stats");
  return 0;
}
