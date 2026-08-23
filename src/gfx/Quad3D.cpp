#include "Quad3D.h"

#include "Blend.h"
#include "Color.h"

namespace gfx {
namespace {

inline int ifloor(float v) {
  const int i = static_cast<int>(v);
  return v < static_cast<float>(i) ? i - 1 : i;
}

inline int iclamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

void Quad3D::triangle(Surface &s, const art::Image &tex, const Vert &a,
                      const Vert &b, const Vert &c, uint16_t alpha,
                      uint8_t tint_mul, bool bilinear) {
  // Signed area via the cross product. Zero is degenerate; the sign is folded
  // into the edge functions below rather than handled by reordering vertices,
  // because the reflection quads are mirrored and would otherwise each need
  // their own winding case.
  const float area = (b.sx - a.sx) * (c.sy - a.sy) - (c.sx - a.sx) * (b.sy - a.sy);
  if (area > -0.0001f && area < 0.0001f) return;
  const float sgn = area > 0.0f ? 1.0f : -1.0f;
  const float inv_area = 1.0f / (area * sgn);  // 1 / |area|

  float minyf = a.sy, maxyf = a.sy;
  if (b.sy < minyf) minyf = b.sy;
  if (b.sy > maxyf) maxyf = b.sy;
  if (c.sy < minyf) minyf = c.sy;
  if (c.sy > maxyf) maxyf = c.sy;

  const int y0 = iclamp(ifloor(minyf), s.y0, s.yEnd() - 1);
  const int y1 = iclamp(ifloor(maxyf) + 1, s.y0, s.yEnd() - 1);
  if (y1 < y0) return;

  // Edge functions, pre-multiplied by the area's sign so "inside" is simply all
  // three being non-negative.
  const float dE0 = -(c.sy - b.sy) * sgn;
  const float dE1 = -(a.sy - c.sy) * sgn;
  const float dE2 = -(b.sy - a.sy) * sgn;

  const int tw = tex.width(), th = tex.height();
  const uint16_t *tpx = tex.pixels();
  const float fw = static_cast<float>(tw);
  const float fh = static_cast<float>(th);

  for (int y = y0; y <= y1; ++y) {
    const float py = static_cast<float>(y) + 0.5f;
    const float px0 = 0.5f;

    float E0 = ((c.sx - b.sx) * (py - b.sy) - (c.sy - b.sy) * (px0 - b.sx)) * sgn;
    float E1 = ((a.sx - c.sx) * (py - c.sy) - (a.sy - c.sy) * (px0 - c.sx)) * sgn;
    float E2 = ((b.sx - a.sx) * (py - a.sy) - (b.sy - a.sy) * (px0 - a.sx)) * sgn;

    // Solve for the covered span directly instead of testing every pixel.
    //
    // Each edge function is linear in x, so it changes sign at exactly one x and
    // the inside of a convex triangle is the intersection of three half-lines -
    // one contiguous run. Finding it costs three divides per row and removes
    // three compares from every pixel in the bounding box, including all the
    // pixels outside the triangle that used to be visited and rejected.
    float lo = 0.0f, hi = static_cast<float>(W - 1);
    bool empty = false;
    const float Es[3] = {E0, E1, E2};
    const float dEs[3] = {dE0, dE1, dE2};
    for (int k = 0; k < 3; ++k) {
      if (dEs[k] > 1e-6f) {
        const float x = -Es[k] / dEs[k];
        if (x > lo) lo = x;
      } else if (dEs[k] < -1e-6f) {
        const float x = -Es[k] / dEs[k];
        if (x < hi) hi = x;
      } else if (Es[k] < 0.0f) {
        empty = true;
        break;
      }
    }
    if (empty) continue;
    int xs = iclamp(static_cast<int>(lo > 0.0f ? lo + 0.9999f : 0.0f), 0, W - 1);
    int xe = iclamp(ifloor(hi), 0, W - 1);
    if (xe < xs) continue;

    // The three interpolants are linear in x, so they are stepped rather than
    // recomputed from barycentrics at every pixel.
    const float d_invw = (dE0 * a.invw + dE1 * b.invw + dE2 * c.invw) * inv_area;
    const float d_un = (dE0 * a.uw + dE1 * b.uw + dE2 * c.uw) * inv_area;
    const float d_vn = (dE0 * a.vw + dE1 * b.vw + dE2 * c.vw) * inv_area;

    const float fxs = static_cast<float>(xs);
    float invw = ((E0 + dE0 * fxs) * a.invw + (E1 + dE1 * fxs) * b.invw +
                  (E2 + dE2 * fxs) * c.invw) * inv_area;
    float un = ((E0 + dE0 * fxs) * a.uw + (E1 + dE1 * fxs) * b.uw +
                (E2 + dE2 * fxs) * c.uw) * inv_area;
    float vn = ((E0 + dE0 * fxs) * a.vw + (E1 + dE1 * fxs) * b.vw +
                (E2 + dE2 * fxs) * c.vw) * inv_area;
    if (invw <= 0.0f) continue;

    uint16_t *row = s.row(y);
    float w = 1.0f / invw;
    float u = un * w, v = vn * w;
    int x = xs;

    while (x <= xe) {
      int n = xe - x + 1;
      if (n > PERSP_STEP) n = PERSP_STEP;
      const float fn = static_cast<float>(n);
      const float invw2 = invw + d_invw * fn;
      float u2 = u, v2 = v;
      if (invw2 > 0.0f) {
        const float w2 = 1.0f / invw2;
        u2 = (un + d_un * fn) * w2;
        v2 = (vn + d_vn * fn) * w2;
      }
      const float inv_n = 1.0f / fn;
      const float du = (u2 - u) * inv_n;
      const float dv = (v2 - v) * inv_n;

      for (int k = 0; k < n; ++k, ++x, u += du, v += dv) {
        uint16_t texel;
        if (bilinear) {
          const float fx = u * fw - 0.5f;
          const float fy = v * fh - 0.5f;
          int ix = ifloor(fx), iy = ifloor(fy);
          uint16_t tx = static_cast<uint16_t>((fx - ix) * 256.0f);
          uint16_t ty = static_cast<uint16_t>((fy - iy) * 256.0f);
          // Clamped once, here, so the four fetches need no bounds tests of
          // their own - sixteen compares per pixel, removed.
          //
          // The weight has to move with the clamp. Simply clamping ix to tw-2
          // changes WHICH texel is read when the true index was the last column,
          // rather than just where the sample sits between two - which the UV
          // orientation test caught immediately, and which would have shown on
          // screen as the right edge of every cover being subtly wrong.
          if (ix < 0) {
            ix = 0;
            tx = 0;
          } else if (ix > tw - 2) {
            ix = tw - 2;
            tx = 256;
          }
          if (iy < 0) {
            iy = 0;
            ty = 0;
          } else if (iy > th - 2) {
            iy = th - 2;
            ty = 256;
          }
          const uint16_t *r0 = tpx + static_cast<size_t>(iy) * tw + ix;
          const uint16_t *r1 = r0 + tw;
          texel = lerp565(lerp565(r0[0], r0[1], tx), lerp565(r1[0], r1[1], tx), ty);
        } else {
          const int ix = iclamp(static_cast<int>(u * fw), 0, tw - 1);
          const int iy = iclamp(static_cast<int>(v * fh), 0, th - 1);
          texel = tpx[static_cast<size_t>(iy) * tw + ix];
        }
        if (tint_mul != 255) texel = fade(texel, tint_mul);
        row[x] = alpha >= 256 ? texel : lerp565(row[x], texel, alpha);
      }

      invw = invw2;
      un += d_un * fn;
      vn += d_vn * fn;
      u = u2;
      v = v2;
    }
  }
}

bool Quad3D::draw(Surface &s, const art::Image &tex, const Vec3 corners[4],
                  uint16_t alpha, uint8_t tint_mul, const QuadUv &uv,
                  bool bilinear) {
  if (!tex.valid() || alpha == 0) return false;

  Vert v[4];
  const float kU[4] = {uv.u0, uv.u1, uv.u1, uv.u0};
  const float kV[4] = {uv.v0, uv.v0, uv.v1, uv.v1};

  for (int i = 0; i < 4; ++i) {
    if (corners[i].z < NEAR_Z) return false;  // reject rather than clip
    const float invz = 1.0f / corners[i].z;
    v[i].sx = static_cast<float>(CX) + corners[i].x * FOCAL * invz;
    v[i].sy = static_cast<float>(CY) + corners[i].y * FOCAL * invz;
    v[i].invw = invz;
    v[i].uw = kU[i] * invz;
    v[i].vw = kV[i] * invz;
  }

  // Two triangles sharing the TL-BR diagonal.
  triangle(s, tex, v[0], v[1], v[2], alpha, tint_mul, bilinear);
  triangle(s, tex, v[0], v[2], v[3], alpha, tint_mul, bilinear);
  return true;
}

}  // namespace gfx
