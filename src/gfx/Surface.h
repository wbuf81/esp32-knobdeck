#pragma once

// A rectangular strip of pixels being composited into.
//
// This is the seam that lets one set of effects run on both targets. On the
// device a Surface is a 40-row band in internal SRAM which is DMA'd straight to
// the panel; on the desktop it is the whole 360x360 frame. Effects are written
// against Surface, always in SCREEN coordinates, and clip themselves - so the
// same source composites a band and a full frame with no conditionals.
//
// Screen coordinates rather than surface-local ones is the important part. An
// effect that had to know its band's offset would need a different code path
// per target, which is exactly what this exists to avoid.

#include <cstddef>
#include <cstdint>

#include "Geometry.h"

namespace gfx {

struct Surface {
  uint16_t *px = nullptr;  // first pixel of row y0
  int w = 0;
  int h = 0;
  int y0 = 0;  // this surface's top row, in screen space

  int yEnd() const { return y0 + h; }

  // Row in screen coordinates. Caller must have checked containsRow.
  uint16_t *row(int screen_y) {
    return px + static_cast<size_t>(screen_y - y0) * w;
  }
  const uint16_t *row(int screen_y) const {
    return px + static_cast<size_t>(screen_y - y0) * w;
  }

  bool containsRow(int screen_y) const {
    return screen_y >= y0 && screen_y < y0 + h;
  }

  // True when [top, bottom) does not intersect this surface at all, so a caller
  // can reject an off-band sprite with one test instead of per-pixel clipping.
  bool rejectsRows(int top, int bottom) const {
    return bottom <= y0 || top >= y0 + h;
  }

  size_t pixelCount() const { return static_cast<size_t>(w) * h; }
};

}  // namespace gfx
