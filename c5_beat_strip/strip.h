// Beat Strip patterns: what each LED shows, beat by beat. Pure maths into an RGB buffer, so the host preview
// (tools/preview_strip.cpp) renders exactly what the strip will.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#define STRIP_MAX 300
enum SPattern : uint8_t { SP_PULSE, SP_CHASE, SP_BURST, SP_BAR, SP_SPARKLE, SP_RAINBOW, SP_COUNT };
static const char* const SP_NAME[SP_COUNT] = {"pulse", "chase", "burst", "bar", "sparkle", "rainbow"};

struct SState {
  uint16_t n;                  // LEDs on the strip
  uint8_t pattern;
  bool live;                   // false = no beat: amber breathing
  uint16_t bpm10;
  uint32_t beatMs, beatId;     // when the current beat started (ms), counts every beat
  uint8_t beatPos, hue;        // 0..3 (0 = downbeat), the conductor's colour
  uint32_t bars;
};

static void shue(uint8_t h, float& r, float& g, float& b) {          // hue 0..255 -> full-saturation RGB 0..1
  float x = h / 42.5f; int i = (int)x; float f = x - i;
  switch (i % 6) { case 0: r = 1; g = f; b = 0; break; case 1: r = 1 - f; g = 1; b = 0; break; case 2: r = 0; g = 1; b = f; break;
                   case 3: r = 0; g = 1 - f; b = 1; break; case 4: r = f; g = 0; b = 1; break; default: r = 1; g = 0; b = 1 - f; }
}
static inline float sper(const SState& s) { return 60000.0f / (s.bpm10 > 300 ? s.bpm10 / 10.0f : 120.0f); }
static inline float sage(const SState& s, uint32_t now) { float t = (now - s.beatMs) / sper(s); return t < 0 ? 0 : t; }   // beats since the beat
static inline uint32_t shash(uint32_t a, uint32_t b) { uint32_t h = a * 2654435761u ^ (b + 0x9E3779B9u) * 2246822519u; h ^= h >> 15; h *= 2654435761u; return h ^ (h >> 13); }

static float sTrail[STRIP_MAX];                                      // per-LED afterglow for chase / sparkle
static uint32_t sLastMs = 0;

// rgb: n * 3 bytes, full scale (brightness and the power limit are applied afterwards)
void stripRender(uint8_t* rgb, const SState& s, uint32_t now) {
  int n = s.n > STRIP_MAX ? STRIP_MAX : s.n;
  float dt = sLastMs ? (now - sLastMs) / 1000.0f : 0; sLastMs = now; if (dt > 0.2f) dt = 0.2f;
  float r, g, b; shue(s.hue, r, g, b);
  auto put = [&](int i, float rr, float gg, float bb) {
    if (i < 0 || i >= n) return;
    rgb[i * 3] = (uint8_t)(fminf(rr, 1) * 255); rgb[i * 3 + 1] = (uint8_t)(fminf(gg, 1) * 255); rgb[i * 3 + 2] = (uint8_t)(fminf(bb, 1) * 255);
  };
  memset(rgb, 0, n * 3);

  if (!s.live) {                                                     // waiting: slow amber breathing
    float v = 0.04f + 0.10f * (0.5f - 0.5f * cosf(now / 1600.0f * 6.2832f));
    for (int i = 0; i < n; i++) put(i, v, v * 0.69f, 0);
    return;
  }
  float age = sage(s, now);
  bool down = s.beatPos == 0;
  float punch = expf(-age * (down ? 3.5f : 5.0f));                  // 1 on the beat, decays within the beat
  float white = down ? punch * 0.35f : 0;                            // beat 1 gets a white edge

  switch (s.pattern) {
    case SP_PULSE: {                                                 // the whole strip hits and fades
      float v = 0.06f + 0.94f * punch;
      for (int i = 0; i < n; i++) put(i, r * v + white, g * v + white, b * v + white);
      break;
    }
    case SP_CHASE: {                                                 // a comet runs the strip once per beat
      float pos = fminf(age, 1.0f) * (n + 6);
      bool rev = s.bars & 1;                                         // direction flips every bar
      float decay = expf(-dt * 9.0f);
      for (int i = 0; i < n; i++) sTrail[i] *= decay;
      int head = (int)pos;
      for (int k = 0; k < 3; k++) { int i = head - k; if (i >= 0 && i < n) sTrail[rev ? n - 1 - i : i] = 1.0f; }
      for (int i = 0; i < n; i++) { float v = sTrail[i]; put(i, r * v + (down ? v * 0.25f : 0), g * v + (down ? v * 0.25f : 0), b * v + (down ? v * 0.25f : 0)); }
      break;
    }
    case SP_BURST: {                                                 // rings out from the centre
      float c = (n - 1) / 2.0f, reach = fminf(age * 1.6f, 1.0f) * (c + 3);
      for (int i = 0; i < n; i++) {
        float d = fabsf(i - c), e = fabsf(d - reach);
        float fade = 1 - fminf(age, 1);
        float v = e < 5 ? (1 - e / 5) * (0.4f + 0.6f * fade) : 0;          // the travelling ring
        if (d < reach) v = fmaxf(v, 0.35f * fade * fade);                     // what it leaves behind
        put(i, r * v + white * v, g * v + white * v, b * v + white * v);
      }
      break;
    }
    case SP_BAR: {                                                   // four sections = the 1-2-3-4 of the bar
      int seg = n / 4 > 0 ? n / 4 : 1;
      for (int i = 0; i < n; i++) {
        int sIdx = i / seg; if (sIdx > 3) sIdx = 3;
        float v = sIdx < s.beatPos ? 0.18f : sIdx == s.beatPos ? 0.25f + 0.75f * punch : 0.02f;
        put(i, r * v + (sIdx == 0 && down ? white : 0), g * v + (sIdx == 0 && down ? white : 0), b * v + (sIdx == 0 && down ? white : 0));
      }
      break;
    }
    case SP_SPARKLE: {                                               // random LEDs catch every beat
      static uint32_t lastId = 0;
      float decay = expf(-dt * 4.0f);
      for (int i = 0; i < n; i++) sTrail[i] *= decay;
      if (s.beatId != lastId) {
        lastId = s.beatId;
        int count = n / (down ? 3 : 6) + 1;
        for (int k = 0; k < count; k++) sTrail[shash(s.beatId, k) % n] = 1.0f;
      }
      for (int i = 0; i < n; i++) { float v = sTrail[i]; float w = down ? v * 0.3f : 0; put(i, r * v + w, g * v + w, b * v + w); }
      break;
    }
    default: {                                                       // RAINBOW: scrolls a step every beat
      float base = s.beatId * 16 + fminf(age, 1) * 16;
      float v = 0.25f + 0.75f * punch;
      for (int i = 0; i < n; i++) {
        float rr, gg, bb; shue((uint8_t)(base + i * 256.0f / (n < 2 ? 2 : n)), rr, gg, bb);
        put(i, rr * v + white, gg * v + white, bb * v + white);
      }
      break;
    }
  }
}
