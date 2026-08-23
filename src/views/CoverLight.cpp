#include "CoverLight.h"

#include <cmath>

#if defined(DEVICE)
#include <esp_timer.h>
#define NOW_US() ((uint64_t)esp_timer_get_time())
#else
#define NOW_US() ((uint64_t)0)
#endif

#include "core/Hash.h"
#include "gfx/Blend.h"
#include "gfx/Color.h"
#include "gfx/Hsv.h"

namespace views {
namespace {

constexpr float TAU = 6.28318530718f;

inline float unitOf(uint32_t v) {
  return static_cast<float>(v >> 8) * (1.0f / 16777216.0f);
}

inline uint8_t clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Bayer 4x4 applied to the radial table index, so the values are +-1 steps of
// index rather than +-8 steps of colour. Centred on zero so the frame neither
// brightens nor darkens overall.
const int8_t kBayer4[4][4] = {
    {-1, 0, -1, 1},
    {1, -1, 1, 0},
    {-1, 1, -1, 0},
    {1, 0, 1, -1},
};

}  // namespace

void CoverLight::buildPalette(uint32_t seed) {
  hue_ = unitOf(seed * 2654435761u);
  // A tight analogous spread rather than the whole wheel. A palette drawn from
  // every hue reads as confetti; holding within about 50 degrees reads as light
  // of one colour, which is the point of the view.
  for (int i = 0; i < 16; ++i) {
    const float t = static_cast<float>(i) / 15.0f;
    const float h = hue_ + (t - 0.5f) * 0.14f;
    const float s = 0.62f + 0.34f * (1.0f - t);
    const float v = 0.45f + 0.55f * t;
    palette_[i] = gfx::hsv565(h, s, v);
  }
}

void CoverLight::begin(uint32_t track_seed) {
  buildPalette(track_seed);
  for (int x = 0; x < gfx::W; ++x) {
    const int dx = x - gfx::CX;
    dx2_[x] = static_cast<uint16_t>(dx * dx);
  }
  for (int y = 0; y < gfx::H; ++y) {
    const int dy = y - gfx::CY;
    const int rem = gfx::RADIUS_SQ - dy * dy;
    int h = 0;
    if (rem > 0) while ((h + 1) * (h + 1) <= rem) ++h;
    half_[y] = static_cast<int16_t>(h);
  }
  const int max_idx = (gfx::RADIUS_SQ >> GRAD_SHIFT);
  for (int i = 0; i < GRAD_N; ++i) {
    r01_[i] = i <= max_idx
                  ? std::sqrt(static_cast<float>(i) / static_cast<float>(max_idx))
                  : 1.0f;
  }
  bloom_.begin();
  bloom_.setThreshold(40);
  bloom_.setStrength(150);

  fx::SpawnParams p;
  p.x = static_cast<float>(gfx::CX);
  p.y = static_cast<float>(gfx::CY);
  p.spread = 46.0f;
  p.speed_min = 8.0f;
  p.speed_max = 34.0f;
  p.life_min = 1.4f;
  p.life_max = 4.0f;
  p.size_min = 1.0f;
  p.size_max = 2.4f;
  p.drag = 0.72f;
  p.gravity_y = 0.0f;
  for (int i = 0; i < 16; ++i) p.colors[i] = palette_[i];
  p.color_count = 16;
  parts_.configure(p);
  parts_.clear();

  for (int i = 0; i < MAX_RINGS; ++i) rings_[i] = Ring();
  clock_ = 0.0f;
  emit_acc_ = 0.0f;
}

void CoverLight::buildGradient(const audio::Modulation &m) {
  // Centre brightness tracks loudness; the falloff tightens as the mids rise,
  // so the disc breathes rather than merely changing brightness.
  const float centre_v = 0.10f + 0.55f * m.loudness;
  const float falloff = 1.5f + 2.4f * m.mid;
  const float core = 0.30f + 0.30f * m.bass;  // fraction of radius held bright
  const float inv_core = 1.0f / (1.0f - core);

  const int max_idx = (gfx::RADIUS_SQ >> GRAD_SHIFT);

  // Which falloff powers to blend, so the curve is shapeable without pow().
  // std::pow costs one to two microseconds here, and a thousand of them per
  // frame is a measurable slice of a 33 ms budget for something the eye reads
  // as "the edge is softer".
  const bool low = falloff <= 2.0f;
  const float mix = low ? (falloff - 1.0f) : (falloff - 2.0f) * 0.5f;

  // The guard entries mirror index 0 so a dithered lookup at the very centre
  // reads the centre colour rather than black.
  for (int i = 0; i < GRAD_N; ++i) {
    if (i > max_idx) {
      // Outside the visible disc. Never read, because the backdrop loop is
      // bounded by the half-chord table - but zeroed so a bounds slip shows up
      // as a black wedge rather than as garbage.
      grad_[GRAD_PAD + i] = 0x0000;
      continue;
    }
    const float r01 = r01_[i];
    float v;
    if (r01 < core) {
      v = centre_v;
    } else {
      const float u = 1.0f - (r01 - core) * inv_core;
      const float u2 = u * u;
      const float f = low ? (u + (u2 - u) * mix)
                          : (u2 + (u2 * u2 - u2) * mix);
      v = centre_v * f;
    }

    // Bake every live shockwave ring into the same table. A ring is a bump in a
    // radial function, so any number of them cost nothing per pixel - which is
    // why this view leans on them.
    float ring_add = 0.0f;
    const float rp = r01 * static_cast<float>(gfx::RADIUS);
    for (int k = 0; k < MAX_RINGS; ++k) {
      if (rings_[k].life <= 0.0f) continue;
      const float d = rp - rings_[k].r;
      const float w = 7.0f + 16.0f * (1.0f - rings_[k].life);
      if (d > -w && d < w) {
        const float f = 1.0f - (d < 0 ? -d : d) / w;
        ring_add += f * f * rings_[k].life * 0.55f;
      }
    }

    float vv = v + ring_add;
    if (vv > 1.0f) vv = 1.0f;
    // Saturation eases off as things brighten, so a peak reads as light rather
    // than as a more intense colour.
    grad_[GRAD_PAD + i] =
        gfx::hsv565(hue_ + 0.02f * r01, 0.85f - 0.45f * vv, vv);
  }
  for (int k = 0; k < GRAD_PAD; ++k) grad_[k] = grad_[GRAD_PAD];
}

void CoverLight::update(const audio::Modulation &m, float dt, core::Rng &rng) {
  clock_ += dt;

  // Advance rings.
  for (int k = 0; k < MAX_RINGS; ++k) {
    if (rings_[k].life <= 0.0f) continue;
    rings_[k].r += rings_[k].speed * dt;
    rings_[k].life -= dt * 1.15f;
    if (rings_[k].r > gfx::RADIUS + 30.0f) rings_[k].life = 0.0f;
  }

  if (m.onset) {
    // A shockwave and a particle burst on the beat.
    for (int k = 0; k < MAX_RINGS; ++k) {
      if (rings_[k].life > 0.0f) continue;
      rings_[k].r = 12.0f;
      rings_[k].speed = 190.0f + 130.0f * m.bass;
      rings_[k].life = 1.0f;
      break;
    }
    parts_.burst(60 + static_cast<int>(70.0f * m.bass), 0.8f + m.bass, rng);
  }

  // A steady drizzle so the field never empties between beats. Accumulated as a
  // float so the rate is frame-rate independent rather than per-frame.
  emit_acc_ += (26.0f + 44.0f * m.loudness) * dt;
  const int n = static_cast<int>(emit_acc_);
  if (n > 0) {
    emit_acc_ -= static_cast<float>(n);
    parts_.emit(n, rng);
  }

  parts_.update(dt);
  buildGradient(m);

  bloom_.setStrength(static_cast<uint8_t>(96 + 140.0f * m.loudness));
}

void CoverLight::renderBand(gfx::Surface &s) {
  uint64_t t0 = NOW_US();

  // Pass 1: backdrop, glow and dither, in one WRITE-ONLY sweep.
  //
  // Fused deliberately. Measured on this board, a separate read-modify-write
  // pass over every pixel to add the glow cost 84 ms of a 115 ms frame. This
  // loop touches each pixel exactly once, never reads it back, and only covers
  // the visible disc.
  for (int y = s.y0; y < s.yEnd(); ++y) {
    const int dy = y - gfx::CY;
    const int dy2 = dy * dy;
    const int h = half_[y];
    uint16_t *row = s.row(y);

    const int x0 = gfx::CX - h;
    const int x1 = gfx::CX + h;
    // Outside the disc: never visible, but cleared so nothing stale shows if
    // the geometry is ever wrong.
    for (int x = 0; x < x0; ++x) row[x] = 0;
    for (int x = x1 + 1; x < gfx::W; ++x) row[x] = 0;
    if (h <= 0) continue;

    const uint16_t *gl = bloom_.glowRow(y);
    const int8_t *bay = kBayer4[y & 3];

    // Two pixels per iteration, written as one 32-bit store.
    //
    // The radial index is computed once for the pair. Measured, internal SRAM
    // does 181 MB/s on 32-bit stores against 91 on 16-bit, so the store alone is
    // twice as fast - and the gradient and glow both vary far too slowly for a
    // two-pixel horizontal step to be visible, especially under dither. The band
    // buffer is malloc'd and the row stride is 360, so an even x is always
    // 4-byte aligned.
    int x = x0;
    if (x & 1) {  // odd start: one single pixel to reach alignment
      const int idx = ((dx2_[x] + dy2) >> GRAD_SHIFT) + bay[x & 3];
      row[x] = gfx::addSat(grad_[GRAD_PAD + idx], gl[x]);
      ++x;
    }
    // One addSat per PAIR, not per pixel.
    //
    // buildGlowRow writes four identical values per small pixel, so for any even
    // x, gl[x] and gl[x+1] are always the same entry - both halves of a pair
    // always land inside one four-pixel glow block. That makes the second
    // saturating add provably redundant rather than approximately redundant.
    uint32_t *out = reinterpret_cast<uint32_t *>(&row[x]);
    for (; x + 1 <= x1; x += 2, ++out) {
      const int idx = ((dx2_[x] + dy2) >> GRAD_SHIFT) + bay[x & 3];
      const uint16_t v = gfx::addSat(grad_[GRAD_PAD + idx], gl[x]);
      *out = (static_cast<uint32_t>(v) << 16) | v;
    }
    for (; x <= x1; ++x) {  // odd tail
      const int idx = ((dx2_[x] + dy2) >> GRAD_SHIFT) + bay[x & 3];
      row[x] = gfx::addSat(grad_[GRAD_PAD + idx], gl[x]);
    }
  }
  const uint64_t t1 = NOW_US();

  // Pass 2: particles, additive, sparse.
  parts_.render(s);
  const uint64_t t2 = NOW_US();

  // Pass 3: bright-pass contribution for next frame's glow. Read-only and
  // subsampled to a quarter of the pixels.
  bloom_.accumulateBand(s);
  const uint64_t t3 = NOW_US();

  t_.backdrop += t1 - t0;
  t_.particles += t2 - t1;
  t_.bloom += t3 - t2;
  ++t_.frames;
}

}  // namespace views
