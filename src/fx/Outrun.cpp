#include "Outrun.h"

#include "gfx/Blend.h"
#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace fx {
namespace {

// Perspective constants. K/t is the drop below the horizon for a line at depth
// t, so t=1 lands near the bottom of the disc and the rest crowd toward the
// horizon exactly as they should.
constexpr float K = 210.0f;
// Horizontal spacing per pixel of drop. At the bottom row this gives about 46px
// between verticals, which is wide enough to read as a road and narrow enough
// that several are always on screen.
constexpr float VS = 0.219f;
constexpr int VVK = 9;  // verticals drawn either side of the vanishing point

constexpr int SUN_CY = Outrun::HORIZON - 44;
constexpr int SUN_R = 58;

// The classic palette, deliberately NOT album-derived - the same reasoning as
// Tetris's block colours. Magenta grid over an orange-to-magenta sun IS the
// genre; an album-tinted version of it is just a grid. The album still drives
// the shell's ring, so the artwork is not shut out.
inline uint16_t skyAt(int y) {
  // Deep indigo at the top, warming toward the horizon.
  const int d = Outrun::HORIZON - y;  // 0 at the horizon, grows upward
  const int t = d > 150 ? 150 : (d < 0 ? 0 : d);
  const int r = 46 - (t * 40) / 150;
  const int g = 8 + (t * 2) / 150;
  const int b = 60 + (t * 8) / 150;
  return gfx::rgb565(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                     static_cast<uint8_t>(b));
}

inline uint16_t groundAt(int y) {
  // Near-black, lifting very slightly toward the viewer so the grid has
  // something to sit on without competing with it.
  const int d = y - Outrun::HORIZON;
  const int t = d > 210 ? 210 : (d < 0 ? 0 : d);
  const int v = 6 + (t * 14) / 210;
  return gfx::rgb565(static_cast<uint8_t>(v / 2), 0,
                     static_cast<uint8_t>(v));
}

}  // namespace

void Outrun::begin() {
  phase_ = 0.0f;
  speed_ = 1.0f;
}

void Outrun::update(const audio::Modulation &m, float dt) {
  // Faster with louder music, but never stopped: a road that halts reads as a
  // frozen frame rather than as a quiet passage.
  speed_ = 0.55f + 1.5f * m.loudness;
  phase_ += speed_ * dt;
  // Kept in 0..1 by subtraction rather than fmod, so the value never grows large
  // enough to lose precision over a long track.
  while (phase_ >= 1.0f) phase_ -= 1.0f;
}

void Outrun::drawBand(gfx::Surface &s, uint16_t tint) const {
  (void)tint;
  const uint16_t grid = gfx::rgb565(255, 40, 200);   // magenta
  const uint16_t glow = gfx::rgb565(120, 24, 110);   // the horizon haze
  const uint16_t sun_hi = gfx::rgb565(255, 196, 64);
  const uint16_t sun_lo = gfx::rgb565(255, 40, 130);

  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);

    if (y < HORIZON) {
      // --- sky ---
      const uint16_t sky = skyAt(y);
      for (int x = 0; x < gfx::W; ++x) row[x] = sky;

      // The sun, as a disc cut by widening horizontal gaps toward its base.
      const int dy = y - SUN_CY;
      if (dy > -SUN_R && dy < SUN_R) {
        // Integer half-width of the circle at this row.
        int half = 0;
        {
          const int rr = SUN_R * SUN_R - dy * dy;
          // A short integer sqrt: the radius is 58, so this converges in a
          // handful of steps and costs nothing next to the row it fills.
          int lo = 0, hi = SUN_R;
          while (lo < hi) {
            const int mid = (lo + hi + 1) / 2;
            if (mid * mid <= rr) lo = mid; else hi = mid - 1;
          }
          half = lo;
        }
        // Gaps only below the sun's middle, widening downward - the band that
        // makes it a sunset rather than a circle.
        bool gap = false;
        if (dy > -6) {
          const int band = (dy + 6) / 7;
          gap = ((dy + 6) % 7) < (band < 5 ? band : 5);
        }
        if (!gap) {
          // Vertical ramp from gold at the top to hot pink at the base.
          const int t = ((dy + SUN_R) * 256) / (2 * SUN_R);
          const uint16_t c = gfx::lerp565(sun_hi, sun_lo,
                                          static_cast<uint16_t>(t));
          int x0 = gfx::CX - half, x1 = gfx::CX + half;
          if (x0 < 0) x0 = 0;
          if (x1 > gfx::W) x1 = gfx::W;
          for (int x = x0; x < x1; ++x) row[x] = c;
        }
      }
      // A bright seam right at the horizon, which is what sells the distance.
      if (HORIZON - y <= 2) {
        for (int x = 0; x < gfx::W; ++x) row[x] = glow;
      }
      continue;
    }

    // --- ground ---
    const int dy = y - HORIZON;
    const uint16_t base = groundAt(y);
    for (int x = 0; x < gfx::W; ++x) row[x] = base;
    if (dy == 0) continue;

    // Verticals: spacing grows linearly with the drop below the horizon, which
    // is exactly what a perspective divide produces for a flat plane. One
    // multiply per line, not per pixel.
    const float step = VS * static_cast<float>(dy);
    for (int k = -VVK; k <= VVK; ++k) {
      const int x = gfx::CX + static_cast<int>(step * static_cast<float>(k));
      if (x < 0 || x >= gfx::W) continue;
      row[x] = grid;
      // Widen as they near the viewer, so the road does not thin to nothing.
      if (dy > 90 && x + 1 < gfx::W) row[x + 1] = grid;
    }

    // Horizontals: a line at depth t sits K/t below the horizon. Walking t
    // rather than y is what makes them accelerate toward the viewer.
    for (int n = 1; n <= LINES; ++n) {
      const float t = static_cast<float>(n) - phase_;
      if (t <= 0.0f) continue;
      const int ly = HORIZON + static_cast<int>(K / t);
      if (ly != y) continue;
      for (int x = 0; x < gfx::W; ++x) row[x] = grid;
      break;
    }
  }
}

}  // namespace fx
