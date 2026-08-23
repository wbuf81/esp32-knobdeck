#pragma once

// A wrap-safe deadline.
//
// millis() is a uint32_t and wraps to zero after 49.7 days. Comparing an
// absolute future timestamp — `if (now < next_poll_ms)` — breaks the moment it
// does: `now` restarts near zero while the stored deadline is still near 2^32,
// so the comparison stays true forever. In this project that meant polling
// would stop permanently and the device would sit on one track until it was
// unplugged. A device meant to be left plugged in reaches that point on a
// timer, not by chance.
//
// Unsigned subtraction is correct across the wrap: (now - start) yields the
// true elapsed interval even when now < start. Everything here is expressed
// that way, so there is one place to get it right instead of six.

#include <cstdint>

class Deadline {
 public:
  void arm(uint32_t now_ms, uint32_t in_ms) {
    start_ms_ = now_ms;
    duration_ms_ = in_ms;
    armed_ = true;
  }

  void disarm() { armed_ = false; }
  bool armed() const { return armed_; }

  // Still waiting.
  bool pending(uint32_t now_ms) const {
    return armed_ && (now_ms - start_ms_) < duration_ms_;
  }

  // Elapsed, or never armed.
  bool elapsed(uint32_t now_ms) const { return !pending(now_ms); }

  uint32_t elapsedMs(uint32_t now_ms) const { return now_ms - start_ms_; }

  // How much longer this has to run. Used to compare two deadlines without
  // comparing absolute timestamps, which is the whole bug this class exists
  // to remove.
  uint32_t remainingMs(uint32_t now_ms) const {
    if (!armed_) return 0;
    const uint32_t e = now_ms - start_ms_;
    return e >= duration_ms_ ? 0 : duration_ms_ - e;
  }

 private:
  uint32_t start_ms_ = 0;
  uint32_t duration_ms_ = 0;
  bool armed_ = false;
};
