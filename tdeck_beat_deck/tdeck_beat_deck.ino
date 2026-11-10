// Beat Deck: Beat Commander on the LilyGo T-Deck Plus (also the plain T-Deck), 915 MHz SX1262 version.
// The T-Deck has its own LoRa radio, so it hears the conductor's beats directly on 916.0 MHz (SF7 / BW500) and also
// follows the ESP-NOW beat link (channel 1). It can take command of the beat and transmit on LoRa itself, with no
// Heltec bridge needed, and mirrors every beat it sends on ESP-NOW so the CYD, Dial, Cardputer, badge and
// visualizers follow it too. Optional bridge role (G): it then does the Heltec bridge's job for the other controllers.
//
// Two pages: the console (BPM, stats, beat bars) and full-screen visuals (the same six modes as the Beat Visualizer).
//
// Keys (digits come from alt + the letter keys, as printed on the keyboard)
//   trackball click or T   take over / release        R      release
//   SPACE                  tap tempo (takes over)      H      help
//   alt 128.5 ENTER        take over at a typed BPM (60-200). DEL fixes a digit, X cancels.
//   trackball up / down    tempo +/- 0.1 BPM           trackball left / right   nudge the beat 10 ms early / late
//   V or touch             console / visuals           M next visual   A auto visuals (new one every 8 bars)
//   G                      bridge role on / off        B brightness    D display fix (colour / flip / invert)
//
// Built by Anubis Darkwatch with Claude. MIT license.
// Arduino IDE 2.x, esp32 by Espressif 3.x, board "ESP32S3 Dev Module", Flash 16MB, PSRAM "OPI PSRAM",
// partition "16M Flash (3MB APP/9.9MB FATFS)", USB CDC On Boot: Enabled. Library: LovyanGFX.

#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "link.h"
#include "ui.h"

#define FW_VERSION     "1.2"
#define LINK_FREQ_HZ   916000000UL
#define LINK_POWER_DBM 10          // same as the W10 conductor and the Heltec bridge
#define LINK_AIR_MS    11          // SF7 / BW500 airtime of a beat packet: flash this long after sending
#define FLYWHEEL_MS    4000        // keep the beat going this long after the last packet

// ---------- T-Deck pins ----------
#define P_POWER   10               // peripherals power (keyboard, radio, display): must be high
#define P_SCK 40
#define P_MOSI 41
#define P_MISO 38
#define P_TFT_CS 12
#define P_TFT_DC 11
#define P_TFT_BL 42
#define P_SD_CS 39
#define L_NSS 9
#define L_BUSY 13
#define L_RST 17
#define L_DIO1 45
#define P_SDA 18
#define P_SCL 8
#define P_TOUCH_INT 16
#define KB_ADDR 0x55
#define TB_UP 3
#define TB_DOWN 15
#define TB_LEFT 1
#define TB_RIGHT 2
#define TB_CLICK 0
#define P_BAT 4

// ================= display =================
class Panel_TDeck : public lgfx::Panel_ST7789 {     // gamma curve 1, as LilyGo / Meshtastic use for this panel
protected:
  const uint8_t* getInitCommands(uint8_t listno) const override {
    static uint8_t list[] = {0x26, 1, 0x01, 0xFF, 0xFF};
    if (listno == 1) return list;
    return Panel_ST7789::getInitCommands(listno);
  }
};
class LGFX_TDeck : public lgfx::LGFX_Device {
  Panel_TDeck _panel; lgfx::Bus_SPI _bus; lgfx::Light_PWM _light;
public:
  void setup(bool bgr, bool invert) {
    auto b = _bus.config();
    b.spi_host = SPI2_HOST; b.spi_mode = 0; b.freq_write = 40000000; b.freq_read = 16000000;
    b.spi_3wire = false; b.use_lock = true;              // shares the Arduino SPI lock with the LoRa radio
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = P_SCK; b.pin_mosi = P_MOSI; b.pin_miso = P_MISO; b.pin_dc = P_TFT_DC;
    _bus.config(b); _panel.setBus(&_bus);
    auto p = _panel.config();
    p.pin_cs = P_TFT_CS; p.pin_rst = -1; p.pin_busy = -1;
    p.panel_width = 240; p.panel_height = 320; p.offset_rotation = 1;   // rotation 0 = landscape, keyboard below
    p.readable = false; p.invert = invert; p.rgb_order = bgr; p.dlen_16bit = false; p.bus_shared = true;
    _panel.config(p);
    auto l = _light.config();
    l.pin_bl = P_TFT_BL; l.invert = false; l.freq = 44100; l.pwm_channel = 7;
    _light.config(l); _panel.setLight(&_light);
    setPanel(&_panel);
  }
};
LGFX_TDeck tft;
LGFX_Sprite cv(&tft);
Preferences prefs;
uint8_t panelMode = 0;             // bit0 BGR, bit1 turn 180, bit2 un-invert
const uint8_t BRIGHT[] = {50, 110, 180, 253};
uint8_t brightIdx = 2;

