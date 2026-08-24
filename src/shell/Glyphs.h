#pragma once

// The transport icons, plotted rather than typeset.
//
// The fonts in this project are generated at 0x20-0xFF, so there is no play
// triangle, no pause bar and no heart to set in text - they would come out as
// the fallback box, which is the worst possible thing on an icon whose entire
// job is to answer "did that register?". Each shape here is an implicit
// function evaluated over its own bounding box: no font data, no flash cost,
// and crisp at any size the caller asks for.
//
// Every pixel in the box is written at most once, by assignment or by a single
// blend. That is what makes a banded draw byte-identical to a full-frame one -
// the invariant that has caught three clipping bugs in this project.

#include <cstdint>

#include "gfx/Surface.h"

namespace shell {

enum class Glyph : uint8_t {
  Play,
  Pause,
  Next,
  Previous,
  HeartFilled,
  HeartOutline,
  HeartSlash,   // liked was asked for while nothing was playing
  ChevronUp,
  ChevronDown,
};

// Centred on (cx, cy), fitting inside +-half in both axes. `alpha` is 0..256,
// blended against what is already there, so a fade costs nothing extra.
void drawGlyph(gfx::Surface &s, Glyph g, int cx, int cy, int half,
               uint16_t color, uint16_t alpha);

}  // namespace shell
