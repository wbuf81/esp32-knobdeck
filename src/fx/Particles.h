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
//
// Capacity is a template parameter because the pools live in globals, which is
// internal RAM on the device. One 1000-slot pool is ~38KB and the board carried
// it fine; a second one starved the TLS stack - every Spotify handshake after
// it died with MEMORY ALLOCATION FAILED while the screen rendered beautifully
// at 121 fps. Size each pool to its worst case, not to a round number.

#include <cmath>
#include <cstdint>

#include "core/Rng.h"
#include "gfx/Blend.h"
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

namespace detail {
// Two pi, spelled out so the header does not depend on M_PI being defined.
constexpr float PARTICLE_TAU = 6.28318530718f;
}  // namespace detail

template <int CAP>
class BasicParticles {
 public:
  static constexpr int MAX = CAP;
  // Streak length is capped so one very fast particle cannot cost a whole
  // frame; beyond about a dozen steps the eye reads it as a line anyway.
  static constexpr int MAX_STEPS = 14;

  void configure(const SpawnParams &p) { p_ = p; }
  // Moves the emission point without re-configuring. Called every frame to keep
  // the field anchored to the cover, which is why it is two stores rather than a
  // copy of SpawnParams and its sixteen palette entries.
  void setOrigin(float x, float y) {
    p_.x = x;
    p_.y = y;
  }
  const SpawnParams &params() const { return p_; }

  void emit(int n, core::Rng &rng) {
    for (int k = 0; k < n; ++k) {
      spawnOne(rng, rng.range(0.0f, detail::PARTICLE_TAU),
               rng.range(p_.speed_min, p_.speed_max), 1.0f);
    }
  }

  // Radial burst at the configured origin. Used on beat onsets.
  void burst(int n, float speed_scale, core::Rng &rng) {
    if (n <= 0) return;
    // Evenly spaced angles with a jittered phase, rather than n random angles:
    // a random burst clumps visibly at this count, and a clumped burst reads as
    // a bug rather than as an explosion.
    const float phase = rng.range(0.0f, detail::PARTICLE_TAU);
    for (int k = 0; k < n; ++k) {
      const float a = phase +
                      detail::PARTICLE_TAU * (static_cast<float>(k) / n) +
                      rng.range(-0.12f, 0.12f);
      spawnOne(rng, a,
               rng.range(p_.speed_min, p_.speed_max) * speed_scale * 3.0f,
               0.7f);
    }
  }

  // The burst's mirror: spawns on a ring of `radius` around the origin with
  // velocity pointing inward, so the field converges rather than scatters.
  // Speeds and lives come from the configured params; `spread` jitters the
  // spawn radius rather than the origin.
  void implode(int n, float radius, core::Rng &rng) {
    if (n <= 0) return;
    // Evenly spaced with a jittered phase, same reasoning as burst: random
    // angles clump at these counts, and a clumped ring reads as a bug.
    const float phase = rng.range(0.0f, detail::PARTICLE_TAU);
    for (int k = 0; k < n; ++k) {
      if (live_ >= MAX) return;  // drop rather than grow
      const int i = live_++;
      const float a = phase +
                      detail::PARTICLE_TAU * (static_cast<float>(k) / n) +
                      rng.range(-0.12f, 0.12f);
      const float rr = radius + rng.range(-p_.spread, p_.spread);
      x_[i] = p_.x + std::cos(a) * rr;
      y_[i] = p_.y + std::sin(a) * rr;
      px_[i] = x_[i];
      py_[i] = y_[i];
      const float speed = rng.range(p_.speed_min, p_.speed_max);
      vx_[i] = -std::cos(a) * speed;
      vy_[i] = -std::sin(a) * speed;
      const float life = rng.range(p_.life_min, p_.life_max);
      life_[i] = life;
      life0_[i] = life > 0.0001f ? life : 0.0001f;
      size_[i] = rng.range(p_.size_min, p_.size_max);
      col_[i] = p_.color_count > 0
                    ? p_.colors[rng.next() %
                                static_cast<uint32_t>(p_.color_count)]
                    : 0xFFFF;
    }
  }

  void update(float dt) {
    // Exponential drag, evaluated per frame rather than per second, so the
    // motion is frame-rate independent.
    const float drag = std::pow(p_.drag, dt);
    int i = 0;
    while (i < live_) {
      life_[i] -= dt;
      if (life_[i] <= 0.0f) {
        // Compact by swapping the last live particle down. No gaps to skip and
        // no allocation; the cost is that draw order is not spawn order, which
        // for additive blending does not matter.
        const int last = --live_;
        if (i != last) {
          x_[i] = x_[last];   y_[i] = y_[last];
          px_[i] = px_[last]; py_[i] = py_[last];
          vx_[i] = vx_[last]; vy_[i] = vy_[last];
          life_[i] = life_[last];
          life0_[i] = life0_[last];
          size_[i] = size_[last];
          col_[i] = col_[last];
        }
        continue;  // re-test the particle just swapped in
      }
      px_[i] = x_[i];
      py_[i] = y_[i];
      vy_[i] += p_.gravity_y * dt;
      vx_[i] *= drag;
      vy_[i] *= drag;
      x_[i] += vx_[i] * dt;
      y_[i] += vy_[i] * dt;
      ++i;
    }
  }