void applyPanel() {
  tft.setup(panelMode & 1, !(panelMode & 4));
  tft.init();
  tft.setRotation(panelMode & 2 ? 2 : 0);
  tft.setBrightness(BRIGHT[brightIdx]);
}

// ================= SX1262 (raw commands, as in the Heltec bridge) =================
SPISettings loraSPI(8000000, MSBFIRST, SPI_MODE0);
void waitBusy(uint32_t ms = 300) { uint32_t t = millis(); while (digitalRead(L_BUSY) && millis() - t < ms) delayMicroseconds(20); }
void loraXfer(const uint8_t* d, int n, uint8_t* rx = nullptr) {
  waitBusy();
  SPI.beginTransaction(loraSPI); digitalWrite(L_NSS, LOW);          // the transaction lock keeps the display off the bus
  for (int i = 0; i < n; i++) { uint8_t r = SPI.transfer(d[i]); if (rx) rx[i] = r; }
  digitalWrite(L_NSS, HIGH); SPI.endTransaction();
}
void cmd(std::initializer_list<uint8_t> l) { uint8_t b[16]; int n = 0; for (auto x : l) b[n++] = x; loraXfer(b, n); }
uint8_t readReg(uint16_t a) { uint8_t t[5] = {0x1D, (uint8_t)(a >> 8), (uint8_t)a, 0, 0}, r[5]; loraXfer(t, 5, r); return r[4]; }
void writeReg(uint16_t a, uint8_t v) { cmd({0x0D, (uint8_t)(a >> 8), (uint8_t)a, v}); }

void startRx() {
  cmd({0x8C, 0x00, 0x08, 0x00, 0xFF, 0x01, 0x00});     // preamble 8, explicit, max len, CRC on, IQ normal
  cmd({0x02, 0xFF, 0xFF});                             // clear IRQs
  cmd({0x82, 0xFF, 0xFF, 0xFF});                       // RX continuous
}
bool loraInit() {
  pinMode(L_RST, OUTPUT); digitalWrite(L_RST, LOW); delay(10); digitalWrite(L_RST, HIGH); delay(20); waitBusy(1000);
  cmd({0x80, 0x00});                                   // standby RC
  if (readReg(0x0740) != 0x14) return false;           // sync word register: no answer = no radio
  cmd({0x97, 0x02, 0x00, 0x01, 0x40});                 // DIO3 -> TCXO 1.8 V, 5 ms
  cmd({0x89, 0x7F}); delay(10); waitBusy();            // calibrate all (needed after TCXO)
  cmd({0x96, 0x01});                                   // DC-DC
  cmd({0x9D, 0x01});                                   // DIO2 = RF switch
  cmd({0x8A, 0x01});                                   // LoRa
  cmd({0x98, 0xE1, 0xE9}); delay(5);                   // image cal 902-928
  uint32_t f = (uint32_t)(((uint64_t)LINK_FREQ_HZ << 25) / 32000000ULL);
  cmd({0x86, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f});
  cmd({0x95, 0x04, 0x07, 0x00, 0x01});                 // PA config (SX1262 high power)
  cmd({0x8E, (uint8_t)LINK_POWER_DBM, 0x04});          // TX power, 200 us ramp
  cmd({0x8B, 0x07, 0x06, 0x01, 0x00});                 // SF7, BW500, CR4/5 (matches the conductor)
  cmd({0x8F, 0x00, 0x00});                             // TX and RX buffer base 0
  writeReg(0x08AC, 0x96);                              // RX boosted gain
  cmd({0x08, 0x02, 0x43, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00}); // IRQ: TxDone|RxDone|CrcErr|HdrErr, DIO1 = TxDone|RxDone
  startRx();
  return true;
}
void loraSend(const BeatPacket& p) {
  uint8_t buf[2 + sizeof(BeatPacket)] = {0x0E, 0x00};
  memcpy(buf + 2, &p, sizeof(p));
  cmd({0x80, 0x01});                                   // standby XOSC (leaves RX)
  loraXfer(buf, sizeof(buf));
  cmd({0x8C, 0x00, 0x08, 0x00, (uint8_t)sizeof(p), 0x01, 0x00});
  cmd({0x02, 0xFF, 0xFF});
  cmd({0x83, 0x00, 0x00, 0x00});                       // TX; TxDone puts us back in RX
}
// one radio event, if DIO1 is up: 1 = TX done, 2 = beat packet received
int loraPoll(BeatPacket& p, int& rssi, int8_t& snr4) {
  if (!digitalRead(L_DIO1)) return 0;
  uint8_t t[4] = {0x12, 0, 0, 0}, r[4]; loraXfer(t, 4, r);
  uint16_t irq = (r[2] << 8) | r[3];
  cmd({0x02, 0xFF, 0xFF});
  if (irq & 0x0001) { startRx(); return 1; }
  if (!(irq & 0x0002) || (irq & 0x0040)) return 0;     // not RxDone, or CRC error
  uint8_t s[4] = {0x13, 0, 0, 0}, rs[4]; loraXfer(s, 4, rs);
  int len = rs[2], start = rs[3];
  if (len != (int)sizeof(BeatPacket)) return 0;
  uint8_t b[3 + sizeof(BeatPacket)] = {0x1E, (uint8_t)start, 0}, rb[3 + sizeof(BeatPacket)];
  loraXfer(b, sizeof(b), rb);
  uint8_t q[5] = {0x14, 0, 0, 0, 0}, rq[5]; loraXfer(q, 5, rq);
  memcpy(&p, rb + 3, sizeof(p));
  if (p.m0 != 'B' || p.m1 != 'T') return 0;
  rssi = -(int)rq[2] / 2; snr4 = (int8_t)rq[3];
  return 2;
}

