#include "RadialShell.h"

#include <cmath>

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace shell {
namespace {

constexpr float TAU = 6.28318530718f;
constexpr float PI_2 = 1.57079632679f;
constexpr float PI_F = 3.14159265359f;

// Unit-circle table, indexed by turn.
//
// 2048 entries is set by the largest ring: at r=174 a full turn is about 1093
// pixels of arc, so this gives roughly half-pixel steps and the stroke is
// continuous. Trig was measured, not guessed - sinf and cosf per step across
// eighteen bands is about 150,000 calls a frame, and it cost half the frame
// rate.
constexpr int LUT_N = 2048;
int16_t g_sin[LUT_N];
bool g_lut_ready = false;

void buildLut() {
  if (g_lut_ready) return;
  for (int i = 0; i < LUT_N; ++i) {
    const float a = TAU * static_cast<float>(i) / static_cast<float>(LUT_N);
    g_sin[i] = static_cast<int16_t>(std::sin(a) * 16384.0f);
  }
  g_lut_ready = true;
}

inline float lutSin(int i) {
  return static_cast<float>(g_sin[i & (LUT_N - 1)]) * (1.0f / 16384.0f);
}
inline float lutCos(int i) { return lutSin(i + LUT_N / 4); }

// Plots a range of LUT indices. Index space, not turns, deliberately.
//
// The walked index range has to depend only on the ARC, never on how it was
// subdivided. Rounding each sub-interval's endpoints outward in turn-space
// meant band mode had thirty-six interval ends to full-frame's two, each
// reaching a step further - so band mode drew a few pixels past the end of the
// progress ring that full-frame did not. Clamping every walk to the arc's own
// global index range makes the union over bands exactly the whole arc.
void walk(gfx::Surface &s, int r, int half, int i0, int i1, uint16_t color) {
  if (i1 < i0) return;
  for (int i = i0; i <= i1; ++i) {
    // The LUT's zero is at three o'clock; the arc's is at twelve, hence the
    // quarter-turn offset.
    const int k = i - LUT_N / 4;
    const float ca = lutCos(k);
    const float sa = lutSin(k);
    for (int d = -half; d <= half; ++d) {
      const float rr = static_cast<float>(r + d);
      const int y = static_cast<int>(static_cast<float>(gfx::CY) + sa * rr);
      if (!s.containsRow(y)) continue;
      const int x = static_cast<int>(static_cast<float>(gfx::CX) + ca * rr);
      if (x < 0 || x >= gfx::W) continue;
      // Assignment, not additive.
      //
      // Two reasons, and the second is the load-bearing one. A gauge should be a
      // definite colour rather than brighter where the walk's steps happen to
      // bunch up. And additive drawing here is not idempotent: neighbouring
      // steps land on the same pixel, so the result depends on how the walk was
      // subdivided - which made the banded and full-frame paths disagree the
      // moment the walk was split per band.
      s.row(y)[x] = color;
    }
  }
}

}  // namespace

void drawArc(gfx::Surface &s, int r, int thickness, float a0, float a1,
             uint16_t color) {
  if (a1 <= a0 || r <= 0) return;
  buildLut();
  const int half = thickness / 2;

  // Only walk the parts of the arc that can possibly land in this surface.
  //
  // A ring at r=170 spans almost every row of the disc, so per-band rejection on
  // the bounding box buys nothing - but for a given 20-row band the ring only
  // crosses it in two short angular stretches, one on each side. Solving for
  // those turns eighteen full-circle walks per frame into the equivalent of
  // about two.
  // The range has to cover EVERY radius layer, not just the outer one.
  //
  // A plot lands at y = CY + sin(angle) * rr, so for a fixed angle the inner and
  // outer layers of a thick stroke land on different rows. Solving the range
  // from the outer radius alone skipped angles where only the inner layer fell
  // inside the band, which showed up as eight missing pixels on the ring's inner
  // edge at band boundaries. Taking the widest bound over both radii is exact
  // enough and costs two divisions.
  const float rr_out = static_cast<float>(r + half);
  const float rr_in = static_cast<float>(r - half > 1 ? r - half : 1);
  const float ylo = static_cast<float>(s.y0 - 1) - static_cast<float>(gfx::CY);
  const float yhi = static_cast<float>(s.yEnd()) - static_cast<float>(gfx::CY);
  const float lo_a = ylo / rr_out, lo_b = ylo / rr_in;
  const float hi_a = yhi / rr_out, hi_b = yhi / rr_in;
  float s0 = lo_a < lo_b ? lo_a : lo_b;
  float s1 = hi_a > hi_b ? hi_a : hi_b;
  if (s1 < -1.0f || s0 > 1.0f) return;
  if (s0 < -1.0f) s0 = -1.0f;
  if (s1 > 1.0f) s1 = 1.0f;

  // sin(angle) must fall between s0 and s1. That has two solution branches: the
  // right half of the circle from asin, and the left half from pi - asin.
  const float A0 = std::asin(s0);
  const float A1 = std::asin(s1);

  // The arc's own index range: computed from a0 and a1 alone, so it is the same
  // whoever asks and however the surface is split.
  const int g0 = static_cast<int>(std::ceil(a0 * LUT_N));
  const int g1 = static_cast<int>(std::floor(a1 * LUT_N));
  if (g1 < g0) return;

  // sin(angle) between s0 and s1 has two solution branches: the right half of
  // the circle from asin, and the left half from pi - asin. Each is widened
  // outward by a step so adjacent bands overlap and cannot leave a gap; the
  // clamp to [g0, g1] is what keeps that widening from overrunning the arc.
  const float branches[2][2] = {
      {(A0 + PI_2) / TAU, (A1 + PI_2) / TAU},
      {(PI_F - A1 + PI_2) / TAU, (PI_F - A0 + PI_2) / TAU},
  };
  for (int b = 0; b < 2; ++b) {
    int lo = static_cast<int>(std::floor(branches[b][0] * LUT_N)) - 1;
    int hi = static_cast<int>(std::ceil(branches[b][1] * LUT_N)) + 1;
    if (lo < g0) lo = g0;
    if (hi > g1) hi = g1;
    walk(s, r, half, lo, hi, color);
  }
}

void RadialShell::showVolume(int pct, uint32_t now_ms) {
  volume_pct_ = pct;
  volume_for_.arm(now_ms, VOLUME_SHOW_MS);
}

bool RadialShell::volumeVisible(uint32_t now_ms) const {
  return volume_for_.pending(now_ms);
}

void RadialShell::render(gfx::Surface &s, float progress01, uint16_t tint,
                         int volume_pct, uint32_t now_ms, float beat) {
  if (progress01 < 0.0f) progress01 = 0.0f;
  if (progress01 > 1.0f) progress01 = 1.0f;

  // The track the progress ring runs in, so the ring reads as a gauge with a
  // full extent rather than as a line of unknown length.
  drawArc(s, PROGRESS_R, PROGRESS_THICK, 0.0f, 1.0f, gfx::rgb565(14, 14, 20));

  if (progress01 > 0.0005f) {
    drawArc(s, PROGRESS_R, PROGRESS_THICK, 0.0f, progress01, tint);
    // A comet head at the leading edge - inherited from the ancestor's progress
    // bar, and what makes a very slow ring feel like it is moving at all.
    const float w = 0.006f;
    const float a0 = progress01 - w < 0.0f ? 0.0f : progress01 - w;
    drawArc(s, PROGRESS_R, PROGRESS_THICK + 2, a0, progress01,
            gfx::rgb565(255, 255, 255));
  }

  if (volume_for_.pending(now_ms)) {
    // Volume is unknown when the active device does not report one. Drawing it
    // as zero would be a confident lie, so it renders as a dim full ring: plainly
    // present, plainly not a reading.
    if (volume_pct_ < 0) {
      drawArc(s, PROGRESS_R, VOLUME_THICK, 0.0f, 1.0f, gfx::rgb565(40, 30, 12));
    } else {
      const float v = static_cast<float>(volume_pct_) * 0.01f;
      const uint8_t pulse = static_cast<uint8_t>(150.0f + 60.0f * beat);
      drawArc(s, PROGRESS_R, VOLUME_THICK, 0.0f, 1.0f, gfx::rgb565(16, 16, 22));
      drawArc(s, PROGRESS_R, VOLUME_THICK, 0.0f, v,
              gfx::rgb565(pulse, pulse, 90));
    }
  }
  (void)volume_pct;
}

}  // namespace shell
