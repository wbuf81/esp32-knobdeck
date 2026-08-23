#include "ListView.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"

namespace shell {
namespace {

const gfx::GFXfont &fontFor(int level) {
  if (level == 0) return gfx::fontTitle();
  if (level == 1) return gfx::fontArtist();
  return gfx::fontSmall();
}

// Brightness by distance from the centre. Steep on purpose: a gentle ramp reads
// as four equally important rows, which is exactly what a selection must not
// look like.
uint16_t colorFor(int level, uint16_t tint) {
  switch (level) {
    case 0: return gfx::rgb565(245, 245, 250);
    case 1: return gfx::rgb565(130, 130, 145);
    default: return gfx::rgb565(66, 66, 78);
  }
  (void)tint;
}

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

}  // namespace

void ListView::prepare(const char *const *items, int count, float pos,
                       const char *heading, bool truncated, const char *note,
                       int current) {
  row_count_ = 0;
  note_[0] = '\0';
  empty_ = count <= 0;

  fitInto(heading_, sizeof(heading_), gfx::fontSmall(), heading,
          gfx::halfChordAt(HEADING_BASELINE, MARGIN) * 2);
  heading_x_ = gfx::CX - gfx::textWidth(gfx::fontSmall(), heading_) / 2;

  if (empty_) {
    fitInto(note_, sizeof(note_), gfx::fontArtist(),
            note && note[0] ? note : "empty",
            gfx::halfChordAt(CENTRE_BASELINE, MARGIN) * 2);
    note_x_ = gfx::CX - gfx::textWidth(gfx::fontArtist(), note_) / 2;
    return;
  }

  // Nearest whole item, with the remainder shifting every row. That fraction is
  // what makes the wheel glide instead of stepping.
  const int centre = static_cast<int>(std::lround(pos));
  const float frac = pos - static_cast<float>(centre);

  // Selection markers flank the centre row's TEXT, not the disc's chord.
  //
  // The chord was tried first and is 160-odd pixels from the name at this row -
  // far enough that the marks read as unrelated furniture, and sitting exactly
  // where the progress ring and the particle field already are. Anchoring them
  // to the text costs a measurement that prepare() is doing anyway and puts them
  // where the eye already is.
  tick_y_ = CENTRE_BASELINE - 6;
  tick_left_ = 0;
  tick_right_ = 0;

  for (int d = -WING - 1; d <= WING + 1; ++d) {
    const int idx = centre + d;
    if (idx < 0 || idx >= count) continue;
    if (!items[idx]) continue;

    const float fy = static_cast<float>(d) - frac;
    const int baseline =
        CENTRE_BASELINE + static_cast<int>(fy * ROW_SPACING);
    // Rows scrolled past the edge of the disc are simply not drawn.
    if (baseline < 30 || baseline > gfx::H - 22) continue;

    const int level = static_cast<int>(std::lround(std::fabs(fy)));
    Row &r = rows_[row_count_];
    const gfx::GFXfont &f = fontFor(level > 2 ? 2 : level);
    fitInto(r.text, sizeof(r.text), f, items[idx],
            gfx::halfChordAt(baseline, MARGIN) * 2);
    r.x = gfx::CX - gfx::textWidth(f, r.text) / 2;
    r.baseline = baseline;
    r.level = level > 2 ? 2 : level;
    r.current = idx == current;
    if (level == 0) {
      // Anchored to this row's measured extent, so they track a name of any
      // length instead of drifting away from a short one.
      const int w = gfx::textWidth(f, r.text);
      tick_left_ = r.x - 13;
      tick_right_ = r.x + w + 9;
    }
    if (++row_count_ >= static_cast<int>(sizeof(rows_) / sizeof(rows_[0])))
      break;
  }

  if (truncated) {
    // Says so, rather than letting a capped list read as the whole library.
    std::snprintf(note_, sizeof(note_), "first %d", count);
    note_x_ = gfx::CX - gfx::textWidth(gfx::fontSmall(), note_) / 2;
  }
}

void ListView::render(gfx::Surface &s, uint16_t tint) const {
  if (heading_[0])
    gfx::drawText(s, gfx::fontSmall(), heading_x_, HEADING_BASELINE, heading_,
                  tint);

  if (empty_) {
    if (note_[0])
      gfx::drawText(s, gfx::fontArtist(), note_x_, CENTRE_BASELINE, note_,
                    gfx::rgb565(120, 120, 130));
    return;
  }

  // Selection markers: two solid wedges pointing at the selected name.
  if (tick_left_ || tick_right_) {
    for (int k = 0; k < 9; ++k) {
      const int y = tick_y_ + k - 4;
      if (!s.containsRow(y)) continue;
      // Widest at the middle row, so each wedge is a triangle pointing inward.
      const int len = 5 - (k > 4 ? k - 4 : 4 - k);
      if (len <= 0) continue;
      uint16_t *row = s.row(y);
      for (int d = 0; d < len; ++d) {
        const int xl = tick_left_ - d;
        const int xr = tick_right_ + d;
        if (xl >= 0 && xl < gfx::W) row[xl] = tint;
        if (xr >= 0 && xr < gfx::W) row[xr] = tint;
      }
    }
  }

  for (int i = 0; i < row_count_; ++i) {
    const Row &r = rows_[i];
    if (!r.text[0]) continue;
    // The playing row takes the album tint, so "where am I in this" is answered
    // by colour rather than by counting.
    const uint16_t col = r.current ? tint : colorFor(r.level, tint);
    gfx::drawText(s, fontFor(r.level), r.x, r.baseline, r.text, col);
  }

  if (note_[0])
    gfx::drawText(s, gfx::fontSmall(), note_x_, gfx::H - 34, note_,
                  gfx::rgb565(90, 90, 100));
}

}  // namespace shell
