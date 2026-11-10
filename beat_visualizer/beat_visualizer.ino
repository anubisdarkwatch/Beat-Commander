// Beat Visualizer: beat-synced visuals for colour screens, part of Beat Commander.
// Follows the beat link over ESP-NOW (channel 1, through the Heltec bridge): every beat, the conductor's tempo
// and its colour drive six visual modes: PULSE, TUNNEL, BARS, WARP, PLASMA and BPM. With no link it runs a dim
// 120 BPM demo, so it always looks alive.
//
// Boards (pick one below, or pass -DBOARD_TTGO / -DBOARD_CYD / -DBOARD_CYD32 / -DBOARD_PHANTOM when building)
//   TTGO  LilyGo TTGO T-Display (ESP32, 1.14" 240x135)
//         left button (GPIO0):   press = next mode,   hold = AUTO on/off (new mode every 8 bars)
//         right button (GPIO35): press = brightness,  hold = turn the screen 180°
//   CYD   ESP32-2432S028R "Cheap Yellow Display" (2.8" 320x240), the badge-sized one
//         tap the screen = next mode,  hold 1.5 s = AUTO,  4 s = next colour / mirror / invert setting,  10 s = power off
//         (BOOT press = AUTO, BOOT hold 2 s = next setting, too)
//         (CYD panels differ: if colours or text look wrong, hold BOOT until it looks right). RGB LED flashes too.
//   CYD32 ESP32-2432S032C(-I), the 3.2" IPS CYD with capacitive touch (ST7789, GT911): tap = next mode,
//         hold 1.5 s = AUTO on/off, hold 4 s = next colour / mirror / invert setting, hold 10 s = power off
//         (BOOT works too where it reads right)
//   PHANTOM Rabbit-Labs Phantom (Sunton ESP32-2432S024, 2.4" 320x240 ILI9341), resistive or capacitive touch:
//         same touch gestures as the CYD (tap = next mode, 1.5 s AUTO, 4 s setting, 10 s off). Side button = its own
//         power switch (double-click off). Both touch versions work: it looks for the capacitive chip first.
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x, board "ESP32 Dev Module", partition "Huge APP". Library: LovyanGFX.

#if !defined(BOARD_CYD) && !defined(BOARD_TTGO) && !defined(BOARD_CYD32) && !defined(BOARD_PHANTOM)
#define BOARD_TTGO        // <- or BOARD_CYD (2.8"), BOARD_CYD32 (3.2" IPS), BOARD_PHANTOM (Rabbit-Labs Phantom 2.4")
#endif
#if defined(BOARD_CYD32) || defined(BOARD_PHANTOM)
#define BOARD_CYD         // the 3.2" and the Phantom share everything with the 2.8" CYD except backlight and touch
#endif

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <Wire.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "link.h"
#include "viz.h"

#define FW_VERSION  "1.4"
#define FLYWHEEL_MS 4000

// ================= display =================
class LGFX_Board : public lgfx::LGFX_Device {
#if defined(BOARD_CYD32)
  lgfx::Panel_ST7789 _panel;
#elif defined(BOARD_CYD)
  lgfx::Panel_ILI9341 _panel;
#else
  lgfx::Panel_ST7789 _panel;
#endif
  lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
#ifdef BOARD_PHANTOM
  lgfx::Touch_XPT2046 _touch;
#endif
public:
#ifdef BOARD_PHANTOM
  void addResistiveTouch() {                              // Phantom ...S024R: XPT2046 on the display's own SPI bus, CS 33
    auto t = _touch.config();
    t.spi_host = HSPI_HOST; t.freq = 2500000; t.pin_sclk = 14; t.pin_mosi = 13; t.pin_miso = 12; t.pin_cs = 33;
    t.pin_int = -1; t.bus_shared = true; t.offset_rotation = 0;
    t.x_min = 300; t.x_max = 3900; t.y_min = 200; t.y_max = 3700;
    _touch.config(t); _panel.setTouch(&_touch);
  }
#endif
  void setup(bool bgr) {
    auto b = _bus.config();
#ifdef BOARD_CYD
    b.spi_host = HSPI_HOST; b.freq_write = 40000000; b.pin_sclk = 14; b.pin_mosi = 13; b.pin_miso = 12; b.pin_dc = 2;
#else
    b.spi_host = VSPI_HOST; b.freq_write = 40000000; b.pin_sclk = 18; b.pin_mosi = 19; b.pin_miso = -1; b.pin_dc = 16;
#endif
    b.spi_mode = 0; b.dma_channel = SPI_DMA_CH_AUTO;
    _bus.config(b); _panel.setBus(&_bus);
    auto p = _panel.config();
#if defined(BOARD_CYD32)
    p.pin_cs = 15; p.pin_rst = -1; p.panel_width = 240; p.panel_height = 320; p.invert = true; p.readable = false;
#elif defined(BOARD_CYD)
    p.pin_cs = 15; p.pin_rst = -1; p.panel_width = 240; p.panel_height = 320; p.invert = false; p.readable = true;
#else
    p.pin_cs = 5; p.pin_rst = 23; p.panel_width = 135; p.panel_height = 240; p.offset_x = 52; p.offset_y = 40;
    p.memory_width = 240; p.memory_height = 320; p.invert = true; p.readable = false;
#endif
    p.rgb_order = bgr;
    _panel.config(p);
    auto l = _light.config();
#if defined(BOARD_CYD32) || defined(BOARD_PHANTOM)
    l.pin_bl = 27;
#elif defined(BOARD_CYD)
    l.pin_bl = 21;
#else
    l.pin_bl = 4;
#endif
    l.invert = false; l.freq = 12000; l.pwm_channel = 7;
    _light.config(l); _panel.setLight(&_light);
    setPanel(&_panel);
  }
};
LGFX_Board tft;
LGFX_Sprite cv(&tft);
Preferences prefs;