// ================= ESP-NOW =================
const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t rxQ;
bool espnowUp = false, loraUp = false;

void onFrame(const uint8_t* d, int len) {
  if (!linkValid(d, len)) return;
  LinkFrame f; memcpy(&f, d, sizeof(f));
  xQueueSend(rxQ, &f, 0);
}
bool espnowInit() {
  WiFi.mode(WIFI_STA); WiFi.disconnect();
  esp_wifi_set_channel(LINK_ESPNOW_CH, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) return false;
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_now_peer_info_t peer = {};
  memset(peer.peer_addr, 0xFF, 6); peer.channel = LINK_ESPNOW_CH; peer.encrypt = false;
  esp_now_add_peer(&peer);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_now_register_recv_cb([](const esp_now_recv_info_t*, const uint8_t* d, int len) { onFrame(d, len); });
#else
  esp_now_register_recv_cb([](const uint8_t*, const uint8_t* d, int len) { onFrame(d, len); });
#endif
  return true;
}
void sendFrame(const LinkFrame& f) { if (espnowUp) esp_now_send(BCAST, (const uint8_t*)&f, sizeof(f)); }

// ================= shared state (control task <-> UI) =================
struct Core {
  uint8_t mode = M_FOLLOW;
  float bpm = 0; int beatPos = 3; uint8_t hue = 160;
  bool gotAny = false; uint8_t lastSeq = 0;
  uint32_t lastRxMs = 0, lastBeatMs = 0, flyNext = 0, nextBeat = 0, beatId = 0;
  uint32_t rx = 0, lost = 0, fires = 0, acks = 0, lastFireMs = 0, bars = 0, relayed = 0, heard = 0;
  int rssi = 0; float snr = 0; int nudgeMs = 0;
  uint32_t bridgeMs = 0, conflictUntil = 0; bool bridgeSeen = false;
  uint32_t flashMs = 0; bool flashDown = false, viaBooth = false, viaLora = false;
  uint32_t tapTimes[5]; int tapCount = 0;
  float frac = 0;
  bool role = false;                               // bridge role
  uint8_t mySeq = 0; uint32_t mySeqMs = 0;         // last beat we put on LoRa (a Heltec bridge relays it back)
  uint8_t toast = T_NONE; uint32_t toastUntil = 0;
};
Core core;
SemaphoreHandle_t lock;            // a mutex (not a spinlock): the beat maths uses floats

enum KeyCmd : uint8_t { K_TAKE_TOGGLE, K_RELEASE, K_TAP, K_SETBPM, K_TEMPO, K_NUDGE, K_ROLE };
struct KeyEv { uint8_t cmd; float val; };
QueueHandle_t keyQ;

static inline float periodMs(float bpm) { return 60000.0f / (bpm > 30 ? bpm : 120); }
static uint32_t stepMs() { float a = core.frac + periodMs(core.bpm); uint32_t s = (uint32_t)a; core.frac = a - s; return s; }
void beatAt(uint32_t t, bool down, bool booth) {
  core.flashMs = t; core.flashDown = down; core.lastBeatMs = t; core.viaBooth = booth; core.beatId++;
  if (down) core.bars++;
}
void setToast(uint8_t t, uint32_t now, uint32_t ms) { core.toast = t; core.toastUntil = now + ms; }

