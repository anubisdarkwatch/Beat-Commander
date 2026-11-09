// Host preview for the Beat Visualizer: renders every mode at TTGO (240x135) and CYD (320x240) sizes,
// and writes frame sequences for GIFs. Build: tools/build_preview.sh <M5GFX folder> <out folder> preview_viz.cpp
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <M5GFX.h>
#include "../beat_visualizer/viz.h"

static void save(LGFX_Sprite& s, const char* name) {
  char path[256]; snprintf(path, sizeof(path), "%s.ppm", name);
  FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", s.width(), s.height());
  for (int y = 0; y < s.height(); y++) for (int x = 0; x < s.width(); x++) {
    auto c = s.readPixelRGB(x, y); uint8_t p[3] = {c.R8(), c.G8(), c.B8()}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}
// simulate a link at 126 BPM and run a mode for `ms`, saving frames every `every` ms from `from`
static void run(LGFX_Sprite& s, uint8_t mode, const char* tag, int ms, int every, int from, bool demo = false) {
  VizState v = {}; v.bpm = 126; v.hue = 200; v.mode = mode; v.src = demo ? SRC_DEMO : SRC_W10; v.hud = true;
  float per = 60000.0f / v.bpm; uint32_t t0 = 10000; int beat = -1;
  for (int t = 0; t <= ms; t += 33) {
    uint32_t now = t0 + t; int b = (int)(t / per);
    if (b != beat) { beat = b; v.beatId = b; v.beatPos = b & 3; v.beatMs = t0 + (uint32_t)(b * per); if (v.beatPos == 0) { v.bars++; v.hue += 40; } }
    vizDraw(s, v, now);
    if (t >= from && (t - from) % every < 33) { char n[64]; snprintf(n, sizeof(n), "%s_%05d", tag, t); save(s, n); }
  }
}
int main() {
  vizInitFonts();
  LGFX_Sprite tt, cyd; tt.setColorDepth(16); tt.createSprite(240, 135); cyd.setColorDepth(8); cyd.createSprite(320, 240);
  const char* names[V_COUNT] = {"pulse", "tunnel", "bars", "warp", "plasma", "bpm"};
  for (int m = 0; m < V_COUNT; m++) {
    char tag[32];
    snprintf(tag, sizeof(tag), "tt_%s", names[m]); run(tt, m, tag, 2400, 300, 1900);    // stills + short anim (TTGO)
    snprintf(tag, sizeof(tag), "cyd_%s", names[m]); run(cyd, m, tag, 3800, 66, 1900);  // CYD
  }
  run(tt, V_PULSE, "tt_demo", 600, 600, 600, true);
  return 0;
}
