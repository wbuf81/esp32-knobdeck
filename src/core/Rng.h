#pragma once

// xorshift32, seeded explicitly and never from a clock.
//
// Effects that need randomness take an Rng by reference rather than calling a
// global. That is what makes a headless replay bit-exact, which is what makes
// the visual assertions stable rather than flaky.

#include <cstdint>

namespace core {

class Rng {
 public:
  explicit Rng(uint32_t seed) : s_(seed ? seed : 0x9E3779B9u) {}

  void reseed(uint32_t seed) { s_ = seed ? seed : 0x9E3779B9u; }

  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }

  // [0, 1). 24 bits of mantissa is more than any effect here can perceive.
  float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }

  float range(float lo, float hi) { return lo + unit() * (hi - lo); }

 private:
  uint32_t s_;
};

}  // namespace core
