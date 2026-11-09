// CYD Beat Console: screen layout and drawing (plain C++, also used by the PC preview tool).
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include "gfx.h"

#define FW_VERSION "v1.0.5"

enum Mode : uint8_t { M_FOLLOW, M_TAKE };
enum LinkState : uint8_t { LS_WAIT, LS_LIVE, LS_HOLD, LS_TAKE };

struct UiState {
  uint8_t mode = M_FOLLOW, link = LS_WAIT;
  bool bridgeOk = false, radioOk = false, conflict = false;
  float bpm = 0; int beatPos = 0; uint8_t hue = 160;
  float flash = 0; bool downbeat = false;      // flash 0..1, decays after each beat
  int rssi = 0; float snr = 0;
  uint32_t rx = 0, lost = 0, sent = 0, fail = 0;
  int nudgeMs = 0;                              // total phase nudge this takeover
  int pressed = -1;                             // button being held
  bool viaBooth = false;                        // following a booth controller rather than a LoRa conductor
};

// ---------- layout ----------
#define Y_HEAD 0
#define H_HEAD 28
#define Y_STAGE 28
#define H_STAGE 100       // flash area
#define Y_BAR 128
#define H_BAR 20
#define Y_STATS 148
#define H_STATS 36
#define Y_BTNS 184
#define H_BTNS 56

enum Btn { B_TAP, B_MINUS, B_PLUS, B_LEFT, B_RIGHT, B_TAKE, B_COUNT };
struct BtnRect { int16_t x, w; };
static const BtnRect BTN[B_COUNT] = {{2, 78}, {82, 42}, {126, 42}, {170, 42}, {214, 42}, {258, 60}};
#define BTN_Y (Y_BTNS + 3)
#define BTN_H (H_BTNS - 6)

static int hitButton(int x, int y) {
  if (y < Y_BTNS - 6) return -1;                // a little slack above the row
  for (int i = 0; i < B_COUNT; i++) if (x >= BTN[i].x - 1 && x < BTN[i].x + BTN[i].w + 1) return i;
  return -1;
}
static bool btnEnabled(const UiState& s, int b) {
  if (b == B_TAP || b == B_TAKE) return true;
  return s.mode == M_TAKE;
}

// ---------- header ----------
static void drawHeader(const UiState& s) {
  fillRect(0, Y_HEAD, SCR_W, H_HEAD, C_BLACK);
  alpha4(JACKAL_S, JACKAL_S_W, JACKAL_S_H, 5, 2, C_AMB);
  text(ORB_14, "BEAT CONSOLE", 33, 19, C_AMB);
  text(STM_12, "916.0", 172, 18, C_DIM);
  const char* t; uint16_t c; bool solid = true;
  if (!s.bridgeOk) { t = "NO LINK"; c = C_RED; solid = false; }
  else switch (s.link) {
    case LS_LIVE: t = "LIVE"; c = C_GRN; break;
    case LS_HOLD: t = "HOLD"; c = C_AMB; break;
    case LS_TAKE: t = "TAKE"; c = C_RED; break;
    default:      t = "WAIT"; c = C_GREY; solid = false;
  }
  int w = textW(ORB_14, t) + 14, x = SCR_W - 4 - w;
  if (solid) { fillRect(x, 5, w, 17, c); textC(ORB_14, t, x + w / 2, 19, C_BLACK); }
  else { rect(x, 5, w, 17, c); textC(ORB_14, t, x + w / 2, 19, c); }
  hline(0, H_HEAD - 1, SCR_W, C_DIM);
  hline(0, H_HEAD - 1, 18, C_AMB); hline(SCR_W - 18, H_HEAD - 1, 18, C_AMB);
}

