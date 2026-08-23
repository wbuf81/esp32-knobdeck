#include "Particles.h"

#include <cmath>

#include "gfx/Blend.h"

namespace fx {
namespace {

// Two pi, spelled out so the file does not depend on M_PI being defined.
constexpr float TAU = 6.28318530718f;

inline int iclamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

void Particles::spawnOne(core::Rng &rng, float angle, float speed,
                         float life_scale) {
  if (live_ >= MAX) return;  // drop rather than grow
  const int i = live_++;

  const float jr = rng.range(0.0f, p_.spread);
  const float ja = rng.range(0.0f, TAU);
  x_[i] = p_.x + std::cos(ja) * jr;
  y_[i] = p_.y + std::sin(ja) * jr;
  // Tail starts coincident with the head, so a particle's first frame is a dot
  // rather than a streak from wherever this slot's previous occupant died.
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

void Particles::emit(int n, core::Rng &rng) {
  for (int k = 0; k < n; ++k) {
    spawnOne(rng, rng.range(0.0f, TAU),
             rng.range(p_.speed_min, p_.speed_max), 1.0f);
  }
}

void Particles::burst(int n, float speed_scale, core::Rng &rng) {
  if (n <= 0) return;
  // Evenly spaced angles with a jittered phase, rather than n random angles: a
  // random burst clumps visibly at this count, and a clumped burst reads as a
  // bug rather than as an explosion.
  const float phase = rng.range(0.0f, TAU);
  for (int k = 0; k < n; ++k) {
    const float a = phase + TAU * (static_cast<float>(k) / n) +
                    rng.range(-0.12f, 0.12f);
    spawnOne(rng, a,
             rng.range(p_.speed_min, p_.speed_max) * speed_scale * 3.0f,
             0.7f);
  }
}

void Particles::update(float dt) {
  // Exponential drag, evaluated per frame rather than per second, so the motion
  // is frame-rate independent.
  const float drag = std::pow(p_.drag, dt);
  int i = 0;
  while (i < live_) {
    life_[i] -= dt;
    if (life_[i] <= 0.0f) {
      // Compact by swapping the last live particle down. No gaps to skip and no
      // allocation; the cost is that draw order is not spawn order, which for
      // additive blending does not matter.
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

void Particles::render(gfx::Surface &s) const {
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

      // The head is full strength and the tail is a third of it, which is what
      // makes a streak read as motion rather than as a smear.
      const float taper = 0.34f + 0.66f * t;
      const uint8_t mul =
          static_cast<uint8_t>(static_cast<float>(life_mul) * taper);
      const uint16_t c = fade(base, mul);

      const int yl = cy - half, yh = cy + half;
      const int xl = cx - half, xh = cx + half;
      for (int yy = yl; yy <= yh; ++yy) {
        if (!s.containsRow(yy)) continue;
        uint16_t *r = s.row(yy);
        const int xa = iclamp(xl, 0, gfx::W - 1);
        const int xb = iclamp(xh, 0, gfx::W - 1);
        for (int xx = xa; xx <= xb; ++xx) r[xx] = addSat(r[xx], c);
      }
    }
  }
}

}  // namespace fx