  // Additive, so overlapping particles read as light rather than occlusion.
  void render(gfx::Surface &s) const {
    using gfx::addSat;
    using gfx::fade;

    for (int i = 0; i < live_; ++i) {
      const float hx = x_[i], hy = y_[i];    // head: where it is now
      const float tx = px_[i], ty = py_[i];  // tail: where it was last frame

      const int half = static_cast<int>(size_[i] * 0.5f);
      const float ylo = hy < ty ? hy : ty;
      const float yhi = hy > ty ? hy : ty;
      const int top = static_cast<int>(ylo) - half;
      const int bot = static_cast<int>(yhi) + half + 1;  // exclusive
      // One test rejects a whole streak that misses this band, which is what
      // makes rendering 2,400 particles into nine bands affordable.
      if (s.rejectsRows(top, bot)) continue;

      // Brightness follows remaining life, so a particle dims out instead of
      // disappearing - an abrupt vanish reads as a glitch.
      const float lifeq = life_[i] / life0_[i];
      const uint8_t life_mul =
          static_cast<uint8_t>(lifeq > 1.0f ? 255.0f : lifeq * 255.0f);

      const float dx = hx - tx;
      const float dy = hy - ty;
      const float adx = dx < 0 ? -dx : dx;
      const float ady = dy < 0 ? -dy : dy;
      int steps = static_cast<int>((adx > ady ? adx : ady) + 0.5f);
      if (steps < 1) steps = 1;
      if (steps > MAX_STEPS) steps = MAX_STEPS;

      const uint16_t base = col_[i];
      const float inv = 1.0f / static_cast<float>(steps);

      for (int k = 0; k <= steps; ++k) {
        const float t = static_cast<float>(k) * inv;
        const int cx = static_cast<int>(tx + dx * t);
        const int cy = static_cast<int>(ty + dy * t);

        // The head is full strength and the tail is a third of it, which is
        // what makes a streak read as motion rather than as a smear.
        const float taper = 0.34f + 0.66f * t;
        const uint8_t mul =
            static_cast<uint8_t>(static_cast<float>(life_mul) * taper);
        const uint16_t c = fade(base, mul);

        const int yl = cy - half, yh = cy + half;
        int xl = cx - half, xh = cx + half;
        // REJECT off-screen, do not clamp.
        //
        // Clamping an entirely off-screen square to [0, W-1] collapses it onto
        // column 0 or 359, so every particle that drifts past the left or
        // right of the disc leaves a bright dot stuck on the edge. Clipping
        // has to discard, not squash.
        if (xh < 0 || xl >= gfx::W) continue;
        if (xl < 0) xl = 0;
        if (xh >= gfx::W) xh = gfx::W - 1;
        for (int yy = yl; yy <= yh; ++yy) {
          if (!s.containsRow(yy)) continue;
          uint16_t *r = s.row(yy);
          for (int xx = xl; xx <= xh; ++xx) r[xx] = addSat(r[xx], c);
        }
      }
    }
  }

  int live() const { return live_; }
  void clear() { live_ = 0; }

 private:
  void spawnOne(core::Rng &rng, float angle, float speed, float life_scale) {
    if (live_ >= MAX) return;  // drop rather than grow
    const int i = live_++;

    const float jr = rng.range(0.0f, p_.spread);
    const float ja = rng.range(0.0f, detail::PARTICLE_TAU);
    x_[i] = p_.x + std::cos(ja) * jr;
    y_[i] = p_.y + std::sin(ja) * jr;
    // Tail starts coincident with the head, so a particle's first frame is a
    // dot rather than a streak from wherever this slot's previous occupant
    // died.
    px_[i] = x_[i];
    py_[i] = y_[i];

    vx_[i] = std::cos(angle) * speed;
    vy_[i] = std::sin(angle) * speed;

    const float life = rng.range(p_.life_min, p_.life_max) * life_scale;
    life_[i] = life;
    life0_[i] = life > 0.0001f ? life : 0.0001f;
    size_[i] = rng.range(p_.size_min, p_.size_max);
    col_[i] = p_.color_count > 0
                  ? p_.colors[rng.next() % static_cast<uint32_t>(p_.color_count)]
                  : 0xFFFF;
  }

  float x_[MAX], y_[MAX];
  float px_[MAX], py_[MAX];  // previous position: the streak's tail
  float vx_[MAX], vy_[MAX];
  float life_[MAX], life0_[MAX], size_[MAX];
  uint16_t col_[MAX];
  int live_ = 0;
  SpawnParams p_;
};

// The music views' pool: sized for a full-screen field on the beat.
using Particles = BasicParticles<1000>;

}  // namespace fx