void takeOver(uint32_t now) {
  if (core.mode == M_TAKE) return;
  core.mode = M_TAKE; core.fires = core.acks = 0; core.nudgeMs = 0; core.tapCount = 0;
  if (core.bpm < 60 || core.bpm > 200) core.bpm = 120;
  float p = periodMs(core.bpm);
  if (core.gotAny && now - core.lastBeatMs < 2 * p) {     // seamless: continue the current beat grid
    uint32_t nb = core.lastBeatMs + (uint32_t)p;
    while ((int32_t)(nb - now) <= 0) { nb += (uint32_t)p; core.beatPos = (core.beatPos + 1) & 3; }
    core.nextBeat = nb;
  } else { core.nextBeat = now + 20; core.beatPos = 3; }
  setToast(loraUp ? T_TAKE : T_NO_LORA, now, 1200);
}
void release(uint32_t now) {
  if (core.mode != M_TAKE) return;
  core.mode = M_FOLLOW; core.gotAny = true;
  core.lastRxMs = now - 1500; core.flyNext = core.nextBeat;    // HOLD: flywheel until the conductor is back
  core.conflictUntil = 0; core.tapCount = 0;
  setToast(T_RELEASE, now, 1200);
}
void tapTempo(uint32_t now) {
  takeOver(now);
  if (core.tapCount && now - core.tapTimes[core.tapCount - 1] > 2000) core.tapCount = 0;
  if (core.tapCount == 5) { memmove(core.tapTimes, core.tapTimes + 1, 4 * sizeof(uint32_t)); core.tapCount = 4; }
  core.tapTimes[core.tapCount++] = now;
  if (core.tapCount >= 2) {
    float avg = (float)(core.tapTimes[core.tapCount - 1] - core.tapTimes[0]) / (core.tapCount - 1);
    core.bpm = constrain(60000.0f / avg, 60.0f, 200.0f);
  }
  if (core.tapCount == 1) core.beatPos = 3;
  core.nextBeat = now; core.nudgeMs = 0;
}

// a beat from anywhere (LoRa direct, a bridge relay, a booth ACK / echo) while following
void followBeat(uint8_t seq, uint16_t bpm10, uint8_t beat, uint8_t hue, bool booth, bool lora, int rssi, float snr, uint32_t now) {
  if (core.gotAny && seq == core.lastSeq && now - core.lastRxMs < 250) return;   // same beat twice
  if (!booth && rssi) { core.rssi = rssi; core.snr = snr; }
  if (core.gotAny) { uint8_t gap = seq - core.lastSeq; if (gap > 1 && gap < 64) core.lost += gap - 1; }
  core.lastSeq = seq; core.rx++; core.gotAny = true; core.lastRxMs = now; core.viaLora = lora;
  core.bpm = bpm10 / 10.0f; core.beatPos = beat & 3; core.hue = hue;
  core.flyNext = now + (uint32_t)periodMs(core.bpm) + 40; core.frac = 0;
  beatAt(now, core.beatPos == 0, booth);
}
bool ownEcho(uint8_t seq, uint32_t now) { return seq == core.mySeq && now - core.mySeqMs < 400; }

