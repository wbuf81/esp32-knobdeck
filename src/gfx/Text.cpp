#include "Font.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "Geometry.h"

namespace gfx {
namespace {

// Codepoint used when a string contains something this font cannot show. A
// visible marker rather than a blank: silently dropping characters makes a
// truncated name look like the metadata is wrong.
constexpr uint32_t FALLBACK = 0x003F;  // '?'

// Decodes one UTF-8 sequence, advancing `p`. Returns the codepoint, or FALLBACK
// for anything malformed - Spotify metadata is user-supplied and a malformed
// byte must not walk the pointer off the end of the string.
uint32_t nextCodepoint(const char **p) {
  const uint8_t *s = reinterpret_cast<const uint8_t *>(*p);
  const uint8_t c = s[0];
  if (c < 0x80) {
    *p += 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
    *p += 2;
    return ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
  }
  if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
    *p += 3;
    return ((c & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
  }
  if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
      (s[3] & 0xC0) == 0x80) {
    *p += 4;
    return ((c & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) |
           ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
  }
  *p += 1;  // malformed: consume one byte so this always terminates
  return FALLBACK;
}

const GFXglyph *glyphFor(const GFXfont &f, uint32_t cp) {
  if (cp < f.first || cp > f.last) cp = FALLBACK;
  if (cp < f.first || cp > f.last) return nullptr;
  return &f.glyph[cp - f.first];
}

}  // namespace

int textWidth(const GFXfont &f, const char *utf8) {
  if (!utf8) return 0;
  int w = 0;
  const char *p = utf8;
  while (*p) {
    const uint32_t cp = nextCodepoint(&p);
    const GFXglyph *g = glyphFor(f, cp);
    if (g) w += g->xAdvance;
  }
  return w;
}

void drawText(Surface &s, const GFXfont &f, int x, int baseline,
              const char *utf8, uint16_t color) {
  if (!utf8) return;
  const char *p = utf8;
  while (*p) {
    const uint32_t cp = nextCodepoint(&p);
    const GFXglyph *g = glyphFor(f, cp);
    if (!g) continue;

    const uint8_t *bits = f.bitmap + g->bitmapOffset;
    const int gx = x + g->xOffset;
    const int gy = baseline + g->yOffset;

    // One bit per pixel, MSB first, packed continuously across rows - so the bit
    // index runs over the whole glyph rather than restarting each row.
    uint32_t bit = 0;
    for (int row = 0; row < g->height; ++row) {
      const int py = gy + row;
      const bool row_visible = s.containsRow(py) && py >= 0 && py < H;
      if (!row_visible) {
        bit += g->width;  // still has to advance through the bitstream
        continue;
      }
      uint16_t *dst = s.row(py);
      for (int col = 0; col < g->width; ++col, ++bit) {
        if (!(bits[bit >> 3] & (0x80u >> (bit & 7)))) continue;
        const int px = gx + col;
        if (px < 0 || px >= W) continue;
        dst[px] = color;
      }
    }
    x += g->xAdvance;
  }
}

void drawTextCentered(Surface &s, const GFXfont &f, int cx, int baseline,
                      const char *utf8, uint16_t color) {
  drawText(s, f, cx - textWidth(f, utf8) / 2, baseline, utf8, color);
}

int drawTextFit(Surface &s, const GFXfont &f, int cx, int baseline,
                const char *utf8, int max_w, uint16_t color) {
  if (!utf8 || max_w <= 0) return 0;
  const int full = textWidth(f, utf8);
  if (full <= max_w) {
    drawText(s, f, cx - full / 2, baseline, utf8, color);
    return full;
  }

  // Truncate on a CHARACTER boundary, not a byte one: cutting a multi-byte
  // sequence in half would render the tail as a fallback glyph, so a trimmed
  // accented name would end in stray punctuation.
  const GFXglyph *dot = glyphFor(f, '.');
  const int ell = dot ? dot->xAdvance * 3 : 0;
  const int budget = max_w - ell;

  char buf[192];
  int used = 0;
  int w = 0;
  const char *p = utf8;
  while (*p) {
    const char *start = p;
    const uint32_t cp = nextCodepoint(&p);
    const GFXglyph *g = glyphFor(f, cp);
    const int adv = g ? g->xAdvance : 0;
    if (w + adv > budget) break;
    const int nbytes = static_cast<int>(p - start);
    if (used + nbytes >= static_cast<int>(sizeof(buf)) - 4) break;
    std::memcpy(buf + used, start, static_cast<size_t>(nbytes));
    used += nbytes;
    w += adv;
  }
  buf[used] = '\0';

  char out[196];
  std::snprintf(out, sizeof(out), "%s...", buf);
  const int total = textWidth(f, out);
  drawText(s, f, cx - total / 2, baseline, out, color);
  return total;
}

int halfChordAt(int y, int margin) {
  const int dy = y - CY;
  const int r = RADIUS - margin;
  const int rem = r * r - dy * dy;
  if (rem <= 0) return 0;
  int h = 0;
  while ((h + 1) * (h + 1) <= rem) ++h;
  return h;
}

}  // namespace gfx