// ================= inputs =================
struct Btn { uint8_t pin; bool down; uint32_t t0; bool longFired; };
int pollBtn(Btn& b, uint32_t longMs) {                  // 0 none, 1 press (on release), 2 hold (once)
  bool d = digitalRead(b.pin) == LOW; uint32_t now = millis();
  if (d && !b.down) { b.down = true; b.t0 = now; b.longFired = false; }
  else if (d && b.down && !b.longFired && now - b.t0 > longMs) { b.longFired = true; return 2; }
  else if (!d && b.down) { b.down = false; if (!b.longFired && now - b.t0 > 30) return 1; }
  return 0;
}
#if defined(BOARD_CYD32)
Btn bBoot = {0, false, 0, false};
uint8_t gtAddr = 0;                                                 // GT911 capacitive touch on I2C 33/32
bool gtTouched() {                                                  // reads "touch points" from the status register
  if (!gtAddr) return false;
  Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(gtAddr, (uint8_t)1) != 1) return false;
  uint8_t st = Wire.read();
  if (st & 0x80) { Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission(); }
  return (st & 0x80) && (st & 0x0F);
}
void gtInit() {
  pinMode(25, OUTPUT); pinMode(21, OUTPUT);                         // reset with INT low -> address 0x5D
  digitalWrite(25, LOW); digitalWrite(21, LOW); delay(10); digitalWrite(25, HIGH); delay(10);
  pinMode(21, INPUT); delay(50);
  Wire.begin(33, 32, 400000);
  for (uint8_t a : {0x5D, 0x14}) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { gtAddr = a; break; } }
  Serial.printf("# GT911 touch %s\n", gtAddr ? "found" : "not found");
}
bool rawTouch() { return gtTouched(); }
const uint8_t LED_R = 4, LED_G = 16, LED_B = 17;                    // RGB LED, active low
#elif defined(BOARD_PHANTOM)
Btn bBoot = {0, false, 0, false};
uint8_t capAddr = 0;                                                 // CST820 (capacitive ...S024C) on I2C 33/32, reset 25
bool capInit() {
  pinMode(25, OUTPUT); digitalWrite(25, LOW); delay(10); digitalWrite(25, HIGH); delay(60);
  Wire.begin(33, 32, 400000);
  Wire.beginTransmission(0x15); if (Wire.endTransmission() == 0) capAddr = 0x15;
  if (!capAddr) Wire.end();                                          // resistive board: 33 is the touch chip select
  return capAddr != 0;
}
bool rawTouch() {
  if (capAddr) {                                                     // register 0x02: number of fingers
    Wire.beginTransmission(capAddr); Wire.write(0x02);
    if (Wire.endTransmission(false) != 0 || Wire.requestFrom(capAddr, (uint8_t)1) != 1) return false;
    return (Wire.read() & 0x0F) != 0;
  }
  lgfx::touch_point_t tp; return tft.getTouchRaw(&tp, 1) > 0;      // XPT2046: only reports above its pressure threshold
}
const uint8_t LED_R = 4, LED_G = 16, LED_B = 17;                    // RGB LED, active low
#elif defined(BOARD_CYD)
Btn bBoot = {0, false, 0, false};
bool rawTouch() { return digitalRead(36) == LOW; }                   // XPT2046 IRQ goes low while touched
const uint8_t LED_R = 4, LED_G = 16, LED_B = 17;                    // RGB LED, active low
#else
Btn bLeft = {0, false, 0, false}, bRight = {35, false, 0, false};
#endif

