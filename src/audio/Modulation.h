#pragma once

// The modulation bus: the one struct every effect reads.
//
// Effects see this and nothing else - not the microphone, not playback state,
// not a clock. That indirection is why the whole engine is deterministic enough
// to assert on frame-by-frame, and why a microphone that hears nothing degrades
// into something that still looks intentional instead of freezing.

#include <cstdint>

namespace audio {

struct Modulation {
  float bass = 0.0f;     // 0..1, smoothed band energy
  float mid = 0.0f;
  float treble = 0.0f;
  float loudness = 0.0f;
  bool onset = false;       // a beat was detected on this frame
  float beat_phase = 0.0f;  // 0..1 sawtooth, phase-locked to tracked tempo
  float progress01 = 0.0f;  // position through the current track
  float volume01 = 0.7f;
  uint32_t track_seed = 0;
  bool live = false;        // the mic is genuinely hearing music
};

}  // namespace audio
