#include "CircleMask.h"

namespace gfx {

void maskToCircle(Framebuffer &fb) {
  uint16_t *px = fb.pixels();
  for (int y = 0; y < H; ++y) {
    const int dy = y - CY;
    // Half-chord at this row: the largest dx with dx^2 + dy^2 <= RADIUS^2.
    const int rem = RADIUS_SQ - dy * dy;
    uint16_t *row = px + static_cast<size_t>(y) * W;
    if (rem <= 0) {
      for (int x = 0; x < W; ++x) row[x] = 0;
      continue;
    }
    int half = 0;
    while ((half + 1) * (half + 1) <= rem) ++half;
    const int x0 = CX - half;
    const int x1 = CX + half;
    for (int x = 0; x < x0 && x < W; ++x) row[x] = 0;
    for (int x = x1 + 1; x < W; ++x) {
      if (x >= 0) row[x] = 0;
    }
  }
}

}  // namespace gfx