#ifdef BOARD_CYD
// Touch screen gestures, decided by how long you hold (the 3.2" _I model's GPIO0 reads pressed all the time, so the
// touch screen does BOOT's jobs too):  tap = next mode,  1.5 s = AUTO,  4 s = next panel setting,  10 s = power off.
// While you hold, the label shows what letting go will do.
struct { bool down = false; uint32_t last = 0, upSince = 0, downSince = 0; } tch;
enum TouchAct : uint8_t { TA_NONE, TA_TAP, TA_AUTO, TA_PANEL, TA_OFF };
uint8_t touchHint = TA_NONE;
uint8_t holdAct(uint32_t ms) { return ms >= 10000 ? TA_OFF : ms >= 4000 ? TA_PANEL : ms >= 1500 ? TA_AUTO : TA_TAP; }
uint8_t pollTouch(uint32_t now) {
  if (now - tch.last < 25) return TA_NONE;
  tch.last = now;
  if (rawTouch()) {
    tch.upSince = 0;
    if (!tch.down) { tch.down = true; tch.downSince = now; }
    touchHint = holdAct(now - tch.downSince);
    if (touchHint == TA_OFF) { tch.down = false; touchHint = TA_NONE; return TA_OFF; }   // 10 s: act while held
  } else if (tch.down) {
    if (!tch.upSince) tch.upSince = now;
    else if (now - tch.upSince > 120) { tch.down = false; uint8_t a = holdAct(tch.upSince - tch.downSince); touchHint = TA_NONE; return a; }
  }
  return TA_NONE;
}
#endif

// ================= beat link =================
QueueHandle_t rxQ;
bool radioUp = false;
bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) {
#else
  esp_now_register_recv_cb([](const uint8_t*, const uint8_t* d, int len) {
#endif
    if (!linkValid(d, len)) return;
    LinkFrame f; memcpy(&f, d, sizeof(f)); xQueueSend(rxQ, &f, 0);
  });
  return true;
}

VizState vs = {};
bool gotAny = false; uint8_t lastSeq = 0; uint32_t lastRxMs = 0, nextBeat = 0;
float frac = 0;

void newBeat(uint32_t t, uint8_t pos) {
  vs.beatMs = t; vs.beatId++; vs.beatPos = pos;
  if (pos == 0) {
    vs.bars++;
    if (vs.autoMode && vs.bars % 8 == 0) { vs.mode = (vs.mode + 1) % V_COUNT; vs.labelUntil = t + 1500; }
  }
#ifdef BOARD_CYD
  int r, g, b; vhue(vs.hue, r, g, b);                      // LED: a blip of the conductor's colour
  analogWrite(LED_R, 255 - r / (pos == 0 ? 1 : 4)); analogWrite(LED_G, 255 - g / (pos == 0 ? 1 : 4)); analogWrite(LED_B, 255 - b / (pos == 0 ? 1 : 4));
#endif
}

void linkStep(uint32_t now) {
  LinkFrame f;
  while (xQueueReceive(rxQ, &f, 0) == pdTRUE) {
    if (f.type != LF_RELAY && f.type != LF_ACK) continue;
    if (gotAny && f.seq == lastSeq && now - lastRxMs < 250) continue;     // same beat twice (ACK + echo)
    bool booth = f.type == LF_ACK || (f.flags & LINK_FLAG_ECHO);
    gotAny = true; lastSeq = f.seq; lastRxMs = now;
    vs.bpm = f.bpm10 / 10.0f; vs.hue = f.hue; vs.src = booth ? SRC_BOOTH : SRC_W10;
    nextBeat = now + (uint32_t)vperiod(vs) + 40; frac = 0;
    newBeat(now, f.beat & 3);
  }
  bool live = gotAny && now - lastRxMs < FLYWHEEL_MS;
  if (!live) {                                             // demo: our own 120 BPM, colour drifting
    if (vs.src != SRC_DEMO) { vs.src = SRC_DEMO; vs.bpm = 120; nextBeat = now; }
  }
  if ((int32_t)(now - nextBeat) >= 0) {                    // flywheel between packets, or the demo clock
    float a = frac + vperiod(vs); uint32_t st = (uint32_t)a; frac = a - st;
    nextBeat += st; if ((int32_t)(now - nextBeat) > 0) nextBeat = now + st;
    if (vs.src == SRC_DEMO && ((vs.beatPos + 1) & 3) == 0) vs.hue += 24;
    newBeat(now, (vs.beatPos + 1) & 3);
  }
}

