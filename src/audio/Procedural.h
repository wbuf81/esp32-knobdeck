#pragma once

// Modulation generated from track progress and a seed, with no audio at all.
//
// This is not a degraded mode to be tolerated. The ancestor project's ambient
// scenes ran entirely on inputs like these and looked good, and this is the path
// a headphone listener sees every time - so it has to stand on its own.
//
// Everything is derived from the seed, so a given track always pulses at the
// same tempo and moves the same way. Deterministic, which also means the visual
// tests can assert on it.

#include <cstdint>

#include "Modulation.h"

namespace audio {

class Procedural {
 public:
  void reseed(uint32_t track_seed);
  // Advances the oscillators and fills the audio-derived fields of `m`.
  void fill(Modulation *m, float dt);
  float bpm() const { return bpm_; }

 private:
  float clock_ = 0.0f;
  float bpm_ = 112.0f;
  float phase_ = 0.0f;
  float p1_ = 0.0f, p2_ = 0.0f, p3_ = 0.0f;  // per-band oscillator phases
  uint32_t seed_ = 0;
};

}  // namespace audio
