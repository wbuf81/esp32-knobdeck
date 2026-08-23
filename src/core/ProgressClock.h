#pragma once

// The playback position actually shown on screen.
//
// Exists because of a real bug: the UI reads the net thread's published state
// every frame, and copying its progress each time silently overwrote the local
// extrapolation, so the clock only moved when a poll landed — a 2s stutter
// instead of a 1s tick. Resyncing is therefore gated on the publish sequence
// number rather than on merely having read the state again.

#include <cstdint>

class ProgressClock {
 public:
  // Call once per frame with the source's current sequence and position.
  // Returns true when this was genuinely new data and the clock resynced.
  bool sync(uint32_t seq, uint32_t authoritative_ms) {
    if (seen_ && seq == last_seq_) return false;
    seen_ = true;
    last_seq_ = seq;
    value_ = authoritative_ms;
    return true;
  }

  // Advance between publishes so the display ticks every second.
  void advance(uint32_t dt_ms, bool playing, uint32_t duration_ms) {
    if (!playing) return;
    value_ += dt_ms;
    if (duration_ms != 0 && value_ > duration_ms) value_ = duration_ms;
  }

  void reset(uint32_t v) { value_ = v; }
  uint32_t value() const { return value_; }

 private:
  uint32_t value_ = 0;
  uint32_t last_seq_ = 0;
  bool seen_ = false;
};
