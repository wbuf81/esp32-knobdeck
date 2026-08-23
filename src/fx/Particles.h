#pragma once

// An additive particle field drawn as motion streaks.
//
// Streaks rather than points, and no persistence buffer. That was forced by
// measurement and turned out to be the better look anyway: a trail buffer needs
// a whole frame of storage plus a read-fade-write pass over it, which on this
// board costs about 12 ms per frame in PSRAM. Drawing each particle as a segment
// from where it was to where it is costs only the pixels it covers, and a crisp
// motion streak reads better than a uniform frame-wide fade.
//
// Structure of arrays: update() touches position and velocity only, and keeping
// those contiguous is the difference between streaming cleanly and thrashing the
// cache on every particle.
//
// Nothing here allocates. The pool is fixed and emit() silently drops when full,
// because dropping a particle is always preferable to growing a buffer mid-frame
// on a device whose heap fragments.

#include <cstdint>

#include "core/Rng.h"
#include "gfx/Surface.h"

namespace fx {

struct SpawnParams {
  float x = 180.0f, y = 180.0f;  // origin, screen pixels
  float spread = 6.0f;           // origin jitter radius
  float speed_min = 12.0f, speed_max = 60.0f;
  float life_min = 0.8f, life_max = 2.6f;
  float size_min = 1.0f, size_max = 2.6f;
  float drag = 0.86f;      // per-second velocity multiplier
  float gravity_y = 0.0f;  // pixels/s^2, positive is downward
  uint16_t colors[16] = {};
  int color_count = 0;
};

class Particles {
 public:
  static constexpr int MAX = 1000;
  // Streak length is capped so one very fast particle cannot cost a whole
  // frame; beyond about a dozen steps the eye reads it as a line anyway.
  static constexpr int MAX_STEPS = 14;

  void configure(const SpawnParams &p) { p_ = p; }
  const SpawnParams &params() const { return p_; }

  void emit(int n, core::Rng &rng);
  // Radial burst at the configured origin. Used on beat onsets.
  void burst(int n, float speed_scale, core::Rng &rng);
  void update(float dt);
  // Additive, so overlapping particles read as light rather than occlusion.
  void render(gfx::Surface &s) const;

  int live() const { return live_; }
  void clear() { live_ = 0; }

 private:
  void spawnOne(core::Rng &rng, float angle, float speed, float life_scale);

  float x_[MAX], y_[MAX];
  float px_[MAX], py_[MAX];  // previous position: the streak's tail
  float vx_[MAX], vy_[MAX];
  float life_[MAX], life0_[MAX], size_[MAX];
  uint16_t col_[MAX];
  int live_ = 0;
  SpawnParams p_;
};

}  // namespace fx