// ---------- stage (flash area) ----------
static void drawStage(const UiState& s) {
  if (s.link == LS_WAIT && s.mode == M_FOLLOW) {
    fillRect(0, Y_STAGE, SCR_W, H_STAGE, C_BLACK);
    alpha4(JACKAL_L, JACKAL_L_W, JACKAL_L_H, 22, Y_STAGE + 8, scale(C_AMB, 210));
    text(ORB_20, "LISTENING", 128, Y_STAGE + 44, C_AMB);
    text(STM_16, "beat link 916.0 MHz", 128, Y_STAGE + 66, C_GRN);
    text(STM_12, "SF7 / BW500 via bridge", 128, Y_STAGE + 84, C_DIM);
    scanlines(0, Y_STAGE, SCR_W, H_STAGE, 200);
    return;
  }
  float lv = s.flash * (s.downbeat ? 1.0f : 0.6f);
  uint16_t bg = hsv(s.hue, 0.9f, 0.05f + 0.95f * lv);
  bool bright = lv > 0.45f;
  uint16_t fg = bright ? C_BLACK : C_WHITE;
  fillRect(0, Y_STAGE, SCR_W, H_STAGE, bg);
  scanlines(0, Y_STAGE, SCR_W, H_STAGE, 215);

  char ib[12], db[12];
  int b10 = (int)(s.bpm * 10 + 0.5f);
  snprintf(ib, sizeof(ib), "%d", b10 / 10); snprintf(db, sizeof(db), ".%d", b10 % 10);
  int x = text(ORB_BIG, ib, 10, Y_STAGE + 84, fg);
  text(ORB_20, db, x + 2, Y_STAGE + 84, fg);
  text(ORB_14, "BPM", x + 4, Y_STAGE + 60, bright ? C_BLACK : C_AMB);

  // source, top right
  const char* src = s.mode == M_TAKE ? "CONSOLE" : s.link == LS_HOLD ? "FLYWHEEL" : s.viaBooth ? "BOOTH" : "CONDUCTOR";
  textR(STM_12, "SOURCE", SCR_W - 10, Y_STAGE + 16, bright ? C_BLACK : C_DIM);
  textR(ORB_14, src, SCR_W - 10, Y_STAGE + 33, bright ? C_BLACK : (s.mode == M_TAKE ? C_RED : C_GRN));
  brackets(4, Y_STAGE + 4, SCR_W - 8, H_STAGE - 8, 10, bright ? C_BLACK : C_DIM);

  if (s.conflict) {
    fillRect(0, Y_STAGE + H_STAGE - 20, SCR_W, 20, C_RED);
    textC(STM_12, "ANOTHER CONDUCTOR IS STILL ON AIR", SCR_W / 2, Y_STAGE + H_STAGE - 6, C_BLACK);
  }
}

// ---------- bar counter ----------
static void drawBar(const UiState& s) {
  fillRect(0, Y_BAR, SCR_W, H_BAR, C_BLACK);
  bool active = !(s.link == LS_WAIT && s.mode == M_FOLLOW);
  for (int i = 0; i < 4; i++) {
    int x = 10 + i * 76, w = 72, y = Y_BAR + 4, h = 12;
    uint16_t c = i == 0 ? C_AMB : C_GRN;
    if (active && i == s.beatPos) fillRect(x, y, w, h, c);
    else rect(x, y, w, h, scale(c, 70));
  }
}

// ---------- stats ----------
static uint16_t rssiColor(int r) { return r >= -90 ? C_GRN : r >= -110 ? C_AMB : C_RED; }
static void cell(int i, const char* label, const char* val, uint16_t vc) {
  int x = 4 + i * 79;
  brackets(x, Y_STATS + 2, 75, H_STATS - 4, 5, C_DIM);
  text(STM_12, label, x + 6, Y_STATS + 14, C_DIM);
  text(STM_16, val, x + 6, Y_STATS + 29, vc);
}
static void drawStats(const UiState& s) {
  fillRect(0, Y_STATS, SCR_W, H_STATS, C_BLACK);
  char b[16];
  if (s.link == LS_WAIT && s.mode == M_FOLLOW) {
    if (!s.bridgeOk) {
      text(STM_12, "NO BRIDGE: power on the Heltec V3 bridge", 8, Y_STATS + 15, C_RED);
      text(STM_12, "(same LINK_NET_ID and ESP-NOW channel)", 8, Y_STATS + 30, C_DIM);
    } else {
      text(STM_12, s.radioOk ? "BRIDGE OK  //  LoRa radio OK" : "BRIDGE OK  //  LoRa radio FAILED", 8, Y_STATS + 15,
           s.radioOk ? C_GRN : C_RED);
      text(STM_12, "Start a conductor: W10 or StickS3 ON AIR", 8, Y_STATS + 30, C_AMB);
    }
    return;
  }
  if (s.mode == M_TAKE) {
    snprintf(b, sizeof(b), "%lu", (unsigned long)s.sent); cell(0, "SENT", b, C_GRN);
    snprintf(b, sizeof(b), "%lu", (unsigned long)s.fail); cell(1, "FAIL", b, s.fail ? C_RED : C_GRN);
    snprintf(b, sizeof(b), "%+d ms", s.nudgeMs); cell(2, "NUDGE", b, C_AMB);
    snprintf(b, sizeof(b), "%d", s.rssi); cell(3, "RSSI", s.rx ? b : "--", s.rx ? rssiColor(s.rssi) : C_GREY);
  } else {
    snprintf(b, sizeof(b), "%d", s.rssi); cell(0, "RSSI dBm", b, rssiColor(s.rssi));
    snprintf(b, sizeof(b), "%+.1f", s.snr); cell(1, "SNR dB", b, s.snr >= 0 ? C_GRN : C_AMB);
    snprintf(b, sizeof(b), "%lu", (unsigned long)s.rx); cell(2, "RX", b, C_GRN);
    snprintf(b, sizeof(b), "%lu", (unsigned long)s.lost); cell(3, "LOST", b, s.lost ? C_AMB : C_GRN);
  }
}

