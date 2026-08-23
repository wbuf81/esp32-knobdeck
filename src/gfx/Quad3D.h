#pragma once

// A texture-mapped quad with a real perspective divide.
//
// This is the "3D" in the view, and a quad is genuinely enough: an album cover
// is a flat object, and a full triangle pipeline would buy nothing a quad cannot
// express while costing clipping, sorting and a depth buffer this board has no
// budget for.
//
// Rasterised as two triangles interpolating u/w, v/w and 1/w linearly in screen
// space, with one divide per pixel. That divide is what perspective-correct
// means; the affine shortcut is visibly wrong the moment the quad tilts, because
// the texture appears to bend along each triangle's shared edge.
//
// Clips to the Surface it is given, so on the device a quad spanning several
// bands is drawn in pieces with no seam and no per-band state.

#include <cstdint>

#include "../art/Image.h"
#include "Surface.h"

namespace gfx {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 1.0f;
};

// Which part of a texture a quad shows. Corner 0 takes (u0, v0), 1 takes
// (u1, v0), 2 takes (u1, v1), 3 takes (u0, v1) - so v1 < v0 is a vertical flip,
// which is how the reflection is drawn without a second code path, and a narrow
// v range crops it.
//
// At namespace scope rather than nested in Quad3D: C++14 will not let a nested
// class's default member initializers be used in a default argument of the
// enclosing class.
struct QuadUv {
  float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
};

class Quad3D {
 public:
  // Focal length in pixels. 340 on a 360-wide screen is a mildly wide lens:
  // enough perspective for a tilt to read as depth, not enough to look
  // fish-eyed when the cover fills the disc.
  static constexpr float FOCAL = 340.0f;
  // Near plane. Clipping a quad properly against it is real work and this view
  // never needs it - the cover orbits, it does not pass through the camera - so
  // a quad with any corner nearer than this is rejected outright rather than
  // divided by something close to zero and sprayed across the screen.
  static constexpr float NEAR_Z = 0.25f;

  // corners: top-left, top-right, bottom-right, bottom-left, in view space.
  // Camera sits at the origin looking down +z.
  //
  // alpha:    0..256 blend against what is already there; 256 is opaque.
  // tint_mul: 0..255 scale on the sampled texel, for the dimmed reflection.
  //
  // Returns false if the quad was rejected or lies entirely outside `s`.
  // Pixels between exact perspective divides. The divide is the expensive part
  // of a textured quad on this core, and u,v are very nearly linear over a short
  // span of a nearly-flat quad, so eight pixels of linear interpolation between
  // exact endpoints is visually identical and costs an eighth as many divides.
  static constexpr int PERSP_STEP = 8;

  // bilinear: false samples nearest, which is right for the dim reflection
  // slices - a faded, half-transparent copy cannot show the shimmer that makes
  // nearest sampling unacceptable on the face itself.
  bool draw(Surface &s, const art::Image &tex, const Vec3 corners[4],
            uint16_t alpha, uint8_t tint_mul, const QuadUv &uv = QuadUv(),
            bool bilinear = true);

 private:
  struct Vert {
    float sx, sy;   // screen position
    float invw;     // 1/z
    float uw, vw;   // u/z, v/z
  };

  void triangle(Surface &s, const art::Image &tex, const Vert &a, const Vert &b,
                const Vert &c, uint16_t alpha, uint8_t tint_mul, bool bilinear);
};

}  // namespace gfx
