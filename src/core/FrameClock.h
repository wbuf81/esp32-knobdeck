#pragma once

// Produces the dt every effect integrates against.
//
// Clamped, because a stall - a page fault on the host, a TLS handshake on the
// device - must not teleport a particle field across the screen. Effects are
// correct at any frame rate; they are not correct across an unbounded jump.

#include <cstdint>

namespace core {

class FrameClock {
 public:
  static constexpr float MAX_DT = 0.100f;

  // Returns seconds since the previous call. The first call returns 0.
  float tick(uint32_t now_ms) {
    if (!started_) {
      started_ = true;
      last_ms_ = now_ms;
      return 0.0f;
    }
    // Unsigned subtraction is correct across the uint32 wrap.
    const uint32_t elapsed = now_ms - last_ms_;
    last_ms_ = now_ms;
    const float dt = static_cast<float>(elapsed) * 0.001f;
    return dt > MAX_DT ? MAX_DT : dt;
  }

 private:
  bool started_ = false;
  uint32_t last_ms_ = 0;
};

}  // namespace core