// ================= control task: radios, keys, beat timing =================
void controlTask(void*) {
  bool txBusy = false, txIsFire = false; uint32_t txStart = 0; BeatPacket txPkt = {};
  uint32_t lastHello = 0;
  for (;;) {
    uint32_t now = millis();
    LinkFrame in[8]; int nIn = 0;
    while (nIn < 8 && xQueueReceive(rxQ, &in[nIn], 0) == pdTRUE) nIn++;
    KeyEv ke[4]; int nKey = 0;
    while (nKey < 4 && xQueueReceive(keyQ, &ke[nKey], 0) == pdTRUE) nKey++;
    BeatPacket lp; int lrssi = 0; int8_t lsnr4 = 0; int lev = 0;
    if (loraUp) lev = loraPoll(lp, lrssi, lsnr4);
    if (txBusy && now - txStart > 200) { txBusy = false; if (loraUp) startRx(); }   // TxDone never came: recover

    LinkFrame out[4]; int nOut = 0; bool doTx = false; BeatPacket tx = {};
    xSemaphoreTake(lock, portMAX_DELAY);
    // --- LoRa ---
    if (lev == 1 && txBusy) {                                 // our packet is on the air
      txBusy = false;
      LinkFrame e = linkFrame(txIsFire ? LF_ACK : LF_RELAY);  // tell the ESP-NOW side
      e.seq = txPkt.seq; e.bpm10 = txPkt.bpm10; e.beat = txPkt.beat; e.hue = txPkt.hue;
      if (txIsFire) e.count = ++core.relayed; else { e.flags = LINK_FLAG_ECHO; core.acks++; }
      out[nOut++] = e;
      if (txIsFire && core.mode != M_TAKE)                    // a booth controller's beat, via us
        followBeat(txPkt.seq, txPkt.bpm10, txPkt.beat, txPkt.hue, true, true, 0, 0, now);
    } else if (lev == 2) {
      core.heard++;
      if (core.role) {                                        // bridge: relay it for the ESP-NOW side
        LinkFrame rf = linkFrame(LF_RELAY);
        rf.seq = lp.seq; rf.bpm10 = lp.bpm10; rf.beat = lp.beat; rf.hue = lp.hue; rf.rssi = lrssi; rf.snr4 = lsnr4;
        out[nOut++] = rf; core.relayed++;
      }
      if (core.mode == M_TAKE) core.conflictUntil = now + 1500;   // someone else is on the air
      else followBeat(lp.seq, lp.bpm10, lp.beat, lp.hue, false, true, lrssi, lsnr4 / 4.0f, now);
    }
    // --- ESP-NOW ---
    for (int i = 0; i < nIn; i++) {
      const LinkFrame& f = in[i];
      if (f.type == LF_FIRE) {                                // a booth controller wants a beat on LoRa
        if (core.mode == M_TAKE) core.conflictUntil = now + 1500;
        else if (core.role && loraUp && !txBusy && !doTx) {
          tx = {'B', 'T', (uint8_t)(core.gotAny ? core.lastSeq + 1 : core.relayed), f.bpm10, (uint8_t)(f.beat & 3), f.hue};
          doTx = true; txIsFire = true; core.mySeq = tx.seq; core.mySeqMs = now;
        }
        continue;
      }
      core.bridgeMs = now; core.bridgeSeen = true;
      if (f.type == LF_HELLO) continue;
      bool echo = f.flags & LINK_FLAG_ECHO;
      if (ownEcho(f.seq, now)) continue;                      // a Heltec bridge relaying our own beat
      if (core.mode == M_TAKE) {
        if (f.type == LF_RELAY && !echo) core.conflictUntil = now + 1500;
        continue;
      }
      if (f.type != LF_RELAY && f.type != LF_ACK) continue;
      bool booth = f.type == LF_ACK || echo;
      followBeat(f.seq, f.bpm10, f.beat, f.hue, booth, false, booth ? 0 : f.rssi, f.snr4 / 4.0f, now);
    }
    // --- keys ---
    for (int i = 0; i < nKey; i++) {
      switch (ke[i].cmd) {
        case K_TAKE_TOGGLE: if (core.mode == M_TAKE) release(now); else takeOver(now); break;
        case K_RELEASE: release(now); break;
        case K_TAP: tapTempo(now); break;
        case K_SETBPM:
          if (ke[i].val < 60 || ke[i].val > 200) { setToast(T_BPM_RANGE, now, 1200); break; }
          takeOver(now); core.bpm = roundf(ke[i].val * 10) / 10; break;
        case K_TEMPO:
          if (core.mode != M_TAKE) { setToast(T_FOLLOW_ONLY, now, 1200); break; }
          core.bpm = constrain(roundf((core.bpm + ke[i].val) * 10) / 10, 60.0f, 200.0f); break;
        case K_NUDGE:
          if (core.mode != M_TAKE) { setToast(T_FOLLOW_ONLY, now, 1200); break; }
          core.nextBeat += (int)ke[i].val; core.nudgeMs += (int)ke[i].val; break;
        case K_ROLE:
          core.role = ke[i].val > 0;
          setToast(!loraUp ? T_NO_LORA : core.role ? T_ROLE_ON : T_ROLE_OFF, now, 1200); break;
      }
    }
    // --- beat timing ---
    if (core.mode == M_TAKE) {
      if ((int32_t)(now - core.nextBeat) >= 0) {
        if (now - core.nextBeat > periodMs(core.bpm)) core.nextBeat = now;   // fell far behind: resync
        core.beatPos = (core.beatPos + 1) & 3;
        if (core.beatPos == 0) core.hue += 40;                // new colour every bar, like the W10
        core.nextBeat += stepMs();
        core.fires++; core.lastFireMs = now;
        if (loraUp && !txBusy && !doTx) {
          core.lastSeq++;                                     // keep the followers' seq continuous
          tx = {'B', 'T', core.lastSeq, (uint16_t)(core.bpm * 10 + 0.5f), (uint8_t)core.beatPos, core.hue};
          doTx = true; txIsFire = false; core.mySeq = core.lastSeq; core.mySeqMs = now;
        } else if (!loraUp) {                                 // no radio: still drive the ESP-NOW side
          LinkFrame e = linkFrame(LF_RELAY); core.lastSeq++;
          e.seq = core.lastSeq; e.bpm10 = (uint16_t)(core.bpm * 10 + 0.5f); e.beat = core.beatPos; e.hue = core.hue;
          e.flags = LINK_FLAG_ECHO; core.acks++;
          if (nOut < 4) out[nOut++] = e;
        }
        beatAt(now + LINK_AIR_MS, core.beatPos == 0, true);
      }
    } else if (core.gotAny && core.bpm > 30 && now - core.lastRxMs < FLYWHEEL_MS &&
               (int32_t)(now - core.flyNext) >= 0) {          // packet missed: keep the beat going
      core.beatPos = (core.beatPos + 1) & 3;
      beatAt(now, core.beatPos == 0, core.viaBooth);
      core.flyNext += stepMs();
    }
    bool hello = core.role && now - lastHello >= 1000;
    uint32_t heard = core.heard;
    xSemaphoreGive(lock);

    if (doTx) { loraSend(tx); txBusy = true; txPkt = tx; txStart = now; }
    if (hello) {                                              // bridge heartbeat, like the Heltec's
      lastHello = now;
      LinkFrame h = linkFrame(LF_HELLO); h.flags = loraUp ? 1 : 0; h.count = heard;
      if (nOut < 4) out[nOut++] = h;
    }
    for (int i = 0; i < nOut; i++) sendFrame(out[i]);
    vTaskDelay(1);
  }
}

