// Pocket Conductor screens: everything drawn into the 135x240 frame buffer `cv` (portrait).
// Shared by the firmware and tools/preview_stick.cpp (host preview), so it uses only LovyanGFX / M5GFX calls.
// Before including: declare `cv` (an LGFX_Sprite / M5Canvas) and FW_VERSION.
#pragma once

enum Mode : uint8_t { MODE_CONDUCT, MODE_FOLLOW };
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD };

struct View {
  uint8_t mode;
  // conduct
  bool onAir, manual, conflict, bridgeOk; uint32_t sent; float level, conf; int gain; bool micOk; const char* micDiag;
  const float* scope; const bool* scopeOn; int scopePos, scopeN;
  // follow
  uint8_t link; bool viaBooth; int rssi; uint32_t lost;
  // both
  float bpm; int beatPos; uint8_t hue; uint32_t flashMs; bool flashDown; int battery; bool toastGain;
};

void hue8(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6, q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}

constexpr int SW = 135, SH = 240, SCX = 67;
static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
uint16_t hueCol(uint8_t h, int lvl) { uint8_t r, g, b; hue8(h, r, g, b); return c565(r * lvl / 255, g * lvl / 255, b * lvl / 255); }

uint16_t COL_GRN, COL_AMB, COL_RED, COL_GREY, COL_DIM, COL_WHITE, COL_PURP, COL_PANEL;
void initColors() {
  COL_GRN = c565(40, 230, 110); COL_AMB = c565(255, 176, 0); COL_RED = c565(255, 50, 60);
  COL_GREY = c565(140, 140, 155); COL_DIM = c565(45, 45, 58); COL_WHITE = 0xFFFF; COL_PURP = c565(150, 110, 255);
  COL_PANEL = c565(14, 14, 22);
}

void ctext(const lgfx::IFont* f, const char* s, int y, uint16_t col, float size = 1.0f) {
  cv.setFont(f); cv.setTextSize(size); cv.setTextDatum(middle_center); cv.setTextColor(col); cv.drawString(s, SCX, y);
  cv.setTextSize(1);
}
void spill(const char* s, int y, uint16_t col, bool filled) {
  cv.setFont(&fonts::FreeSansBold9pt7b);
  int w = cv.textWidth(s) + 18; if (w > SW - 8) w = SW - 8;
  if (filled) cv.fillRoundRect(SCX - w / 2, y - 11, w, 22, 11, col);
  else cv.drawRoundRect(SCX - w / 2, y - 11, w, 22, 11, col);
  cv.setTextDatum(middle_center); cv.setTextColor(filled ? 0x0000 : col); cv.drawString(s, SCX, y + 1);
}
void battery(int pct, int x, int y) {
  if (pct < 0) return;
  uint16_t c = pct <= 15 ? COL_RED : COL_GREY;
  cv.drawRect(x, y, 20, 10, c); cv.fillRect(x + 20, y + 3, 2, 4, c);
  cv.fillRect(x + 2, y + 2, 16 * (pct > 100 ? 100 : pct) / 100, 6, c);
}

