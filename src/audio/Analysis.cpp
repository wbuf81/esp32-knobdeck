#include "Analysis.h"

#include <cmath>

namespace audio {
namespace {

inline int binOf(float hz) {
  const int b = static_cast<int>(hz / Fft::BIN_HZ + 0.5f);
  return b < 1 ? 1 : (b > Fft::BINS - 1 ? Fft::BINS - 1 : b);
}

// Sum of magnitudes over a frequency range, divided by the bin count so a wide
// band is not automatically louder than a narrow one.
float bandMean(const float *mag, int bins, float lo_hz, float hi_hz) {
  const int lo = binOf(lo_hz);
  int hi = binOf(hi_hz);
  if (hi >= bins) hi = bins - 1;
  if (hi <= lo) return 0.0f;
  float sum = 0.0f;
  for (int i = lo; i < hi; ++i) sum += mag[i];
  return sum / static_cast<float>(hi - lo);
}

inline float smoothTo(float cur, float target, float dt, float attack,
                      float release) {
  const float rate = target > cur ? attack : release;
  float a = rate * dt;
  if (a > 1.0f) a = 1.0f;
  return cur + (target - cur) * a;
}

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

}  // namespace

void BandEnergy::reset() {
  b_ = m_ = t_ = l_ = 0.0f;
  peak_ = 0.02f;
  lpeak_ = 0.02f;
}

void BandEnergy::process(const float *mag, int bins, float dt, float *bass,
                         float *mid, float *treble, float *loudness) {
  const float rb = bandMean(mag, bins, 40.0f, 250.0f);
  const float rm = bandMean(mag, bins, 250.0f, 2000.0f);
  const float rt = bandMean(mag, bins, 2000.0f, 8000.0f);
  const float rl = (rb + rm + rt) * 0.3333f;

  // Track the loudest thing seen recently and normalise against it, decaying so
  // a single loud transient does not flatten the next minute. Absolute
  // calibration is not available - the gain of a MEMS microphone in an unknown
  // room is unknowable - so relative is the only honest scale.
  const float loudest = rb > rm ? (rb > rt ? rb : rt) : (rm > rt ? rm : rt);
  if (loudest > peak_) peak_ = loudest;
  peak_ *= (1.0f - 0.35f * dt);
  if (peak_ < 0.004f) peak_ = 0.004f;
  const float inv = 1.0f / peak_;

  if (rl > lpeak_) lpeak_ = rl;
  lpeak_ *= (1.0f - 0.35f * dt);
  if (lpeak_ < 0.004f) lpeak_ = 0.004f;
  const float linv = 1.0f / lpeak_;

  b_ = smoothTo(b_, clamp01(rb * inv), dt, ATTACK_PER_S, RELEASE_PER_S);
  m_ = smoothTo(m_, clamp01(rm * inv), dt, ATTACK_PER_S, RELEASE_PER_S);
  t_ = smoothTo(t_, clamp01(rt * inv), dt, ATTACK_PER_S, RELEASE_PER_S);
  l_ = smoothTo(l_, clamp01(rl * linv), dt, ATTACK_PER_S, RELEASE_PER_S);

  if (bass) *bass = b_;
  if (mid) *mid = m_;
  if (treble) *treble = t_;
  if (loudness) *loudness = l_;
}

void OnsetDetector::reset() {
  for (int i = 0; i < Fft::BINS; ++i) prev_[i] = 0.0f;
  for (int i = 0; i < HIST; ++i) flux_[i] = 0.0f;
  head_ = 0;
  filled_ = 0;
  since_ = 0;
  primed_ = false;
}

bool OnsetDetector::process(const float *mag, int bins) {
  // Half-wave-rectified spectral flux: only bins that got LOUDER count. Falling
  // energy is not an onset, and counting it makes every note-off fire one.
  float flux = 0.0f;
  for (int i = 1; i < bins; ++i) {
    const float d = mag[i] - prev_[i];
    if (d > 0.0f) flux += d;
    prev_[i] = mag[i];
  }

  if (!primed_) {
    primed_ = true;
    flux_[head_] = flux;
    head_ = (head_ + 1) % HIST;
    if (filled_ < HIST) ++filled_;
    return false;
  }

  // Mean of the window as the adaptive floor. A true median would be better and
  // costs a sort of 43 elements per hop; the mean plus a sensitivity factor has
  // measured close enough on the fixtures, and the difference is smaller than
  // the variation between rooms.
  float sum = 0.0f;
  for (int i = 0; i < filled_; ++i) sum += flux_[i];
  const float mean = filled_ > 0 ? sum / static_cast<float>(filled_) : 0.0f;

  flux_[head_] = flux;
  head_ = (head_ + 1) % HIST;
  if (filled_ < HIST) ++filled_;

  ++since_;
  // A floor as well as a ratio: in near-silence the mean is tiny and any noise
  // clears the ratio test, which would make a quiet room beat steadily.
  const bool loud_enough = flux > 0.02f;
  if (loud_enough && flux > mean * SENSITIVITY && since_ >= MIN_GAP) {
    since_ = 0;
    return true;
  }
  return false;
}

void TempoTracker::reset() {
  n_ = 0;
  head_ = 0;
  since_ = 0.0f;
  bpm_ = 120.0f;
  phase_ = 0.0f;
}

void TempoTracker::process(bool onset, float dt) {
  since_ += dt;

  if (onset) {
    // Ignore intervals outside a plausible musical range rather than letting a
    // spurious onset drag the estimate: 0.3 s is 200 bpm and 1.2 s is 50.
    if (since_ > 0.3f && since_ < 1.2f) {
      intervals_[head_] = since_;
      head_ = (head_ + 1) % TAPS;
      if (n_ < TAPS) ++n_;

      float sum = 0.0f;
      for (int i = 0; i < n_; ++i) sum += intervals_[i];
      const float mean = sum / static_cast<float>(n_);
      if (mean > 0.0001f) bpm_ = 60.0f / mean;
    }
    since_ = 0.0f;
    // Re-align the predicted beat to the one just heard. Without this the phase
    // drifts against the music and every effect keyed to it slowly goes out.
    phase_ = 0.0f;
  }

  const float beats_per_s = bpm_ / 60.0f;
  phase_ += beats_per_s * dt;
  while (phase_ >= 1.0f) phase_ -= 1.0f;
}

}  // namespace audio
