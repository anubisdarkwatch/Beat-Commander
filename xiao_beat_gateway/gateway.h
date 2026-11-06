// Types used by xiao_beat_gateway.ino (kept here so the Arduino IDE's auto-prototypes can see them).
#pragma once
#include <stdint.h>
#include "link.h"

struct Ev { LinkFrame f; int64_t t; };                      // one ESP-NOW frame + its arrival time (us)
enum Src : uint8_t { SRC_NONE, SRC_AIR, SRC_BOOTH };       // AIR = W10 heard on LoRa, BOOTH = a controller