void drawFrame(const View& v, uint32_t now) {
  int lvl = 0;
  if (v.flashMs && (int32_t)(now - v.flashMs) >= 0) { lvl = 255 - (int)(now - v.flashMs); if (lvl < 0) lvl = 0; }
  bool live = v.mode == MODE_CONDUCT ? v.bpm > 30 : (v.link != LS_WAIT);

  // badge flash: the whole stage lights up in the beat colour, downbeats brighter
  cv.fillSprite(0x0000);
  if (live && lvl > 0) cv.fillRect(0, 26, SW, 150, hueCol(v.hue, (v.flashDown ? lvl : lvl * 45 / 100) / 3));

  // header
  cv.setFont(&fonts::FreeSansBold9pt7b); cv.setTextDatum(middle_left);
  cv.setTextColor(v.mode == MODE_CONDUCT ? COL_PURP : COL_GREY);
  cv.drawString(v.mode == MODE_CONDUCT ? "CONDUCT" : "FOLLOW", 4, 12);
  battery(v.battery, SW - 25, 7);

  // status pill
  if (v.mode == MODE_CONDUCT) {
    if (v.conflict) spill("CLASH", 42, COL_RED, false);
    else if (v.onAir) spill(v.bridgeOk ? "ON AIR" : "NO LINK", 42, COL_RED, v.bridgeOk);
    else spill("LOCAL", 42, COL_GREY, false);
  } else {
    const char* st = "WAIT"; uint16_t sc = COL_GREY; bool f = false;
    if (v.link == LS_LIVE) { st = v.viaBooth ? "BOOTH" : "LIVE"; sc = v.viaBooth ? COL_AMB : COL_GRN; f = true; }
    else if (v.link == LS_HOLD) { st = "HOLD"; sc = COL_AMB; f = true; }
    if (!v.bridgeOk) { st = "NO LINK"; sc = COL_RED; f = false; }
    spill(st, 42, sc, f);
  }

  // BPM: whole number big, tenths small
  char whole[8], tenth[4];
  if (v.bpm > 30) { int t = (int)(v.bpm * 10 + 0.5f); snprintf(whole, sizeof(whole), "%d", t / 10); snprintf(tenth, sizeof(tenth), ".%d", t % 10); }
  else { strcpy(whole, "---"); tenth[0] = 0; }
  uint16_t bc = live ? (lvl > 140 ? hueCol(v.hue, 255) : COL_WHITE) : COL_GREY;
  cv.setFont(&fonts::Font7); cv.setTextDatum(middle_center); cv.setTextColor(bc);
  int ww = cv.textWidth(whole);
  cv.setFont(&fonts::FreeSansBold12pt7b); int tw = cv.textWidth(tenth);
  int x0 = SCX - (ww + tw) / 2;
  cv.setFont(&fonts::Font7); cv.setTextDatum(middle_left); cv.drawString(whole, x0, 92);
  cv.setFont(&fonts::FreeSansBold12pt7b); cv.setTextDatum(baseline_left); cv.drawString(tenth, x0 + ww + 1, 114);
  const char* src = "BPM";
  if (v.mode == MODE_CONDUCT) src = !v.micOk ? "MIC FAILED" : (v.manual ? "BPM  TAP" : "BPM  MIC");
  ctext(&fonts::FreeSans9pt7b, src, 134, v.mode == MODE_CONDUCT && !v.micOk ? COL_RED : COL_GREY);

  // beat pips
  for (int i = 0; i < 4; i++) {
    int x = SCX - 36 + i * 24, y = 158;
    bool on = live && i == v.beatPos;
    if (on) cv.fillCircle(x, y, i == 0 ? 8 : 6, hueCol(v.hue, 255));
    else cv.drawCircle(x, y, i == 0 ? 8 : 6, COL_DIM);
  }

  // lower panel
  cv.fillRect(0, 178, SW, SH - 178, COL_PANEL);
  if (v.mode == MODE_CONDUCT) {
    if (!v.micOk) {                                     // mic diagnostics where the scope would be
      cv.setFont(&fonts::Font0); cv.setTextColor(COL_RED); cv.setTextDatum(top_left);
      const char* d = v.micDiag ? v.micDiag : ""; int len = strlen(d), y = 182;
      for (int k = 0; k < len && y < 226; k += 21, y += 10) { char ln2[22]; snprintf(ln2, sizeof(ln2), "%.21s", d + k); cv.drawString(ln2, 4, y); }
    }
    // onset scope + level meter
    int base = 214;
    for (int i = 0; i < v.scopeN && i < SW; i++) {
      int k = (v.scopePos + i) % v.scopeN;
      int h = (int)(v.scope[k] * 30);
      if (h > 0) cv.drawFastVLine(i, base - h, h, v.scopeOn[k] ? hueCol(v.hue, 255) : COL_DIM);
    }
    float lv = v.level * 6; if (lv > 1) lv = 1;
    cv.fillRect(4, 220, (int)((SW - 8) * lv), 5, lv > 0.9f ? COL_AMB : COL_GRN);
    cv.drawRect(3, 219, SW - 6, 7, COL_DIM);
    char ln[24];
    const char* path = v.micDiag ? v.micDiag : "";
    if (!v.micOk) snprintf(ln, sizeof(ln), "send me the red text");
    else if (v.onAir) snprintf(ln, sizeof(ln), "SENT %lu  G%d %s", (unsigned long)v.sent, v.gain, path);
    else snprintf(ln, sizeof(ln), "MIC GAIN %d  %s", v.gain, path);
    cv.setFont(&fonts::Font2); cv.setTextDatum(middle_center); cv.setTextColor(v.micOk ? COL_GREY : COL_RED); cv.drawString(ln, SCX, 233);
  } else {
    char l1[24], l2[24];
    if (!v.bridgeOk) { strcpy(l1, "no bridge"); strcpy(l2, "heard"); }
    else if (v.viaBooth) { strcpy(l1, "booth has"); strcpy(l2, "the beat"); }
    else if (v.link != LS_WAIT) { snprintf(l1, sizeof(l1), "%d dBm", v.rssi); snprintf(l2, sizeof(l2), "lost %lu", (unsigned long)v.lost); }
    else { strcpy(l1, "waiting for"); strcpy(l2, "a beat"); }
    ctext(&fonts::FreeSansBold9pt7b, l1, 198, COL_GREY);
    ctext(&fonts::FreeSansBold9pt7b, l2, 222, COL_GREY);
  }

  if (v.toastGain) {
    cv.fillRoundRect(10, 70, SW - 20, 60, 10, 0x0000); cv.drawRoundRect(10, 70, SW - 20, 60, 10, COL_PURP);
    char g[16]; snprintf(g, sizeof(g), "GAIN %d", v.gain);
    ctext(&fonts::FreeSansBold12pt7b, g, 92, COL_PURP);
    static const char* name[4] = {"club", "loud room", "quiet room", "max"};
    ctext(&fonts::FreeSans9pt7b, name[v.gain & 3], 116, COL_WHITE);
  }
}

void splash() {
  cv.fillSprite(0x0000);
  for (int i = 0; i < 4; i++) cv.fillRoundRect(14 + i * 28, 70, 23, 8, 4, hueCol(160 + i * 40, 220));
  ctext(&fonts::Orbitron_Light_24, "POCKET", 108, COL_WHITE);
  ctext(&fonts::Orbitron_Light_24, "CONDUCTOR", 136, COL_WHITE, 0.62f);
  ctext(&fonts::FreeSansBold9pt7b, "ANUBIS", 172, COL_PURP);
  ctext(&fonts::FreeSansBold9pt7b, "DARKWATCH", 192, COL_PURP);
  ctext(&fonts::Font2, FW_VERSION, 222, COL_GREY);
}
