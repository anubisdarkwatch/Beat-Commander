// Host preview for the Beat Strip: renders each pattern in c5_beat_strip/strip.h as a time strip
// (each column = one LED, each row = 16 ms; two bars at 128 BPM). Output: strip_<pattern>.ppm
// Build: g++ -O2 -o preview_strip tools/preview_strip.cpp && ./preview_strip
#include <cstdio>
#include "../c5_beat_strip/strip.h"

int main() {
  const int N = 60, ROWS = 2 * 4 * 469 / 16, K = 6;        // 2 bars of 128 BPM, 16 ms per row, LEDs drawn 6 px wide
  static uint8_t rgb[STRIP_MAX * 3];
  for (int p = 0; p < SP_COUNT; p++) {
    SState s = {}; s.n = N; s.pattern = p; s.live = true; s.bpm10 = 1280; s.hue = 200;
    sLastMs = 0; memset(sTrail, 0, sizeof(sTrail));
    char path[64]; snprintf(path, sizeof(path), "strip_%s.ppm", SP_NAME[p]);
    FILE* f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", N * K, ROWS);
    uint32_t t0 = 100000;
    for (int r = 0; r < ROWS; r++) {
      uint32_t now = t0 + r * 16;
      uint32_t beat = (now - t0) / 469;
      if (beat + 1 != s.beatId) { s.beatId = beat + 1; s.beatPos = beat & 3; s.beatMs = t0 + beat * 469; if (s.beatPos == 0) { s.bars++; s.hue += 40; } }
      stripRender(rgb, s, now);
      for (int i = 0; i < N; i++) for (int k = 0; k < K; k++) {
        uint8_t px[3] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
        if (k == K - 1) px[0] = px[1] = px[2] = 0;          // gap between LEDs
        fwrite(px, 1, 3, f);
      }
    }
    fclose(f);
  }
  return 0;
}