// ---------- buttons ----------
static void tri(int cx, int cy, int s, bool left, uint16_t c) {
  for (int dy = -s; dy <= s; dy++) {
    int len = s - abs(dy);
    if (left) hline(cx + s / 2 - len, cy + dy, len, c);
    else hline(cx - s / 2, cy + dy, len, c);
  }
}
static void drawButtons(const UiState& s) {
  fillRect(0, Y_BTNS, SCR_W, H_BTNS, C_BLACK);
  for (int i = 0; i < B_COUNT; i++) {
    int x = BTN[i].x, w = BTN[i].w, cx = x + w / 2, y = BTN_Y, h = BTN_H;
    bool en = btnEnabled(s, i), down = en && s.pressed == i;
    bool takeOn = i == B_TAKE && s.mode == M_TAKE;
    uint16_t edge = !en ? scale(C_DIM, 120) : takeOn ? C_RED : C_DIM;
    uint16_t fg = !en ? scale(C_GREY, 110) : down ? C_BLACK : takeOn ? C_RED : C_AMB;
    if (down) fillRect(x, y, w, h, takeOn ? C_RED : C_AMB);
    else { fillRect(x, y, w, h, C_PANEL); rect(x, y, w, h, edge); }
    if (en && !down) brackets(x, y, w, h, 6, takeOn ? C_RED : C_AMB);
    int by = y + h / 2;
    switch (i) {
      case B_TAP:   textC(ORB_20, "TAP", cx, by + 4, fg); textC(STM_12, "tempo", cx, by + 19, en && !down ? C_DIM : fg); break;
      case B_MINUS: fillRect(cx - 8, by - 6, 16, 4, fg); textC(STM_12, "0.1", cx, by + 19, fg); break;
      case B_PLUS:  fillRect(cx - 8, by - 6, 16, 4, fg); fillRect(cx - 2, by - 12, 4, 16, fg); textC(STM_12, "0.1", cx, by + 19, fg); break;
      case B_LEFT:  tri(cx, by - 4, 8, true, fg); textC(STM_12, "10ms", cx, by + 19, fg); break;
      case B_RIGHT: tri(cx, by - 4, 8, false, fg); textC(STM_12, "10ms", cx, by + 19, fg); break;
      case B_TAKE:
        if (takeOn) { textC(STM_12, "RELEASE", cx, by - 2, fg); textC(STM_12, "hand back", cx, by + 13, down ? C_BLACK : C_DIM); }
        else { textC(ORB_14, "TAKE", cx, by + 1, fg); textC(STM_12, "over", cx, by + 17, down ? C_BLACK : C_DIM); }
        break;
    }
  }
}

// ---------- full screens ----------
static void drawAll(const UiState& s) {
  paint(Y_HEAD, H_HEAD, [&] { drawHeader(s); });
  paint(Y_STAGE, H_STAGE, [&] { drawStage(s); });
  paint(Y_BAR, H_BAR, [&] { drawBar(s); });
  paint(Y_STATS, H_STATS, [&] { drawStats(s); });
  paint(Y_BTNS, H_BTNS, [&] { drawButtons(s); });
}

// CRT power-on: p 0..1 opens a horizontal slit into the splash
static void drawSplash(float p) {
  int open = (int)(SCR_H * p * p), y0 = SCR_H / 2 - open / 2;
  paint(0, SCR_H, [&] {
    fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
    if (open < 3) { hline(SCR_W / 2 - (int)(150 * p * 3), SCR_H / 2, (int)(300 * p * 3), C_WHITE); return; }
    alpha4(JACKAL_L, JACKAL_L_W, JACKAL_L_H, (SCR_W - JACKAL_L_W) / 2, 30, C_AMB);
    textC(ORB_20, "BEAT CONSOLE", SCR_W / 2, 150, C_AMB);
    textC(STM_12, "ANUBIS DARKWATCH  //  " FW_VERSION, SCR_W / 2, 172, C_GRN);
    textC(STM_12, "touch: calibrate   BOOT: display check", SCR_W / 2, 222, C_DIM);
    brackets(8, 8, SCR_W - 16, SCR_H - 16, 14, C_DIM);
    scanlines(0, 0, SCR_W, SCR_H, 190);
    fillRect(0, 0, SCR_W, y0, C_BLACK);                       // slit mask
    fillRect(0, y0 + open, SCR_W, SCR_H - y0 - open, C_BLACK);
    if (p < 1) { hline(0, y0, SCR_W, C_WHITE); hline(0, y0 + open - 1, SCR_W, C_WHITE); }
  });
}

static void drawCal(int step, int cx, int cy) {
  paint(0, SCR_H, [&] {
    fillRect(0, 0, SCR_W, SCR_H, C_BLACK);
    textC(ORB_14, "TOUCH CALIBRATION", SCR_W / 2, 100, C_AMB);
    static const char* where[3] = {"tap the cross, top left", "now the cross, top right", "last one, bottom left"};
    textC(STM_12, where[step], SCR_W / 2, 122, C_GRN);
    textC(STM_12, "use a stylus or fingernail", SCR_W / 2, 140, C_DIM);
    hline(cx - 10, cy, 21, C_RED); vline(cx, cy - 10, 21, C_RED); rect(cx - 4, cy - 4, 9, 9, C_WHITE);
  });
}
