#pragma once

// A toggle in flight: sent to the Mac, not yet confirmed by Teams.
//
// This is what keeps the Teams screen honest between a tap and its echo. The
// tapped half dims while pending and only changes state when Teams reports the
// new truth - the display is never optimistic about a hot microphone. But Teams
// may never answer (the local API can drop mid-call), so pending EXPIRES rather
// than dimming forever: an expired pending simply resumes showing the last
// reported state, which is still the most honest thing known.
//
// Pure and wrap-safe like Deadline, and separate from it because the semantics
// differ: a Deadline asks "has this elapsed", this asks "am I still waiting" -
// cleared early by confirmation, expired late by silence.

#include <cstdint>

namespace core {

class PendingToggle {
 public:
  // Teams answers in well under a second when it answers at all.
  static constexpr uint32_t EXPIRE_MS = 3000;

  void arm(uint32_t now_ms) {
    armed_ = true;
    at_ms_ = now_ms;
  }

  // Confirmation arrived (the reported state changed): stop waiting.
  void clear() { armed_ = false; }

  bool pending(uint32_t now_ms) const {
    return armed_ && (now_ms - at_ms_) < EXPIRE_MS;
  }

 private:
  bool armed_ = false;
  uint32_t at_ms_ = 0;
};

}  // namespace core
