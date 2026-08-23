#pragma once

// Adafruit-GFX bitmap fonts, rendered by this project rather than by a library.
//
// The struct layout is Adafruit's, deliberately, because the generated font
// headers are written against it and initialise as-is. This project does not
// link Adafruit_GFX or LovyanGFX, so the types are declared here.
//
// The fonts themselves are generated at codepoint range 0x20-0xFF, not the
// default 0x20-0x7E. That is load-bearing: the ancestor project's note says the
// ASCII-only range "would reintroduce the tofu-box bug for accented artist
// names", and a device pointed at a real music library will meet Bjork, Beyonce
// and Sigur Ros within the first hour.
//
// The limit is real and worth stating: Latin-1 only. Cyrillic, Greek and CJK
// render as a fallback glyph. The ancestor sidestepped this with LovyanGFX's
// CJK faces, which cost far more flash than this project has spare.

#include <cstdint>

#include "Surface.h"

namespace gfx {

struct GFXglyph {
  uint16_t bitmapOffset;
  uint8_t width;
  uint8_t height;
  uint8_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
};

struct GFXfont {
  uint8_t *bitmap;
  GFXglyph *glyph;
  uint16_t first;
  uint16_t last;
  uint8_t yAdvance;
};

// Rendered width in pixels. Input is UTF-8; codepoints above 0xFF count as the
// fallback glyph, so the measurement matches what will actually be drawn.
int textWidth(const GFXfont &f, const char *utf8);

// Line height.
inline int textHeight(const GFXfont &f) { return f.yAdvance; }

// Draws with the string's left edge at `x` and its BASELINE at `baseline`,
// clipped to the surface. Baseline rather than top because that is what the
// glyph metrics are relative to, and mixing two fonts on one line only lines up
// if they share a baseline.
void drawText(Surface &s, const GFXfont &f, int x, int baseline,
              const char *utf8, uint16_t color);

// Horizontally centred on `cx`.
void drawTextCentered(Surface &s, const GFXfont &f, int cx, int baseline,
                      const char *utf8, uint16_t color);

// Centred, truncated with a trailing ellipsis if it will not fit `max_w`.
// Returns the width actually drawn.
int drawTextFit(Surface &s, const GFXfont &f, int cx, int baseline,
                const char *utf8, int max_w, uint16_t color);

// Half the chord of the visible disc at row `y`, inset by `margin`. This is how
// much room text has on a round screen, and it is much less than the pixel width
// suggests anywhere near the top or bottom.
int halfChordAt(int y, int margin);

}  // namespace gfx
