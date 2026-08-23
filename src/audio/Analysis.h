#pragma once

// Turning a spectrum into the three band energies, an onset flag and a tempo.
//
// All three live here because they are one pipeline with one set of tuning
// constants, and splitting them across files made it harder to see that the
// onset threshold and the band smoothing interact.

#include <cstdint>

#include "Fft.h"

namespace audio {

class BandEnergy {
 public:
  // Log-spaced: bass 40-250 Hz, mid 250-2000 Hz, treble 2000-8000 Hz.
  //
  // Attack and release are asymmetric on purpose. A visualiser that rises
  // instantly and falls slowly reads as punchy; the reverse reads as broken,
  // and symmetric smoothing reads as mush.
  static constexpr float ATTACK_PER_S = 26.0f;
  static constexpr float RELEASE_PER_S = 4.2f;

  void reset();
  void process(const float *mag, int bins, float dt, float *bass, float *mid,
               float *treble, float *loudness);

 private:
  float b_ = 0.0f, m_ = 0.0f, t_ = 0.0f, l_ = 0.0f;
  // Running peaks, so the outputs are normalised against how loud this room
  // actually is rather than against an absolute scale that would be wrong
  // everywhere.
  //
  // Loudness gets its OWN peak. Normalising the mean of three bands by the
  // loudest single band caps it at about a third for anything narrowband, so
  // loudness could never reach its full range - and loudness is what drives
  // bloom strength, so the brightest thing in the design would have been
  // permanently dimmed.
  float peak_ = 0.02f;
  float lpeak_ = 0.02f;
};

class OnsetDetector {
 public:
  // Spectral flux against an adaptive threshold: the median of a rolling window
  // times a sensitivity factor. A fixed threshold works on one track and fails
  // on the next, which is worse than having none at all.
  static constexpr int HIST = 43;  // about a second of hops
  static constexpr float SENSITIVITY = 1.7f;
  static constexpr int MIN_GAP = 4;  // hops; suppresses double-triggering

  void reset();
  bool process(const float *mag, int bins);

 private:
  float prev_[Fft::BINS] = {};
  float flux_[HIST] = {};
  int head_ = 0;
  int filled_ = 0;
  int since_ = 0;
  bool primed_ = false;
};

class TempoTracker {
 public:
  void reset();
  // Feed every frame; `onset` true on the frames a beat was detected.
  void process(bool onset, float dt);
  float bpm() const { return bpm_; }
  // A 0..1 sawtooth wrapping on each PREDICTED beat. Effects that want to move
  // with the music rather than react to it use this, because reaction is always
  // at least one frame late and prediction is not.
  float beatPhase() const { return phase_; }

 private:
  static constexpr int TAPS = 8;
  float intervals_[TAPS] = {};
  int n_ = 0;
  int head_ = 0;
  float since_ = 0.0f;
  float bpm_ = 120.0f;
  float phase_ = 0.0f;
};

}  // namespace audio
