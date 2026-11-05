// PC preview of every Beat Console screen.
// g++ -std=c++17 -O2 -I cyd_beat_console tools/preview_console.cpp -o preview && ./preview outdir
#include <cstdio>
#include <string>
#include "ui.h"

static uint16_t fb[SCR_W * SCR_H];
void panelPushBand() { memcpy(fb + band.y0 * SCR_W, band.px, band.h * SCR_W * 2); }

static void save(const std::string& path) {
  FILE* f = fopen(path.c_str(), "wb");
  fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
  for (int i = 0; i < SCR_W * SCR_H; i++) {
    uint16_t c = fb[i];
    uint8_t px[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
    fwrite(px, 1, 3, f);
  }
  fclose(f);
}

int main(int argc, char** argv) {
  std::string out = argc > 1 ? argv[1] : ".";
  drawSplash(0.15f); save(out + "/00-boot-slit.ppm");
  drawSplash(1.0f); save(out + "/01-splash.ppm");

  UiState s;
  drawAll(s); save(out + "/02-no-bridge.ppm");
  s.bridgeOk = s.radioOk = true; drawAll(s); save(out + "/03-waiting.ppm");

  s.link = LS_LIVE; s.bpm = 128.0f; s.beatPos = 2; s.hue = 200; s.rssi = -76; s.snr = 9.5f; s.rx = 312; s.lost = 0;
  s.flash = 0; drawAll(s); save(out + "/04-live.ppm");
  s.beatPos = 0; s.flash = 1; s.downbeat = true; s.hue = 24; drawAll(s); save(out + "/05-live-downbeat-flash.ppm");
  s.beatPos = 1; s.flash = 0.7f; s.downbeat = false; s.hue = 120; drawAll(s); save(out + "/06-live-beat-flash.ppm");
  s.flash = 0; s.link = LS_HOLD; s.beatPos = 3; s.lost = 3; s.rssi = -104; s.snr = -3.25f; drawAll(s); save(out + "/07-hold.ppm");

  s.mode = M_TAKE; s.link = LS_TAKE; s.bpm = 174.0f; s.beatPos = 1; s.hue = 80; s.sent = 96; s.fail = 0; s.nudgeMs = -20;
  s.rssi = -76; s.pressed = B_TAP; drawAll(s); save(out + "/08-takeover-tap.ppm");
  s.pressed = -1; s.conflict = true; s.fail = 2; drawAll(s); save(out + "/09-takeover-conflict.ppm");
  drawCal(0, 20, 20); save(out + "/10-calibrate.ppm");
  return 0;
}