// ================= setup / loop =================
uint8_t brightLevels[3] = {255, 140, 60}; uint8_t brightIdx = 0;
uint8_t panelMode = 0;   // CYD: bit0 = BGR colour order, bit1 = mirrored
bool flipped = false;

void applyRotation() {
#ifdef BOARD_CYD
  tft.setRotation((panelMode & 2 ? 4 : 0) + 1);            // rotations 4-7 are the mirrored ones
#else
  tft.setRotation(flipped ? 3 : 1);
#endif
}

// "Off": screen, LED and radio off, then deep sleep. Wake it with the RST button or by re-plugging power.
// On the 3.2" running from its battery, the battery chip also cuts power after ~30 s at this low current,
// so the side (battery) button turns it back on.
void powerOff() {
  cv.fillScreen(TFT_BLACK); cv.setFont(&vfLbl); cv.setTextDatum(middle_center); cv.setTextColor(V_AMB);
  cv.drawString("POWERING OFF", cv.width() / 2, cv.height() / 2 - 10);
  cv.setTextColor(V_DIM); cv.drawString("RST or re-plug to wake", cv.width() / 2, cv.height() / 2 + 14);
  cv.pushSprite(0, 0); delay(1500);
#ifdef BOARD_CYD
  for (uint8_t p : {LED_R, LED_G, LED_B}) { analogWrite(p, 255); }
#endif
  tft.setBrightness(0); tft.sleep();
  esp_now_deinit(); esp_wifi_stop();
  esp_deep_sleep_start();
}

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // anti-aliased glyphs are copied onto the stack while drawing

void setup() {
  Serial.begin(115200);
  prefs.begin("beatviz", false);
  vs.mode = prefs.getUChar("mode", V_PULSE); if (vs.mode >= V_COUNT) vs.mode = V_PULSE;
  vs.autoMode = prefs.getBool("auto", true);
  brightIdx = prefs.getUChar("bright", 0) % 3;
  flipped = prefs.getBool("flip", false);
  panelMode = prefs.getUChar("panel", 0) & 7;
  vs.bpm = 120; vs.hue = 160; vs.hud = true; vs.src = SRC_DEMO; vs.labelUntil = millis() + 2500;

#if defined(BOARD_CYD32)
  pinMode(0, INPUT_PULLUP);
  gtInit();
  for (uint8_t p : {LED_R, LED_G, LED_B}) { pinMode(p, OUTPUT); analogWrite(p, 255); }
#elif defined(BOARD_PHANTOM)
  pinMode(0, INPUT_PULLUP); pinMode(21, INPUT);                      // 21 is the capacitive chip's interrupt here: leave it alone
  bool cap = capInit();
  if (!cap) tft.addResistiveTouch();
  Serial.printf("# Phantom touch: %s\n", cap ? "capacitive CST820" : "resistive XPT2046");
  for (uint8_t p : {LED_R, LED_G, LED_B}) { pinMode(p, OUTPUT); analogWrite(p, 255); }
#elif defined(BOARD_CYD)
  pinMode(0, INPUT_PULLUP); pinMode(36, INPUT);
  for (uint8_t p : {LED_R, LED_G, LED_B}) { pinMode(p, OUTPUT); analogWrite(p, 255); }
#else
  pinMode(0, INPUT_PULLUP); pinMode(35, INPUT);
#endif
#ifdef BOARD_CYD32
  tft.setup(!(panelMode & 1));                             // this panel is BGR + inverted by default (Freenove/Sunton setup)
#else
  tft.setup(panelMode & 1);
#endif
  tft.init();
#ifdef BOARD_CYD
  if (panelMode & 4) tft.invertDisplay(
#ifdef BOARD_CYD32
    false
#else
    true
#endif
  );                                                       // setting bit 2: colours inverted from the default
#endif
  applyRotation();
  tft.setBrightness(brightLevels[brightIdx]);
  tft.fillScreen(TFT_BLACK);
  vizInitFonts();
#ifdef BOARD_CYD
  cv.setColorDepth(8);                                     // 320x240 at 8-bit fits in RAM; pushed as 16-bit
#else
  cv.setColorDepth(16);
#endif
  if (!cv.createSprite(tft.width(), tft.height())) Serial.println("# sprite alloc failed");
  rxQ = xQueueCreate(16, sizeof(LinkFrame));
  tft.setBrightness(40);                                   // Wi-Fi RF calibration + full backlight can brown out a weak USB port
  radioUp = espnowInit();
  delay(150);
  tft.setBrightness(brightLevels[brightIdx]);
  Serial.printf("# panel setting %u (BGR %u, mirror %u, invert %u), BOOT reads %s\n", panelMode, panelMode & 1, (panelMode >> 1) & 1,
                (panelMode >> 2) & 1, digitalRead(0) ? "released" : "PRESSED");
  Serial.printf("# Beat Visualizer v%s (%s) | ESP-NOW %s\n", FW_VERSION,
#if defined(BOARD_CYD32)
                "CYD 3.2 IPS",
#elif defined(BOARD_PHANTOM)
                "RL Phantom 2.4",
#elif defined(BOARD_CYD)
                "CYD",
#else
                "TTGO T-Display",
#endif
                radioUp ? "up" : "FAILED");
  nextBeat = millis() + 500;
}

