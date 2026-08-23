#pragma once

// Microphone to Modulation: the whole chain, plus the decision about whether to
// trust it.
//
// The microphone hears the ROOM, not Spotify's stream. On desk speakers that is
// the same thing and the reactivity is genuine. On headphones the microphone
// hears nothing, and a visualiser keyed to it would simply freeze - which is the
// failure the ancestor project warned about when it deleted its spectrum
// analyser: anything shaped like an equaliser looks broken the moment it stops
// agreeing with what you can hear.
//
// So silence is detected explicitly and crossfaded to procedural modulation. The
// fallback is not a degraded mode to be tolerated; it is what a headphone
// listener sees every time, and it has to stand on its own.

#include <cstdint>

#include "Analysis.h"
#include "Fft.h"
#include "MicSource.h"
#include "Modulation.h"
#include "Procedural.h"

namespace audio {

class AudioAnalyzer {
 public:
  // 256 samples at 16 kHz is a 16 ms hop, so there are one or two per rendered
  // frame. More than two are dropped rather than queued: falling behind on
  // audio analysis is better than stalling the render loop to catch up.
  static constexpr int HOP = 256;
  static constexpr int MAX_HOPS_PER_FRAME = 2;

  // Silence is decided on a DECAYING PEAK of RMS, not on instantaneous RMS.
  //
  // Instantaneous RMS is the obvious choice and it is wrong. Percussive music is
  // mostly quiet: a 120 bpm click track is 5 ms of sound every 500 ms, so an
  // instantaneous test reads "silent" for 99% of it and the analyzer flickers
  // between live and procedural in the gaps. Music has rests; a level detector
  // has to remember.
  static constexpr float SILENCE_RMS = 0.006f;
  // Time constant of the peak's decay. Long enough to ride through a gap
  // between tracks, short enough that walking away is noticed within seconds.
  static constexpr float PEAK_DECAY_S = 2.5f;
  // How long the peak must stay under the floor before handing over, and how
  // long the handover itself takes.
  static constexpr float QUIET_HOLD_S = 0.7f;
  static constexpr float CROSSFADE_S = 0.6f;

  void begin(MicSource *mic);
  void setTrack(uint32_t track_seed);

  // Fills the audio-derived fields of `m`. Leaves progress01, volume01 and
  // track_seed alone: those belong to playback state, not to audio.
  void update(Modulation *m, float dt);

  bool live() const { return mix_ > 0.5f; }
  float bpm() const { return tempo_.bpm(); }

 private:
  void runHop();

  MicSource *mic_ = nullptr;
  Fft fft_;
  BandEnergy bands_;
  OnsetDetector onset_;
  TempoTracker tempo_;
  Procedural fallback_;

  // A plain shift-down buffer rather than a circular index. At 512 samples the
  // move is trivial, and off-by-one errors in a circular FFT input buffer are
  // both easy to write and very hard to see.
  float ring_[Fft::N] = {};
  int filled_ = 0;
  float mag_[Fft::BINS] = {};
  float scratch_[HOP] = {};

  float live_bass_ = 0.0f, live_mid_ = 0.0f, live_treble_ = 0.0f;
  float live_loud_ = 0.0f;
  bool live_onset_ = false;
  float mix_ = 0.0f;  // 0 fully procedural, 1 fully live
  float quiet_s_ = 0.0f;
  float rms_ = 0.0f;
  float rms_peak_ = 0.0f;
  uint32_t seed_ = 0;
};

}  // namespace audio
