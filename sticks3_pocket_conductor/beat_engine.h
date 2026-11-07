// Beat engine from the Beat Commander W10 conductor (w10_beat_conductor.ino), unchanged apart from the scope size.
// Onset strength (log-energy rise, kick band + full band) every 8 ms hop, tempo by autocorrelation over ~6 s,
// beat clock phase-locked to strong onsets. No hardware: feed processHop() 128 mono samples at 16 kHz.
#pragma once
#include <math.h>
#include <string.h>
#include <stdint.h>
#ifndef SCOPE_N
#define SCOPE_N 135                   // onset scope width (pixels)
#endif
#define AUD_RATE 16000
float MIC_GATE = 0.0015f;             // below this RMS level: silence, no onsets
// Onset strength (log-energy rise, kick band + full band) every 8 ms hop,
// tempo by autocorrelation over ~6 s, beat clock phase-locked to strong onsets.
#define HOP 128                       // 8 ms at 16 kHz
#define OSS_N 768                     // 6.1 s of onset strength
#define HOP_MS 8.0f
float oss[OSS_N]; int ossPos = 0; uint32_t hops = 0;
float lp = 0, prevLow = -100, prevFull = -100, level = 0;
float ossMean = 0, ossVar = 1e-4f;
uint32_t lastOnsetMs = 0;
float bpm = 120.0f, bpmEst = 0, conf = 0;
float estHist[3] = {0, 0, 0}; int estN = 0;
uint32_t nextBeat = 0; volatile int32_t phaseAdj = 0;
int beatNum = 0;                      // beat in bar = beatNum % 4
uint32_t tapTimes[5]; int tapCount = 0; uint32_t tapLock = 0;
int onsetsSec = 0, onsetCount = 0;
float onsetHistory[SCOPE_N]; int ohPos = 0; bool ohOnset[SCOPE_N];

void estimateTempo(uint32_t tNow) {
  static float x[OSS_N];
  float m = 0; for (int i = 0; i < OSS_N; i++) m += oss[i]; m /= OSS_N;
  for (int i = 0; i < OSS_N; i++) x[i] = oss[(ossPos + i) % OSS_N] - m;   // oldest -> newest
  float r0 = 0; for (int i = 0; i < OSS_N; i++) r0 += x[i] * x[i];
  if (r0 < 1e-6f) { conf = 0; return; }
  const int Lmin = 39, Lmax = 108;    // 190 .. 70 BPM at 8 ms hops
  static float sc[120];
  auto acf = [&](int L) { float r = 0; for (int i = L; i < OSS_N; i++) r += x[i] * x[i - L]; return r; };
  for (int L = Lmin - 1; L <= Lmax + 1; L++) {
    float r = acf(L) + 0.5f * acf(2 * L) + 0.33f * acf(3 * L) + 0.25f * acf(4 * L); // beat-period harmonics
    float b = 60000.0f / (L * HOP_MS);
    float o = log2f(b / 125.0f);                          // mild prior toward dance tempos
    sc[L] = r * expf(-0.5f * (o * o) / (0.45f * 0.45f));
  }
  int best = Lmin; for (int L = Lmin; L <= Lmax; L++) if (sc[L] > sc[best]) best = L;
  float a = sc[best - 1], b = sc[best], c = sc[best + 1];
  float d = (a - 2 * b + c) != 0 ? 0.5f * (a - c) / (a - 2 * b + c) : 0;   // parabolic peak
  if (d > 0.5f) d = 0.5f; if (d < -0.5f) d = -0.5f;
  bpmEst = 60000.0f / ((best + d) * HOP_MS);
  conf = b / (2.08f * r0);
  if (conf < 0.08f) return;
  estHist[estN % 3] = bpmEst; estN++;
  if (estN >= 3) {                                        // median of last 3 estimates
    float e0 = estHist[0], e1 = estHist[1], e2 = estHist[2];
    float med = fmaxf(fminf(e0, e1), fminf(fmaxf(e0, e1), e2));
    if (fabsf(med - bpm) / bpm > 0.04f) bpm = med;        // big change: jump
    else bpm = bpm * 0.8f + med * 0.2f;                   // small: glide
  }

  // phase: fold recent onset strength at the beat period, align beat clock to the strongest slot
  {
    float P = 60000.0f / bpm / HOP_MS;                    // period in hops
    int Pi = (int)(P + 0.5f);
    float bestS = -1; int bestK = 0;
    for (int k = 0; k < Pi; k++) {
      float sum = 0;
      for (int j = 0; ; j++) {
        int back = k + (int)(j * P + 0.5f);
        if (back >= OSS_N / 2) break;                     // last ~3 s
        int idx = (ossPos - 1 - back + OSS_N) % OSS_N;
        { float c = oss[idx] + 0.5f * oss[(idx + OSS_N - 1) % OSS_N] + 0.5f * oss[(idx + 1) % OSS_N]; sum += c * c; }  // kicks dominate
      }
      if (sum > bestS) { bestS = sum; bestK = k; }
    }
    float period = 60000.0f / bpm;
    float lastBeatMs = (float)tNow - bestK * HOP_MS - HOP_MS;   // onset lags the kick by ~1 hop
    float err = fmodf((float)(int32_t)(nextBeat - (uint32_t)lastBeatMs), period);  // where our clock sits vs the kick grid
    if (err > period / 2) err -= period;
    if (err < -period / 2) err += period;
    if (fabsf(err) > period * 0.15f) phaseAdj -= (int32_t)err;      // wrong phase: snap
    else phaseAdj -= (int32_t)(err * 0.5f);                         // close: glide
  }
}