// ================= inputs: keyboard, trackball, touch =================
char input[8] = "";
bool help = false, vizPage = false;
VizState vs = {};
void queueKey(uint8_t cmd, float val = 0) { KeyEv e = {cmd, val}; xQueueSend(keyQ, &e, 0); }

volatile int16_t tbUp = 0, tbDown = 0, tbLeft = 0, tbRight = 0;
void IRAM_ATTR isrUp() { tbUp++; }
void IRAM_ATTR isrDown() { tbDown++; }
void IRAM_ATTR isrLeft() { tbLeft++; }
void IRAM_ATTR isrRight() { tbRight++; }
int takeCount(volatile int16_t& c) { noInterrupts(); int n = c; c = 0; interrupts(); return n; }

bool kbOk = false; uint8_t gtAddr = 0;
void nextViz(uint32_t now) { vs.mode = (vs.mode + 1) % V_COUNT; vs.labelUntil = now + 1500; prefs.putUChar("viz", vs.mode); }

void onKey(char c, uint32_t now) {
  size_t n = strlen(input);
  if (c == '\r' || c == '\n') { if (input[0]) { queueKey(K_SETBPM, atof(input)); input[0] = 0; } return; }
  if (c == 0x08 || c == 0x7F) { if (n) input[n - 1] = 0; return; }
  if ((c >= '0' && c <= '9') || (c == '.' && n && !strchr(input, '.'))) { if (n < 5) { input[n] = c; input[n + 1] = 0; } return; }
  switch (c) {
    case ' ': queueKey(K_TAP); break;
    case 'x': case 'X': input[0] = 0; help = false; break;
    case 't': case 'T': queueKey(K_TAKE_TOGGLE); break;
    case 'r': case 'R': queueKey(K_RELEASE); break;
    case 'h': case 'H': help = !help; break;
    case 'v': case 'V': vizPage = !vizPage; vs.labelUntil = now + 1500; break;
    case 'm': case 'M': nextViz(now); break;
    case 'a': case 'A': vs.autoMode = !vs.autoMode; vs.labelUntil = now + 1500; prefs.putBool("auto", vs.autoMode); break;
    case 'g': case 'G': { bool r = !prefs.getBool("role", false); prefs.putBool("role", r); queueKey(K_ROLE, r ? 1 : 0); break; }
    case 'b': case 'B': brightIdx = (brightIdx + 1) % sizeof(BRIGHT); tft.setBrightness(BRIGHT[brightIdx]); prefs.putUChar("bright", brightIdx); break;
    case 'd': case 'D': {
      panelMode = (panelMode + 1) & 7; prefs.putUChar("panel", panelMode); applyPanel();
      xSemaphoreTake(lock, portMAX_DELAY); setToast(T_PANEL, now, 1500); xSemaphoreGive(lock); break;
    }
    case '-': case '_': queueKey(K_TEMPO, -1.0f); break;
    case '+': queueKey(K_TEMPO, 1.0f); break;
  }
}

bool gtTouched() {                                    // GT911 "touch points" status register
  if (!gtAddr) return false;
  Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(gtAddr, (uint8_t)1) != 1) return false;
  uint8_t st = Wire.read();
  if (st & 0x80) { Wire.beginTransmission(gtAddr); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission(); }
  return (st & 0x80) && (st & 0x0F);
}

void inputs(uint32_t now) {
  static uint32_t lastPoll = 0;
  if (now - lastPoll < 15) return;
  lastPoll = now;
  if (kbOk && Wire.requestFrom((uint16_t)KB_ADDR, (size_t)1) == 1) {
    char c = Wire.read();
    if (c && c != (char)0xE0) onKey(c, now);
  }
  // trackball: every roll pulse is one step
  int up = takeCount(tbUp), dn = takeCount(tbDown), lf = takeCount(tbLeft), rt = takeCount(tbRight);
  if (up - dn) queueKey(K_TEMPO, (up - dn) * 0.1f);
  if (rt - lf) queueKey(K_NUDGE, (rt - lf) * 10.0f);
  // trackball click (GPIO0): only once it has read released, so a stuck-low pin can't loop
  static bool clickArmed = false, clickDown = false; static uint32_t relSince = 0, downAt = 0;
  bool c = digitalRead(TB_CLICK) == LOW;
  if (!c) { if (!relSince) relSince = now; if (now - relSince > 300) clickArmed = true; }
  else relSince = 0;
  if (clickArmed) {
    if (c && !clickDown) { clickDown = true; downAt = now; }
    else if (!c && clickDown) { clickDown = false; if (now - downAt > 30) queueKey(K_TAKE_TOGGLE); }
  }
  // touch: a tap swaps page
  static bool wasTouch = false; static uint32_t lastTouchPoll = 0, lastTap = 0;
  if (gtAddr && now - lastTouchPoll > 40) {
    lastTouchPoll = now;
    bool t = gtTouched();
    if (t && !wasTouch && now - lastTap > 350) { lastTap = now; vizPage = !vizPage; vs.labelUntil = now + 1500; }
    wasTouch = t;
  }
}

