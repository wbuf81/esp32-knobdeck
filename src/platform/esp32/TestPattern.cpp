#include "TestPattern.h"

#include <cstddef>

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace esp32 {

void drawTestPattern(uint16_t *fb) {
  using namespace gfx;

  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const int dx = x - CX;
      const int dy = y - CY;
      const int d2 = dx * dx + dy * dy;
      uint16_t c;

      if (d2 > RADIUS_SQ) {
        c = 0x0000;  // outside the disc: never visible, so prove we know that
      } else if (d2 >= (RADIUS - 1) * (RADIUS - 1)) {
        c = 0xFFFF;  // the outermost visible ring
      } else if (dy > -12 && dy < 12 && dx > -80 && dx < 80) {
        // Centre grey ramp: 16 steps, so banding and bit depth are both visible.
        const int step = (dx + 80) * 16 / 160;
        const uint8_t v = static_cast<uint8_t>(step * 17);
        c = rgb565(v, v, v);
      } else if (dy < -60 && dy > -120 && (dx > -(dy + 120) / 2) &&
                 (dx < (dy + 120) / 2)) {
        c = rgb565(255, 200, 0);  // "this way up" wedge
      } else if (dx < 0 && dy < 0) {
        c = rgb565(255, 0, 0);    // top-left    RED
      } else if (dx >= 0 && dy < 0) {
        c = rgb565(0, 255, 0);    // top-right   GREEN
      } else if (dx < 0) {
        c = rgb565(0, 0, 255);    // bottom-left BLUE
      } else {
        c = rgb565(255, 255, 255);// bottom-right WHITE
      }
      fb[static_cast<size_t>(y) * W + x] = c;
    }
  }
}

}  // namespace esp32