// BOOT only counts once it has read "released" for half a second after power-up, so a button (or a board) that
// holds GPIO0 low can't keep cycling the panel setting and restarting (v1.1.1).
bool bootReady(uint32_t now) {
  static bool armed = false; static uint32_t highSince = 0;
  if (armed) return true;
  if (digitalRead(0) == HIGH) { if (!highSince) highSince = now | 1; else if (now - highSince > 500) armed = true; }
  else highSince = 0;
  return armed;
}

void nextMode(uint32_t now) { vs.mode = (vs.mode + 1) % V_COUNT; vs.labelUntil = now + 1500; prefs.putUChar("mode", vs.mode); }
void toggleAuto(uint32_t now) { vs.autoMode = !vs.autoMode; vs.labelUntil = now + 1500; prefs.putBool("auto", vs.autoMode); }

void loop() {
  uint32_t now = millis();
  linkStep(now);
#ifdef BOARD_CYD
  uint8_t t = pollTouch(now); int b = bootReady(now) ? pollBtn(bBoot, 2000) : 0;
  if (t == TA_TAP) nextMode(now);
  if (t == TA_AUTO || b == 1) toggleAuto(now);
  if (t == TA_PANEL) b = 2;
  if (t == TA_OFF) powerOff();
  vs.hint = touchHint == TA_AUTO ? (vs.autoMode ? "LET GO: AUTO OFF" : "LET GO: AUTO ON") : touchHint == TA_PANEL ? "LET GO: NEXT SETTING"
          : (touchHint == TA_TAP && tch.down && now - tch.downSince > 400) ? "HOLD: 1.5s AUTO  4s SETTING  10s OFF" : nullptr;
  if (tch.down && now - tch.downSince > 7000) vs.hint = "KEEP HOLDING TO POWER OFF";
  if (b == 2) { panelMode = (panelMode + 1) & 7; prefs.putUChar("panel", panelMode); delay(100); ESP.restart(); }
  static uint32_t ledOff = 0;                              // LED blip lasts ~80 ms
  if (now - vs.beatMs > 80 && ledOff != vs.beatId) { ledOff = vs.beatId; for (uint8_t p : {LED_R, LED_G, LED_B}) analogWrite(p, 255); }
#else
  int l = pollBtn(bLeft, 700), r = pollBtn(bRight, 700);
  if (l == 1) nextMode(now);
  if (l == 2) toggleAuto(now);
  if (r == 1) { brightIdx = (brightIdx + 1) % 3; tft.setBrightness(brightLevels[brightIdx]); prefs.putUChar("bright", brightIdx); }
  if (r == 2) { flipped = !flipped; prefs.putBool("flip", flipped); applyRotation(); }
#endif
  static uint32_t lastDraw = 0;
  if (now - lastDraw < 30) return;                         // ~33 fps cap
  lastDraw = now;
  vizDraw(cv, vs, now);
  cv.pushSprite(0, 0);
}
