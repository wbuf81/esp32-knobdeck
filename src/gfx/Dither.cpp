#include "Dither.h"

#include "Color.h"

namespace gfx {
namespace {

// Bayer 4x4, centred so the mean perturbation is zero and the image neither
// brightens nor darkens overall.
const int8_t kBayer[4][4] = {
    {-8, 0, -6, 2},
    {4, -4, 6, -2},
    {-5, 3, -7, 1},
    {7, -1, 5, -3},
};

inline uint8_t clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

void ditherFrame(Framebuffer &fb) {
  uint16_t *px = fb.pixels();
  for (int y = 0; y < H; ++y) {
    uint16_t *row = px + static_cast<size_t>(y) * W;
    const int8_t *bay = kBayer[y & 3];
    for (int x = 0; x < W; ++x) {
      const uint16_t c = row[x];
      if (c == 0x0000 || c == 0xFFFF) continue;  // nothing to dither
      uint8_t r, g, b;
      unpack565(c, r, g, b);
      const int d = bay[x & 3];
      row[x] = rgb565(clamp8(r + d), clamp8(g + (d >> 1)), clamp8(b + d));
    }
  }
}

}  // namespace gfx
