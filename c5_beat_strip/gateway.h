// Types used by the gateway (from xiao_beat_gateway/gateway.h; GS_ names so they don't clash with viz.h). Was: types used by xiao_beat_gateway.ino (kept here so the Arduino IDE's auto-prototypes can see them).
#pragma once
#include <stdint.h>
#include "link.h"

struct Ev { LinkFrame f; int64_t t; };                      // one ESP-NOW frame + its arrival time (us)
enum Src : uint8_t { GS_NONE, GS_AIR, GS_BOOTH };       // AIR = W10 heard on LoRa, BOOTH = a controller