// one 8 ms hop of mono samples taken at audio time tMs (millis domain)
void processHop(const int16_t* x, int n, uint32_t tMs) {
  const float a = 0.057f;             // ~150 Hz low-pass: kick band
  float el = 0, ef = 0;
  for (int i = 0; i < n; i++) { float s = x[i] / 32768.0f; lp += a * (s - lp); el += lp * lp; ef += s * s; }
  el /= n; ef /= n;
  level = level * 0.8f + sqrtf(ef) * 0.2f;
  float ldb = 10 * log10f(el + 1e-10f), fdb = 10 * log10f(ef + 1e-10f);
  float o = fmaxf(0, ldb - prevLow) + 0.5f * fmaxf(0, fdb - prevFull);
  prevLow = ldb; prevFull = fdb;
  if (level < MIC_GATE) o = 0;         // silence: no onsets
  oss[ossPos] = o; ossPos = (ossPos + 1) % OSS_N; hops++;
  ossMean = ossMean * 0.99f + o * 0.01f;
  float dv = o - ossMean; ossVar = ossVar * 0.99f + dv * dv * 0.01f;
  float thr = ossMean + 2.0f * sqrtf(ossVar);
  bool on = false;
  if (o > thr && o > 1.5f && tMs - lastOnsetMs > 220) {
    on = true; lastOnsetMs = tMs; onsetCount++;
  }
  if ((hops & 1) == 0) {              // scope at 62 Hz
    float v = o / 12.0f; if (v > 1) v = 1;
    onsetHistory[ohPos] = v; ohOnset[ohPos] = on; ohPos = (ohPos + 1) % SCOPE_N;
  } else if (on) ohOnset[(ohPos + SCOPE_N - 1) % SCOPE_N] = true;
  if (hops % 64 == 0 && tMs > tapLock) estimateTempo(tMs);   // every ~0.5 s
}
void tap(uint32_t now) {
  if (tapCount && now - tapTimes[tapCount - 1] > 2000) tapCount = 0;
  if (tapCount == 5) { memmove(tapTimes, tapTimes + 1, 4 * sizeof(uint32_t)); tapCount = 4; }
  tapTimes[tapCount++] = now;
  if (tapCount >= 2) {
    float avg = (float)(tapTimes[tapCount - 1] - tapTimes[0]) / (tapCount - 1);
    bpm = 60000.0f / avg; tapLock = now + 15000; estN = 0;
  }
  if (tapCount == 1) beatNum = -1;    // first tap = downbeat
  nextBeat = now;                     // fire on the tap
}

