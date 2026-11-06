// Beat Dial screens: everything drawn into the 240x240 frame buffer `cv`.
// Shared by the firmware and tools/preview_dial.cpp (host preview), so it uses only LovyanGFX / M5GFX calls.
// Before including: declare `cv` (an LGFX_Sprite / M5Canvas), `bool lockOn`, and FW_VERSION.
#pragma once

enum Mode : uint8_t { M_FOLLOW, M_TAKE };
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE };
enum Toast : uint8_t { T_NONE, T_SCAN, T_DENIED, T_GRANTED, T_TURN };

struct View {
  uint8_t mode, link, toast; bool bridgeOk, radioOk, conflict, viaBooth, flashDown;
  float bpm; int beatPos, rssi, nudgeMs; float snr; uint8_t hue;
  uint32_t rx, lost, sent, fail, bars, flashMs;
};


void hue8(uint8_t h, uint8_t& r, uint8_t& g, uint8_t& b) {
  uint8_t reg = h / 43, rem = (h - reg * 43) * 6, q = 255 - rem, t = rem;
  switch (reg) { case 0: r = 255; g = t; b = 0; break; case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break; case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break; default: r = 255; g = 0; b = q; }
}

constexpr int W = 240, CX = 120, CY = 120;

static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
uint16_t hueCol(uint8_t h, int lvl) { uint8_t r, g, b; hue8(h, r, g, b); return c565(r * lvl / 255, g * lvl / 255, b * lvl / 255); }

const uint16_t COL_BG = 0x0000;
uint16_t COL_GRN, COL_AMB, COL_RED, COL_GREY, COL_DIM, COL_WHITE, COL_PURP;
void initColors() {
  COL_GRN = c565(40, 230, 110); COL_AMB = c565(255, 176, 0); COL_RED = c565(255, 50, 60);
  COL_GREY = c565(140, 140, 155); COL_DIM = c565(45, 45, 58); COL_WHITE = 0xFFFF; COL_PURP = c565(150, 110, 255);
}

void centerText(const lgfx::IFont* f, const char* s, int y, uint16_t col, float size = 1.0f) {
  cv.setFont(f); cv.setTextSize(size); cv.setTextDatum(middle_center); cv.setTextColor(col); cv.drawString(s, CX, y);
  cv.setTextSize(1);
}
void pill(const char* s, int y, uint16_t col, bool filled) {
  cv.setFont(&fonts::FreeSansBold9pt7b);
  int w = cv.textWidth(s) + 20;
  if (filled) cv.fillRoundRect(CX - w / 2, y - 11, w, 22, 11, col);
  else cv.drawRoundRect(CX - w / 2, y - 11, w, 22, 11, col);
  cv.setTextDatum(middle_center); cv.setTextColor(filled ? COL_BG : col); cv.drawString(s, CX, y + 1);
}

// lock glyph (closed / open)
void lockIcon(int x, int y, uint16_t col, bool open) {
  cv.fillRoundRect(x - 7, y - 2, 14, 11, 2, col);
  int ox = open ? 6 : 0;
  cv.drawArc(x + ox, y - 3, 6, 4, 180, 360, col);
  cv.fillRect(x - 6 + ox, y - 3, 2, 3, col); if (!open) cv.fillRect(x + 4, y - 3, 2, 3, col);
}

