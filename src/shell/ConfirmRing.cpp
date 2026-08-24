#include "ConfirmRing.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "RadialShell.h"  // drawArc
#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"

namespace shell {
namespace {

constexpr float TAU = 6.28318530718f;
constexpr char kHeading[] = "PLAY?";

// Scales a packed colour toward black. The dim end has a floor: an arc that
// fades to literally nothing reads as a rendering fault rather than as the
// unselected option.
uint16_t dim(uint16_t c, float k) {
  if (k > 1.0f) k = 1.0f;
  if (k < 0.18f) k = 0.18f;
  uint8_t r, g, b;
  gfx::unpack565(c, r, g, b);
  return gfx::rgb565(static_cast<uint8_t>(r * k), static_cast<uint8_t>(g * k),
                     static_cast<uint8_t>(b * k));
}

// Truncating fit with a trailing ellipsis, stepping whole UTF-8 sequences so a
// cut never lands mid-codepoint and leaves a stray continuation byte.
void fitInto(char *dst, size_t cap, const gfx::GFXfont &f, const char *src,
             int max_w) {
  dst[0] = '\0';
  if (!src || !src[0] || max_w <= 0) return;
  if (gfx::textWidth(f, src) <= max_w) {
    std::snprintf(dst, cap, "%s", src);
    return;
  }
  const int budget = max_w - gfx::textWidth(f, "...");
  int used = 0, w = 0;
  const char *p = src;
  while (*p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    int n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    char one[8] = {};
    for (int i = 0; i < n && p[i]; ++i) one[i] = p[i];
    const int adv = gfx::textWidth(f, one);
    if (w + adv > budget) break;
    if (used + n + 4 >= static_cast<int>(cap)) break;
    std::memcpy(dst + used, p, static_cast<size_t>(n));
    used += n;
    w += adv;
    p += n;
  }
  dst[used] = '\0';
  std::snprintf(dst + used, cap - used, "...");
}

// Breaks a title across two lines at a SPACE, never mid-word.
//
// Two lines and no more: a third would collide with the arcs, and a track name
// that needs three lines on a 1.8-inch dial is not going to be read at a glance
// anyway - it is better ellipsised than shrunk.
void wrapTwo(const char *src, const gfx::GFXfont &f, int w1, int w2, char *l1,
             size_t cap1, char *l2, size_t cap2) {
  l1[0] = '\0';
  l2[0] = '\0';
  if (!src || !src[0]) return;
  if (gfx::textWidth(f, src) <= w1) {
    std::snprintf(l1, cap1, "%s", src);
    return;
  }

  // Longest prefix ending at a space that still fits the first line.
  int best = -1;
  char probe[96];
  for (int i = 0; src[i]; ++i) {
    if (src[i] != ' ') continue;
    if (i >= static_cast<int>(sizeof(probe))) break;
    std::memcpy(probe, src, static_cast<size_t>(i));
    probe[i] = '\0';
    if (gfx::textWidth(f, probe) > w1) break;
    best = i;
  }

  if (best <= 0) {
    // One unbroken word wider than the disc. Nothing to split on, so it takes
    // the ellipsis rather than spilling.
    fitInto(l1, cap1, f, src, w1);
    return;
  }
  std::memcpy(l1, src, static_cast<size_t>(best));
  l1[best] = '\0';
  fitInto(l2, cap2, f, src + best + 1, w2);
}

// A straight stroke of half-width `half`, plotted rather than scan-converted.
//
// Assignment, not blending, and the whole line is always walked with rows
// outside the surface skipped - so a band draws exactly the pixels the full
// frame does at those rows. Blending here would double-hit where the two
// strokes of a cross meet, and that is precisely the kind of difference the
// banded/full-frame assertion catches.
void stroke(gfx::Surface &s, int x0, int y0, int x1, int y1, int half,
            uint16_t color) {
  const int dx = x1 - x0, dy = y1 - y0;
  int steps = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx)
                                                        : (dy < 0 ? -dy : dy);
  if (steps <= 0) steps = 1;
  for (int i = 0; i <= steps; ++i) {
    const int cx = x0 + dx * i / steps;
    const int cy = y0 + dy * i / steps;
    for (int oy = -half; oy <= half; ++oy) {
      const int y = cy + oy;
      if (!s.containsRow(y)) continue;
      uint16_t *row = s.row(y);
      for (int ox = -half; ox <= half; ++ox) {
        const int x = cx + ox;
        if (x < 0 || x >= gfx::W) continue;
        row[x] = color;
      }
    }
  }
}

// Screen position of an angle in turns, 0 = twelve o'clock, clockwise.
void polar(float turns, int r, int *x, int *y) {
  const float a = turns * TAU;
  *x = gfx::CX + static_cast<int>(std::sin(a) * static_cast<float>(r));
  *y = gfx::CY - static_cast<int>(std::cos(a) * static_cast<float>(r));
}

}  // namespace

