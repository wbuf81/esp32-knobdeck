#include "Glyphs.h"

#include "gfx/Blend.h"
#include "gfx/Geometry.h"

namespace shell {
namespace {

// The heart, as the standard implicit curve:
//   (x^2 + y^2 - 1)^3 - x^2 * y^3 <= 0
//
// Chosen over a bezier or a pair of circles because it is one expression with
// no seams, and seams on a heart at 88 pixels are exactly where a hand-built
// one goes wrong. `k` scales it, which is what makes the outline variant a
// difference of two evaluations rather than a second shape.
bool heartAt(float x, float y, float k) {
  x /= k;
  y /= k;
  const float a = x * x + y * y - 1.0f;
  return a * a * a - x * x * y * y * y <= 0.0f;
}

// u, v are normalised to +-1 over the glyph's box, v POSITIVE UP so the shapes
// read the way they are written rather than upside down.
bool inside(Glyph g, float u, float v) {
  switch (g) {
    case Glyph::Play: {
      // A triangle pointing right: the allowed height falls off linearly from
      // the flat left edge to the point.
      if (u < -0.55f || u > 0.80f) return false;
      const float t = (u + 0.55f) / 1.35f;
      return (v < 0 ? -v : v) <= 0.82f * (1.0f - t);
    }
    case Glyph::Pause: {
      const float au = u < 0 ? -u : u;
      return au >= 0.16f && au <= 0.62f && (v < 0 ? -v : v) <= 0.80f;
    }
    case Glyph::Next:
    case Glyph::Previous: {
      // Two stacked triangles, mirrored for Previous. Drawn as one shape so the
      // pair cannot drift apart at different sizes.
      const float uu = g == Glyph::Previous ? -u : u;
      const float av = v < 0 ? -v : v;
      for (int k = 0; k < 2; ++k) {
        const float x0 = -0.90f + k * 0.72f;
        if (uu < x0 || uu > x0 + 0.72f) continue;
        const float t = (uu - x0) / 0.72f;
        if (av <= 0.76f * (1.0f - t)) return true;
      }
      // The trailing bar, so a skip does not read as a play.
      return uu >= 0.74f && uu <= 0.92f && av <= 0.76f;
    }
    case Glyph::HeartFilled:
      return heartAt(u, v, 0.82f);
    case Glyph::HeartOutline:
      return heartAt(u, v, 0.82f) && !heartAt(u, v, 0.60f);
    case Glyph::HeartSlash: {
      if (heartAt(u, v, 0.82f) && !heartAt(u, v, 0.60f)) return true;
      // A bar through it at 45 degrees. Distance to the line v = u.
      const float d = (v - u) * 0.7071f;
      return (d < 0 ? -d : d) <= 0.10f;
    }
    case Glyph::ChevronUp:
    case Glyph::ChevronDown: {
      const float vv = g == Glyph::ChevronDown ? -v : v;
      // Distance to the two arms of the V, each a half-width band.
      const float au = u < 0 ? -u : u;
      if (au > 0.85f) return false;
      const float arm = vv - (0.42f - au * 0.85f);
      return (arm < 0 ? -arm : arm) <= 0.17f;
    }
  }
  return false;
}

}  // namespace

void drawGlyph(gfx::Surface &s, Glyph g, int cx, int cy, int half,
               uint16_t color, uint16_t alpha) {
  if (half <= 0 || alpha == 0) return;
  if (alpha > 256) alpha = 256;
  const int y0 = cy - half, y1 = cy + half;
  // One rejection for the whole glyph rather than a test per row.
  if (s.rejectsRows(y0, y1 + 1)) return;

  const float inv = 1.0f / static_cast<float>(half);
  for (int y = y0; y <= y1; ++y) {
    if (!s.containsRow(y)) continue;
    // v positive up.
    const float v = static_cast<float>(cy - y) * inv;
    uint16_t *row = s.row(y);
    for (int x = cx - half; x <= cx + half; ++x) {
      if (x < 0 || x >= gfx::W) continue;
      const float u = static_cast<float>(x - cx) * inv;
      if (!inside(g, u, v)) continue;
      row[x] = alpha >= 256 ? color : gfx::lerp565(row[x], color, alpha);
    }
  }
}

}  // namespace shell