int batteryPct() {
  static int pct = -1; static uint32_t at = 0;
  if (at && millis() - at < 5000) return pct;
  at = millis() | 1;
  uint32_t mv = analogReadMilliVolts(P_BAT) * 2;
  if (mv < 2500) return pct = -1;                                // no battery / not readable
  pct = constrain(((int)mv - 3300) * 100 / (4150 - 3300), 0, 100);
  return pct;
}

// ================= screen =================
void snapshot(View& v, uint32_t now) {
  v = {};
  xSemaphoreTake(lock, portMAX_DELAY);
  v.mode = core.mode; v.role = core.role; v.loraOk = loraUp;
  v.bridgeOk = core.bridgeSeen && now - core.bridgeMs < 3000;
  v.conflict = core.mode == M_TAKE && (int32_t)(core.conflictUntil - now) > 0;
  if (!loraUp && !espnowUp) v.link = LS_NORADIO;
  else if (core.mode == M_TAKE) v.link = LS_TAKE;
  else if (!core.gotAny) v.link = LS_WAIT;
  else if (now - core.lastRxMs < 1500) v.link = LS_LIVE;
  else if (now - core.lastRxMs < FLYWHEEL_MS) v.link = LS_HOLD;
  else v.link = LS_WAIT;
  v.bpm = core.bpm; v.beatPos = core.beatPos; v.hue = core.hue; v.bars = core.bars;
  v.rssi = core.rssi; v.snr = core.snr; v.rx = core.rx; v.lost = core.lost; v.sent = core.acks; v.relayed = core.relayed;
  uint32_t pend = (core.fires > core.acks && now - core.lastFireMs < 80) ? 1 : 0;
  v.fail = core.fires > core.acks + pend ? core.fires - core.acks - pend : 0;
  v.nudgeMs = core.nudgeMs; v.viaBooth = core.viaBooth; v.viaLora = core.viaLora;
  v.flashDown = core.flashDown; v.flashMs = core.flashMs;
  v.toast = (int32_t)(core.toastUntil - now) > 0 ? core.toast : T_NONE;
  // visualizer state
  static uint32_t lastId = 0;
  bool live = v.link == LS_LIVE || v.link == LS_HOLD || v.link == LS_TAKE;
  if (live) {
    if (core.beatId != lastId) {
      lastId = core.beatId; vs.beatMs = core.flashMs; vs.beatId++; vs.beatPos = core.beatPos;
      if (core.beatPos == 0) { vs.bars++; if (vs.autoMode && vs.bars % 8 == 0) { vs.mode = (vs.mode + 1) % V_COUNT; vs.labelUntil = now + 1500; } }
    }
    vs.bpm = core.bpm; vs.hue = core.hue; vs.src = core.viaBooth || core.mode == M_TAKE ? SRC_BOOTH : SRC_W10;
  }
  xSemaphoreGive(lock);
  if (!live) {                                            // demo: our own 120 BPM, colour drifting
    static uint32_t demoNext = 0;
    if (vs.src != SRC_DEMO) { vs.src = SRC_DEMO; vs.bpm = 120; demoNext = now; }
    if ((int32_t)(now - demoNext) >= 0) {
      vs.beatPos = (vs.beatPos + 1) & 3; vs.beatMs = now; vs.beatId++; demoNext = now + 500;
      if (vs.beatPos == 0) { vs.hue += 24; vs.bars++; if (vs.autoMode && vs.bars % 8 == 0) { vs.mode = (vs.mode + 1) % V_COUNT; vs.labelUntil = now + 1500; } }
    }
  }
  v.battery = batteryPct();
  v.input = input; v.help = help; v.panelMode = panelMode;
}

void pushFrame() {                                        // in strips, so the radio never waits long for the bus
  const int STRIP = 24;
  for (int y = 0; y < SH; y += STRIP) {
    if (cv.getColorDepth() == 16) tft.pushImage(0, y, SW, STRIP, (const lgfx::swap565_t*)cv.getBuffer() + y * SW);
    else tft.pushImage(0, y, SW, STRIP, (const lgfx::rgb332_t*)cv.getBuffer() + y * SW);
  }
}

SET_LOOP_TASK_STACK_SIZE(16 * 1024);   // anti-aliased glyphs are copied onto the stack while drawing

