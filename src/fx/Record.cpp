#include "Record.h"

#include <cmath>

#include "gfx/Color.h"

namespace fx {
namespace {

// Squared radii, so the inner loop compares against these and never takes a
// square root. Same trick as gfx::RADIUS_SQ.
constexpr int OUTER_SQ = Record::OUTER_R * Record::OUTER_R;
constexpr int LABEL_SQ = Record::LABEL_R * Record::LABEL_R;
constexpr int SPINDLE_SQ = Record::SPINDLE_R * Record::SPINDLE_R;

// Vinyl, not black: a true black disc against the black surround makes the
// record invisible except for the art, so the grooves have nothing to sit on.
constexpr uint8_t VINYL = 14;

// Groove spacing in squared-radius steps. Working in r2 keeps the sqrt out of
// the loop; the visual consequence is grooves that crowd toward the rim, which
// is what a real record does anyway.
constexpr int GROOVE_MASK = 0x0400;

// The cover is at most a couple of hundred rows and this avoids the multiply
// that art::Image::at() does per sample. 300 covers the 150px art with room for
// a larger decode later.
constexpr int MAX_ROWS = 320;

}  // namespace

void Record::begin() {
  turns_ = 0.0f;
  for (int x = 0; x < gfx::W; ++x) {
    const int dx = x - gfx::CX;
    dx2_[x] = static_cast<uint16_t>(dx * dx);
  }
}

void Record::update(float dt) {
  // Accumulated, never wrapped: turns() is asserted against a known angle after
  // a known duration, and a wrap would silently make that test vacuous.
  turns_ += dt * TURNS_PER_S;
}

void Record::drawBand(gfx::Surface &s, const art::Image *cover,
                      uint16_t tint) const {
  const float a = turns_ * 6.283185307f;
  const float ca = std::cos(a);
  const float sa = std::sin(a);

  // The art is a picture disc: the cover's shorter side spans the disc's
  // diameter, so a square cover fills it edge to edge with the corners cropped
  // away by the circle.
  const bool have_art =
      cover != nullptr && cover->valid() && cover->height() <= MAX_ROWS;
  const int cw = have_art ? cover->width() : 0;
  const int ch = have_art ? cover->height() : 0;
  // Half the shorter side MINUS ONE. Mapping OUTER_R to exactly half puts the
  // rim pixel at index `half + half` = the width itself, one past the end - a
  // genuine off-by-one at the very edge of the disc, which the spin test caught
  // by way of the bounds guard below silently falling back to bare grooves.
  // Losing one pixel of a 150px cover is invisible; reading one past it is not.
  const float scale =
      have_art ? (static_cast<float>(cw < ch ? cw : ch) * 0.5f - 1.0f) /
                     static_cast<float>(OUTER_R)
               : 0.0f;

  const uint16_t surround = gfx::rgb565(0, 0, 0);
  const uint16_t vinyl = gfx::rgb565(VINYL, VINYL, VINYL + 2);
  const uint16_t groove = gfx::rgb565(VINYL + 10, VINYL + 10, VINYL + 13);
  const uint16_t spindle = gfx::rgb565(0, 0, 0);

  // Row base pointers, so a sample is an index rather than a multiply. Built
  // once per band, not per pixel.
  const uint16_t *rows[MAX_ROWS];
  if (have_art) {
    const uint16_t *base = cover->pixels();
    for (int i = 0; i < ch; ++i) rows[i] = base + static_cast<size_t>(i) * cw;
  }

  // 16.16 fixed point. Two int adds per pixel and a shift to sample, where the
  // float version cost two float adds and two float-to-int conversions - which
  // is where 23.8 ms of the first version's 23.76 ms backdrop pass went.
  const int32_t one = 1 << FP;
  const int32_t du = static_cast<int32_t>(ca * scale * one);
  const int32_t dv = static_cast<int32_t>(-sa * scale * one);
  const int32_t cu = static_cast<int32_t>(static_cast<float>(cw) * 0.5f * one);
  const int32_t cv = static_cast<int32_t>(static_cast<float>(ch) * 0.5f * one);
  const int32_t cw_fp = static_cast<int32_t>(cw) << FP;
  const int32_t ch_fp = static_cast<int32_t>(ch) << FP;

  // The sample can never fall outside the art, and this is worth stating rather
  // than testing 130,000 times a frame. `scale` maps the cover's SHORTER side to
  // the disc's diameter, so a point at radius r <= OUTER_R samples at radius
  // r * scale <= (shorter/2) from the cover's centre - inside the cover's own
  // inscribed circle. The four bounds compares the first version did per pixel
  // were unreachable. Checked once here so a corrupt cover size still cannot
  // walk off the buffer.
  const bool art_bounded =
      have_art && static_cast<int>(static_cast<float>(OUTER_R) * scale) + 1 <=
                      (cw < ch ? cw : ch) / 2;

  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);
    const int dy = y - gfx::CY;
    const int dy2 = dy * dy;

    // Solve the disc's span on this row instead of testing every pixel for it.
    // A third of the screen is surround, and this turns that third from a
    // compare-and-store into a store - the same reason drawArc walks its arc
    // rather than testing the annulus.
    int half_w = 0;
    if (dy2 < OUTER_SQ) {
      half_w = static_cast<int>(std::sqrt(
          static_cast<float>(OUTER_SQ - dy2)));
    }
    int x0 = gfx::CX - half_w;
    int x1 = gfx::CX + half_w;
    if (x0 < 0) x0 = 0;
    if (x1 > gfx::W) x1 = gfx::W;

    for (int x = 0; x < x0; ++x) row[x] = surround;
    for (int x = x1; x < gfx::W; ++x) row[x] = surround;
    if (x0 >= x1) continue;

    // One solve per row; the rest of the rotation is the two adds below. This is
    // the whole reason the theme is affordable.
    const float fdx0 = static_cast<float>(x0 - gfx::CX);
    const float fdy = static_cast<float>(dy);
    int32_t u = cu + static_cast<int32_t>((fdx0 * ca + fdy * sa) * scale * one);
    int32_t v = cv + static_cast<int32_t>((-fdx0 * sa + fdy * ca) * scale * one);

    if (art_bounded) {
      for (int x = x0; x < x1; ++x, u += du, v += dv) {
        const int r2 = static_cast<int>(dx2_[x]) + dy2;
        if (r2 > LABEL_SQ) {
          row[x] = rows[v >> FP][u >> FP];
        } else if (r2 <= SPINDLE_SQ) {
          row[x] = spindle;
        } else {
          // The label carries the track's tint - the one place this theme
          // reacts to anything, and it is the album's own colour, not the beat.
          row[x] = tint;
        }
      }
    } else {
      // No artwork yet, or a cover whose size this cannot trust. Grooves on
      // bare vinyl, and nothing invented: a plausible-looking picture here
      // would be a confident lie about what is playing.
      for (int x = x0; x < x1; ++x) {
        const int r2 = static_cast<int>(dx2_[x]) + dy2;
        if (r2 <= SPINDLE_SQ) row[x] = spindle;
        else if (r2 <= LABEL_SQ) row[x] = tint;
        else row[x] = (r2 & GROOVE_MASK) ? groove : vinyl;
      }
    }
  }
}

}  // namespace fx