void ConfirmRing::prepare(const char *track, float choice) {
  if (choice < 0.0f) choice = 0.0f;
  if (choice > 1.0f) choice = 1.0f;
  choice_ = choice;

  heading_x_ =
      gfx::CX - gfx::textWidth(gfx::fontSmall(), kHeading) / 2;

  const gfx::GFXfont &f = gfx::fontTitle();
  wrapTwo(track, f, gfx::halfChordAt(LINE1_BASELINE, MARGIN) * 2,
          gfx::halfChordAt(LINE2_BASELINE, MARGIN) * 2, line1_, sizeof(line1_),
          line2_, sizeof(line2_));
  line1_x_ = gfx::CX - gfx::textWidth(f, line1_) / 2;
  line2_x_ = gfx::CX - gfx::textWidth(f, line2_) / 2;
}

void ConfirmRing::render(gfx::Surface &s, uint16_t tint) const {
  gfx::drawText(s, gfx::fontSmall(), heading_x_, HEADING_BASELINE, kHeading,
                gfx::rgb565(120, 120, 132));

  if (line1_[0])
    gfx::drawText(s, gfx::fontTitle(), line1_x_, LINE1_BASELINE, line1_,
                  gfx::rgb565(245, 245, 250));
  if (line2_[0])
    gfx::drawText(s, gfx::fontTitle(), line2_x_, LINE2_BASELINE, line2_,
                  gfx::rgb565(245, 245, 250));

  // Both arcs are always drawn; the CHOICE is carried by brightness and by
  // where the marker sits. Hiding the option you are not on would make the
  // screen look like it has one answer.
  const float lean = choice_;
  const uint16_t no_col = dim(gfx::rgb565(235, 70, 62), 1.0f - lean * 0.8f);
  const uint16_t yes_col = dim(tint, 0.2f + lean * 0.8f);
  drawArc(s, ARC_R, ARC_THICK, NO_A0, NO_A1, no_col);
  drawArc(s, ARC_R, ARC_THICK, YES_A0, YES_A1, yes_col);

  // The marker rides between the two arc midpoints, thicker and white, so the
  // eye tracks one bright thing moving rather than two arcs changing shade.
  const float no_mid = (NO_A0 + NO_A1) * 0.5f;
  const float yes_mid = (YES_A0 + YES_A1) * 0.5f;
  const float at = no_mid + (yes_mid - no_mid) * lean;
  drawArc(s, ARC_R, MARKER_THICK, at - MARKER_HALF, at + MARKER_HALF,
          gfx::rgb565(250, 250, 255));

  // The glyphs, each sitting inside its own arc.
  int nx = 0, ny = 0, yx = 0, yy = 0;
  polar(no_mid, GLYPH_R, &nx, &ny);
  polar(yes_mid, GLYPH_R, &yx, &yy);

  const int h = GLYPH_HALF;
  // Cross: two full diagonals.
  stroke(s, nx - h, ny - h, nx + h, ny + h, 2, no_col);
  stroke(s, nx + h, ny - h, nx - h, ny + h, 2, no_col);
  // Tick: a short fall to the low point, then a long rise past the top.
  stroke(s, yx - h, yy, yx - h / 3, yy + h, 2, yes_col);
  stroke(s, yx - h / 3, yy + h, yx + h, yy - h, 2, yes_col);
}

}  // namespace shell
