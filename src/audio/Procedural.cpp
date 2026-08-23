#include "Procedural.h"

#include <cmath>

namespace audio {
namespace {

constexpr float TAU = 6.28318530718f;

inline float unit(uint32_t v) {
  return static_cast<float>(v >> 8) * (1.0f / 16777216.0f);
}

// 0..1 from a sine, so band values never leave range regardless of how the
// oscillators are combined.
inline float half(float s) { return 0.5f + 0.5f * s; }

}  // namespace

void Procedural::reseed(uint32_t track_seed) {
  seed_ = track_seed ? track_seed : 0x9E3779B9u;
  // A plausible tempo, stable for a given track. Real music mostly lives in
  // this range, and a track that always pulses at the same rate feels like it
  // has an opinion about itself.
  bpm_ = 84.0f + unit(seed_ * 2654435761u) * 64.0f;
  clock_ = 0.0f;
  phase_ = 0.0f;
  // Independent starting phases, or the three bands move in lockstep and the
  // whole thing reads as one oscillator driving everything - which it is, but
  // it should not look like it.
  p1_ = unit(seed_ * 40503u) * TAU;
  p2_ = unit(seed_ * 22695477u) * TAU;
  p3_ = unit(seed_ * 69069u) * TAU;
}

void Procedural::fill(Modulation *m, float dt) {
  if (!m) return;
  clock_ += dt;

  const float beats_per_s = bpm_ / 60.0f;
  const float prev = phase_;
  phase_ += beats_per_s * dt;
  m->onset = phase_ >= 1.0f && prev < 1.0f ? true : false;
  while (phase_ >= 1.0f) {
    phase_ -= 1.0f;
    m->onset = true;
  }
  m->beat_phase = phase_;

  // Two detuned sines per band, at rates an octave or so apart, so each band
  // has its own character rather than being a scaled copy of the others.
  const float t = clock_;
  const float b = half(std::sin(t * 1.7f + p1_)) * 0.65f +
                  half(std::sin(t * 0.41f + p1_ * 1.7f)) * 0.35f;
  const float mi = half(std::sin(t * 2.6f + p2_)) * 0.55f +
                   half(std::sin(t * 0.83f + p2_ * 1.3f)) * 0.45f;
  const float tr = half(std::sin(t * 4.3f + p3_)) * 0.5f +
                   half(std::sin(t * 1.31f + p3_ * 2.1f)) * 0.5f;

  // A decaying pulse on each beat, so the bass reads as a kick rather than as a
  // slow swell. Without this the procedural path looks like a lava lamp.
  const float kick = (1.0f - phase_) * (1.0f - phase_);

  m->bass = b * 0.55f + kick * 0.45f;
  m->mid = mi;
  m->treble = tr;
  m->loudness = m->bass * 0.45f + m->mid * 0.35f + m->treble * 0.20f;
  m->live = false;
}

}  // namespace audio
