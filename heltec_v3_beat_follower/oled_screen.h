// Heltec V3 OLED runtime for the beat screen (oled_viz.h): finds the OLED, runs it from its own task on core 0 so
// I2C never delays the radio, flashes the panel (hardware invert) within a couple of ms of each beat, and handles
// the PRG button: press = next screen, hold 1 s = AUTO (new visual every 8 bars). Mode and AUTO are saved.
// Keep this file identical in heltec_v3_beat_follower/ and heltec_v3_espnow_bridge/.
#pragma once
#include <Wire.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "oled_viz.h"

#define OLED_SDA 17
#define OLED_SCL 18
#define OLED_RST 21
#define OLED_VEXT 36           // LOW = OLED power on
#define OLED_PRG 0             // PRG button

static LGFX_Sprite oCv;
static OState oState = {};
static portMUX_TYPE oMux = portMUX_INITIALIZER_UNLOCKED;   // ints only inside: no floats in critical sections
static uint8_t oAddr = 0;
static Preferences oPrefs;
bool screenOk = false;

static void oCmd(uint8_t c) { Wire.beginTransmission(oAddr); Wire.write(0x00); Wire.write(c); Wire.endTransmission(); }

static bool oFind() {
  pinMode(OLED_VEXT, OUTPUT); pinMode(OLED_RST, OUTPUT);
  Wire.begin(OLED_SDA, OLED_SCL, 400000);
  for (int vx = 0; vx < 2; vx++) {                     // Vext LOW = on (V3); try HIGH too
    digitalWrite(OLED_VEXT, vx ? HIGH : LOW); delay(150);
    digitalWrite(OLED_RST, LOW); delay(20); digitalWrite(OLED_RST, HIGH); delay(50);
    for (uint8_t a : {0x3C, 0x3D}) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { oAddr = a; return true; } }
  }
  return false;
}

static void oFlashCheck(uint32_t& lastId, bool& inv, uint32_t& invAt) {
  portENTER_CRITICAL(&oMux);
  uint32_t id = oState.beatId; uint8_t pos = oState.beatPos, mode = oState.mode, link = oState.link;
  portEXIT_CRITICAL(&oMux);
  uint32_t now = millis();
  if (id != lastId) {
    lastId = id;
    if (link != OL_WAIT && mode != O_STATS && (mode == O_BEAT || pos == 0)) { oCmd(0xA7); inv = true; invAt = now; }
  }
  if (inv && now - invAt > 70) { oCmd(0xA6); inv = false; }
}

static void oTask(void*) {
  uint8_t pg[OW], chunk[33];
  uint32_t lastId = 0, invAt = 0; bool inv = false;
  for (;;) {
    OState v;
    portENTER_CRITICAL(&oMux); v = oState; portEXIT_CRITICAL(&oMux);
    uint32_t now = millis();
    if (v.link == OL_WAIT && v.mode != O_STATS && v.mode != O_BEAT) {   // idle visuals at 120 BPM
      v.bpm10 = 1200; v.beatId = now / 500 + 1000000; v.beatMs = now / 500 * 500; v.beatPos = (now / 500) & 3;
    }
    oledDraw(oCv, v, now);
    oCmd(0x21); oCmd(0); oCmd(127); oCmd(0x22); oCmd(0); oCmd(7);
    for (int p = 0; p < 8; p++) {
      oledPage(oCv, p, pg);
      for (int x = 0; x < OW; x += 32) {
        oFlashCheck(lastId, inv, invAt);                // between chunks: the flash is never more than ~1 ms late
        chunk[0] = 0x40; memcpy(chunk + 1, pg + x, 32);
        Wire.beginTransmission(oAddr); Wire.write(chunk, 33); Wire.endTransmission();
      }
    }
    oFlashCheck(lastId, inv, invAt);
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// call once in setup(); returns false (and powers the OLED rail off) if no screen answers
bool screenBegin(bool bridge) {
  oState.bridge = bridge;
  oPrefs.begin("beatscr", false);
  oState.mode = oPrefs.getUChar("mode", O_BEAT); if (oState.mode >= O_COUNT) oState.mode = O_BEAT;
  oState.autoMode = oPrefs.getBool("auto", false);
  pinMode(OLED_PRG, INPUT_PULLUP);
  if (!oFind()) { digitalWrite(OLED_VEXT, HIGH); return false; }
  Wire.setClock(700000);
  const uint8_t init[] = {0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00,
                          0xA1, 0xC8, 0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF};
  for (uint8_t c : init) oCmd(c);
  oledInitFonts();
  oCv.setColorDepth(1);
  if (!oCv.createSprite(OW, OH)) return false;
  oState.labelUntil = millis() + 1500;
  xTaskCreatePinnedToCore(oTask, "oled", 6144, nullptr, 1, nullptr, 0);
  screenOk = true;
  return true;
}

// a beat happened (real or flywheel) at time ms
void screenBeat(uint16_t bpm10, uint8_t pos, uint32_t ms) {
  portENTER_CRITICAL(&oMux);
  oState.bpm10 = bpm10; oState.beatPos = pos & 3; oState.beatMs = ms; oState.beatId++;
  if (oState.beatPos == 0) {
    oState.bars++;
    if (oState.autoMode && oState.mode < O_STATS && oState.bars % 8 == 0) {
      oState.mode = (oState.mode + 1) % O_STATS; oState.labelUntil = ms + 1500;
    }
  }
  portEXIT_CRITICAL(&oMux);
}

void screenLink(uint8_t link) { portENTER_CRITICAL(&oMux); oState.link = link; portEXIT_CRITICAL(&oMux); }

void screenStats(int rssi, int8_t snr4, uint32_t pkts, uint32_t lost, uint32_t sent, uint32_t clients) {
  portENTER_CRITICAL(&oMux);
  oState.rssi = rssi; oState.snr4 = snr4; oState.pkts = pkts; oState.lost = lost; oState.sent = sent; oState.clients = clients;
  portEXIT_CRITICAL(&oMux);
}

// call from loop(): PRG press = next screen, hold 1 s = AUTO on/off
void screenButton() {
  static bool down = false, held = false, armed = false; static uint32_t t0 = 0, upSince = 0;
  bool d = digitalRead(OLED_PRG) == LOW; uint32_t now = millis();
  if (!d) { if (!upSince) upSince = now; if (now - upSince > 300) armed = true; } else upSince = 0;
  if (!armed) return;                                  // only once it has read released (no loops if GPIO0 sticks low)
  if (d && !down) { down = true; held = false; t0 = now; }
  else if (d && down && !held && now - t0 > 1000) {
    held = true;
    portENTER_CRITICAL(&oMux);
    oState.autoMode = !oState.autoMode; if (oState.mode >= O_STATS) oState.mode = O_PULSE;
    oState.labelUntil = now + 1500;
    portEXIT_CRITICAL(&oMux);
    oPrefs.putBool("auto", oState.autoMode); oPrefs.putUChar("mode", oState.mode);
  } else if (!d && down) {
    down = false;
    if (!held && now - t0 > 30) {
      portENTER_CRITICAL(&oMux);
      oState.mode = (oState.mode + 1) % O_COUNT; oState.labelUntil = now + 1500;
      uint8_t m = oState.mode;
      portEXIT_CRITICAL(&oMux);
      oPrefs.putUChar("mode", m);
    }
  }
}