void setup() {
  pinMode(P_POWER, OUTPUT); digitalWrite(P_POWER, HIGH);   // power the keyboard, radio and screen
  Serial.begin(115200);
  pinMode(L_NSS, OUTPUT); digitalWrite(L_NSS, HIGH);
  pinMode(P_SD_CS, OUTPUT); digitalWrite(P_SD_CS, HIGH);
  pinMode(P_TFT_CS, OUTPUT); digitalWrite(P_TFT_CS, HIGH);
  pinMode(L_BUSY, INPUT); pinMode(L_DIO1, INPUT);
  pinMode(TB_CLICK, INPUT_PULLUP);
  for (uint8_t p : {TB_UP, TB_DOWN, TB_LEFT, TB_RIGHT}) pinMode(p, INPUT_PULLUP);
  attachInterrupt(TB_UP, isrUp, FALLING); attachInterrupt(TB_DOWN, isrDown, FALLING);
  attachInterrupt(TB_LEFT, isrLeft, FALLING); attachInterrupt(TB_RIGHT, isrRight, FALLING);
  delay(100);

  prefs.begin("beatdeck", false);
  panelMode = prefs.getUChar("panel", 0) & 7;
  brightIdx = prefs.getUChar("bright", 2) % sizeof(BRIGHT);
  vs.mode = prefs.getUChar("viz", V_PULSE); if (vs.mode >= V_COUNT) vs.mode = V_PULSE;
  vs.autoMode = prefs.getBool("auto", true);
  vs.bpm = 120; vs.hue = 160; vs.hud = true; vs.src = SRC_DEMO;

  SPI.begin(P_SCK, P_MISO, P_MOSI);
  applyPanel();
  initFonts();
  cv.setColorDepth(16);
  if (!cv.createSprite(SW, SH)) { cv.setColorDepth(8); cv.createSprite(SW, SH); }   // 8-bit fits anywhere

  // splash: animated for SPLASH_MS while the radios and the keyboard start up
  static const char* const names[] = {"LORA 916.0", "ESP-NOW CH1", "KEYBOARD", "TOUCH"};
  int8_t st[4] = {-1, -1, -1, -1};
  uint32_t t0 = millis();
  auto splashUntil = [&](uint32_t ms) {
    do { drawSplash(cv, millis() - t0, names, st, 4, FW_VERSION); pushFrame(); delay(15); } while (millis() - t0 < ms);
  };
  splashUntil(700);
  lock = xSemaphoreCreateMutex();
  rxQ = xQueueCreate(24, sizeof(LinkFrame));
  keyQ = xQueueCreate(16, sizeof(KeyEv));
  loraUp = loraInit(); st[0] = loraUp;
  splashUntil(1300);
  espnowUp = espnowInit(); st[1] = espnowUp;
  core.role = prefs.getBool("role", false);
  splashUntil(1900);                                   // the keyboard's own chip needs a moment after power-on
  Wire.begin(P_SDA, P_SCL, 400000);
  Wire.beginTransmission(KB_ADDR); kbOk = Wire.endTransmission() == 0; st[2] = kbOk;
  splashUntil(2400);
  for (uint8_t a : {0x5D, 0x14}) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { gtAddr = a; break; } }
  st[3] = gtAddr != 0;
  splashUntil(SPLASH_MS);
  Serial.printf("# Beat Deck v%s | LoRa %s | ESP-NOW %s | keyboard %s | touch %s | sprite %d-bit | MAC %s\n", FW_VERSION,
                loraUp ? "OK" : "FAILED", espnowUp ? "up" : "FAILED", kbOk ? "OK" : "missing", gtAddr ? "OK" : "missing",
                cv.getColorDepth(), WiFi.macAddress().c_str());
  vs.labelUntil = millis() + 1500;
  xTaskCreatePinnedToCore(controlTask, "control", 6144, nullptr, 5, nullptr, 1);
}

void loop() {
  static uint32_t lastDraw = 0;
  uint32_t now = millis();
  inputs(now);
  if (now - lastDraw < 25) { delay(2); return; }     // ~30-40 fps
  lastDraw = now;
  View v; snapshot(v, now);
  if (vizPage) {
    vs.hint = v.toast == T_TAKE ? "TAKEOVER" : v.toast == T_RELEASE ? "RELEASED" : v.toast == T_ROLE_ON ? "BRIDGE ON"
            : v.toast == T_ROLE_OFF ? "BRIDGE OFF" : v.toast == T_FOLLOW_ONLY ? "TAKE OVER FIRST" : v.toast == T_NO_LORA ? "NO LORA RADIO" : nullptr;
    vizDraw(cv, vs, now);
    if (v.link == LS_TAKE) {                         // a thin purple frame so you know you have the beat
      cv.drawRect(0, 0, SW, SH, v.conflict ? C_RED : C_PURP); cv.drawRect(1, 1, SW - 2, SH - 2, v.conflict ? C_RED : C_PURP);
    }
  } else draw(cv, v, now);
  pushFrame();
}
