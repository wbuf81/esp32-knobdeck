#pragma once

// Bring-up test pattern.
//
// Chosen so that orientation, colour order and the visible disc boundary are
// all readable at a glance, because "the screen lit up" is not the same as "the
// screen is correct" and the difference is expensive to discover later:
//
//   quadrants   red / green / blue / white, clockwise from top-left.
//               If red is where blue should be, the BGR bit is wrong.
//               If they are rotated, MADCTL is wrong.
//   arrow       a wedge pointing at the top edge, so "up" is unambiguous.
//   ring        one pixel at r=179, the outermost visible circle. If any of it
//               is cut off, the panel offset is wrong.
//   grey ramp   a horizontal 16-step wedge through the centre, to show banding
//               and confirm the panel really is taking 16-bit pixels.

#include <cstdint>

namespace esp32 {

void drawTestPattern(uint16_t *fb);

}  // namespace esp32
