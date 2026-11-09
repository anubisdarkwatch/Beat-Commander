// Beat Console link: ESP-NOW frames between the LoRa bridge and the CYD console.
// Keep this file identical in every sketch folder that uses the beat link (cyd, m5dial, sticks3, heltec bridge, xiao gateway, cardputer, piglet, visualizer, t-deck, t-dongle, w10 conductor).
#pragma once
#include <stdint.h>

#define LINK_NET_ID    0x42   // change on both boards to run two setups side by side
#define LINK_ESPNOW_CH 1      // Wi-Fi channel used for ESP-NOW (both boards)
#define LINK_FLAG_ECHO 0x02   // RELAY sent by a controller (Dial) mirroring the bridge's ACK, so consoles see booth beats

// Beat packet on the LoRa beat link (from the W10 conductor), unchanged.
struct __attribute__((packed)) BeatPacket { uint8_t m0, m1; uint8_t seq; uint16_t bpm10; uint8_t beat; uint8_t hue; };

enum : uint8_t {
  LF_RELAY = 'R',   // bridge -> console: a beat packet heard on LoRa (+ RSSI / SNR)
  LF_HELLO = 'H',   // bridge -> console: heartbeat once a second
  LF_FIRE  = 'F',   // console -> bridge: transmit this beat on LoRa now (takeover)
  LF_ACK   = 'A',   // bridge -> console: that beat went out (TX done)
};

struct __attribute__((packed)) LinkFrame {
  uint8_t  m0, m1;      // 'A', 'D'
  uint8_t  ver;         // 1
  uint8_t  net;         // LINK_NET_ID
  uint8_t  type;        // LF_*
  uint8_t  seq;         // beat seq (RELAY / ACK)
  uint16_t bpm10;       // BPM x 10
  uint8_t  beat;        // 0..3, 0 = downbeat
  uint8_t  hue;         // 0..255
  int16_t  rssi;        // RELAY: dBm
  int8_t   snr4;        // RELAY: SNR x 4
  uint8_t  flags;       // HELLO: bit0 = LoRa radio OK. RELAY: LINK_FLAG_ECHO
  uint32_t count;       // HELLO: beat packets heard since boot; ACK: beats sent
};

static inline bool linkValid(const uint8_t* d, int len) {
  return len == (int)sizeof(LinkFrame) && d[0] == 'A' && d[1] == 'D' && d[2] == 1 && d[3] == LINK_NET_ID;
}
static inline LinkFrame linkFrame(uint8_t type) {
  LinkFrame f = {};
  f.m0 = 'A'; f.m1 = 'D'; f.ver = 1; f.net = LINK_NET_ID; f.type = type;
  return f;
}