void drawFrame(const View& v, uint32_t now) {
  cv.fillSprite(COL_BG);

  // flash level from the latest beat (decays over ~300 ms)
  int lvl = 0;
  if (v.flashMs && (int32_t)(now - v.flashMs) >= 0) { lvl = 255 - (int)(now - v.flashMs); if (lvl < 0) lvl = 0; }
  bool live = v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE;

  // background glow on the downbeat
  if (live && v.flashDown && lvl > 0) cv.fillCircle(CX, CY, 104, hueCol(v.hue, lvl / 6));

  // beat ring: 4 segments, top = beat 1
  for (int i = 0; i < 4; i++) {
    float a0 = -90 + i * 90 + 4, a1 = a0 + 82;
    bool cur = live && i == v.beatPos;
    int r0 = (i == 0) ? 104 : 108;
    uint16_t col;
    if (!live) col = COL_DIM;
    else if (cur) col = hueCol(v.hue, 90 + lvl * 165 / 255);
    else col = hueCol(v.hue, 38);
    cv.fillArc(CX, CY, r0, 119, a0, a1, col);
  }

  // status pill
  const char* st = "WAIT"; uint16_t sc = COL_GREY; bool filled = false;
  switch (v.link) {
    case LS_LIVE: st = v.viaBooth ? "BOOTH" : "LIVE"; sc = v.viaBooth ? COL_AMB : COL_GRN; filled = true; break;
    case LS_HOLD: st = "HOLD"; sc = COL_AMB; filled = true; break;
    case LS_TAKE: st = "TAKE"; sc = COL_RED; filled = true; break;
  }
  if (!v.bridgeOk) { st = "NO LINK"; sc = COL_RED; filled = false; }
  if (v.conflict) {                                       // another transmitter is on the beat link
    pill("TAKE", 52, COL_RED, true);
    cv.fillRoundRect(40, 62, 160, 20, 10, COL_RED);
    centerText(&fonts::FreeSansBold9pt7b, "W10 ON AIR TOO", 73, COL_WHITE);
  } else pill(st, 52, sc, filled);

  // BPM
  char b[12];
  if (v.bpm > 30) snprintf(b, sizeof(b), "%.1f", v.bpm); else strcpy(b, "---.-");
  uint16_t bc = (v.link == LS_WAIT) ? COL_GREY : COL_WHITE;
  if (live && lvl > 120) bc = hueCol(v.hue, 255);
  cv.setFont(&fonts::Font7); cv.setTextDatum(middle_center); cv.setTextColor(bc); cv.drawString(b, CX, 108);
  centerText(&fonts::Orbitron_Light_24, "BPM", 146, COL_GREY, 0.6f);

  // beat pips
  for (int i = 0; i < 4; i++) {
    int x = CX - 27 + i * 18, y = 166;
    bool on = live && i == v.beatPos;
    if (on) cv.fillCircle(x, y, i == 0 ? 6 : 5, hueCol(v.hue, 255));
    else cv.drawCircle(x, y, i == 0 ? 6 : 5, COL_DIM);
  }

  // bottom line
  char line[32]; uint16_t lc = COL_GREY;
  if (v.mode == M_TAKE) {
    if (v.nudgeMs) snprintf(line, sizeof(line), "NUDGE %+d ms", v.nudgeMs);
    else snprintf(line, sizeof(line), "SENT %lu%s", (unsigned long)v.sent, v.fail ? "  !" : "");
    if (v.fail) lc = COL_AMB;
  } else if (!v.bridgeOk) snprintf(line, sizeof(line), "BRIDGE?");
  else if (v.rx && !v.viaBooth) snprintf(line, sizeof(line), "%d dBm  LOST %lu", v.rssi, (unsigned long)v.lost);
  else if (v.rx) snprintf(line, sizeof(line), "RX %lu", (unsigned long)v.rx);
  else snprintf(line, sizeof(line), v.radioOk ? "LISTENING" : "RADIO?");
  centerText(&fonts::FreeSansBold9pt7b, line, 192, lc);

  // lock state
  if (v.mode == M_FOLLOW) lockIcon(CX, 26, lockOn ? COL_GREY : COL_DIM, !lockOn);

  // toasts
  if (v.toast != T_NONE) {
    uint16_t tc = COL_PURP; const char* t1 = ""; const char* t2 = "";
    switch (v.toast) {
      case T_SCAN:    tc = COL_PURP; t1 = "SCAN IMPLANT"; t2 = "to take the beat"; break;
      case T_DENIED:  tc = COL_RED;  t1 = "DENIED"; t2 = "tag not enrolled"; break;
      case T_GRANTED: tc = COL_GRN;  t1 = "TAKEN"; t2 = "booth has the beat"; break;
      case T_TURN:    tc = COL_PURP; t1 = lockOn ? "LOCKED" : "FOLLOWING"; t2 = lockOn ? "scan to take" : "press to take"; break;
    }
    cv.fillRoundRect(16, 84, 208, 72, 14, COL_BG);
    cv.drawRoundRect(16, 84, 208, 72, 14, tc);
    cv.drawRoundRect(17, 85, 206, 70, 13, tc);
    centerText(&fonts::Orbitron_Light_24, t1, 108, tc, strlen(t1) > 10 ? 0.82f : 1.0f);
    centerText(&fonts::FreeSansBold9pt7b, t2, 136, COL_WHITE);
  }
}

// full-screen message pages (boot / enroll)
void page(const char* title, uint16_t tc, const char* l1, const char* l2, const char* l3 = "") {
  cv.fillSprite(COL_BG);
  cv.drawCircle(CX, CY, 118, tc); cv.drawCircle(CX, CY, 117, tc);
  centerText(&fonts::Orbitron_Light_24, title, 78, tc);
  centerText(&fonts::FreeSansBold9pt7b, l1, 116, COL_WHITE);
  centerText(&fonts::FreeSansBold9pt7b, l2, 140, COL_GREY);
  centerText(&fonts::FreeSansBold9pt7b, l3, 164, COL_GREY);
}

void splash() {
  cv.fillSprite(COL_BG);
  for (int i = 0; i < 4; i++) cv.fillArc(CX, CY, 108, 119, -90 + i * 90 + 4, -90 + i * 90 + 86, hueCol(160 + i * 40, 200));
  centerText(&fonts::Orbitron_Light_24, "BEAT DIAL", 104, COL_WHITE);
  centerText(&fonts::FreeSansBold9pt7b, "ANUBIS DARKWATCH", 136, COL_PURP);
  centerText(&fonts::FreeSansBold9pt7b, FW_VERSION, 160, COL_GREY);
}

