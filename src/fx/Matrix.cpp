#include "Matrix.h"

#include "gfx/Color.h"

namespace fx {
namespace {

// Sixteen glyphs, five wide and seven tall, one bit per pixel with bit 0 as the
// leftmost column. Invented rather than transcribed: what matters is that they
// read as a dense unfamiliar script at 10x14 pixels, and half-remembered
// katakana at this size is indistinguishable from shapes that simply have the
// right stroke weight.
const uint8_t GLYPH[16][7] = {
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00},  // T-bar
    {0x11, 0x1F, 0x11, 0x11, 0x11, 0x1F, 0x00},  // boxed
    {0x1F, 0x01, 0x1F, 0x10, 0x10, 0x1F, 0x00},  // S
    {0x04, 0x0E, 0x15, 0x04, 0x04, 0x04, 0x00},  // arrow up
    {0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00, 0x00},  // X
    {0x1F, 0x10, 0x1C, 0x10, 0x10, 0x1F, 0x00},  // E
    {0x0E, 0x11, 0x01, 0x06, 0x00, 0x04, 0x00},  // question
    {0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00},  // arch
    {0x04, 0x04, 0x1F, 0x04, 0x04, 0x1F, 0x00},  // double cross
    {0x02, 0x04, 0x1F, 0x04, 0x08, 0x00, 0x00},  // slash bar
    {0x1F, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x00},  // hourglass
    {0x08, 0x1F, 0x08, 0x0F, 0x02, 0x0C, 0x00},  // hook
    {0x11, 0x11, 0x1F, 0x01, 0x01, 0x01, 0x00},  // corner
    {0x0E, 0x11, 0x11, 0x11, 0x0E, 0x00, 0x00},  // ring
    {0x04, 0x1F, 0x04, 0x1F, 0x04, 0x04, 0x00},  // ladder
    {0x1F, 0x08, 0x04, 0x02, 0x01, 0x1F, 0x00},  // zig
};

// A cell's glyph, hashed rather than stored.
inline int glyphAt(int col, int row, uint16_t churn, uint8_t seed) {
  uint32_t h = static_cast<uint32_t>(col) * 73856093u ^
               static_cast<uint32_t>(row) * 19349663u ^
               static_cast<uint32_t>(churn) * 83492791u ^
               static_cast<uint32_t>(seed) * 2654435761u;
  h ^= h >> 13;
  return static_cast<int>(h & 15u);
}

}  // namespace

void Matrix::respawn(int c, core::Rng &rng) {
  // Starts above the top so a column enters rather than appearing.
  len_[c] = static_cast<uint8_t>(6 + rng.next() % 15u);
  head_[c] = -static_cast<float>(len_[c]) - rng.unit() * 12.0f;
  speed_[c] = 4.0f + rng.unit() * 12.0f;
  seed_[c] = static_cast<uint8_t>(rng.next() & 0xFFu);
}

void Matrix::begin(core::Rng &rng) {
  for (int c = 0; c < COLS; ++c) {
    respawn(c, rng);
    // The first fill is scattered over the whole screen rather than stacked
    // above it - the same bug Tetris had, where selecting the theme showed an
    // empty disc for several seconds before anything arrived.
    head_[c] = rng.unit() * static_cast<float>(ROWS + len_[c]);
  }
  churn_ = 0;
  churn_acc_ = 0.0f;
}

void Matrix::update(const audio::Modulation &m, float dt, core::Rng &rng) {
  const float scale = 0.7f + 1.3f * m.loudness;
  for (int c = 0; c < COLS; ++c) {
    head_[c] += speed_[c] * scale * dt;
    if (head_[c] - static_cast<float>(len_[c]) > static_cast<float>(ROWS))
      respawn(c, rng);
  }
  // The characters flicker on their own, and a beat scrambles them all at once.
  churn_acc_ += dt;
  if (churn_acc_ > 0.07f) {
    churn_acc_ = 0.0f;
    ++churn_;
  }
  if (m.onset) churn_ += 5;
}

void Matrix::drawBand(gfx::Surface &s) const {
  // The backdrop. Black, and written rather than assumed - this owns every
  // pixel it is handed.
  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);
    for (int x = 0; x < gfx::W; ++x) row[x] = 0x0000;
  }

  // Only the cell rows that touch this band.
  int r0 = s.y0 / CELL_H;
  int r1 = (s.yEnd() - 1) / CELL_H;
  if (r0 < 0) r0 = 0;
  if (r1 >= ROWS) r1 = ROWS - 1;

  for (int c = 0; c < COLS; ++c) {
    const float head = head_[c];
    const int len = len_[c];
    const int x0 = c * CELL_W + 1;

    for (int r = r0; r <= r1; ++r) {
      // Distance behind the head, in cells. Negative means this cell is ahead
      // of the drop and shows nothing.
      const float d = head - static_cast<float>(r);
      if (d < 0.0f || d > static_cast<float>(len)) continue;

      uint16_t col;
      if (d < 1.0f) {
        // The leading character is almost white, which is what gives the column
        // a direction. Without it a trail reads as a static gradient.
        col = gfx::rgb565(200, 255, 210);
      } else {
        const float t = 1.0f - d / static_cast<float>(len);
        const int g = 40 + static_cast<int>(200.0f * t);
        col = gfx::rgb565(0, static_cast<uint8_t>(g),
                          static_cast<uint8_t>(g / 5));
      }

      const uint8_t *bits = GLYPH[glyphAt(c, r, churn_, seed_[c])];
      const int y0 = r * CELL_H + 1;
      for (int gy = 0; gy < 7; ++gy) {
        const uint8_t bitrow = bits[gy];
        if (!bitrow) continue;
        for (int sy = 0; sy < 2; ++sy) {
          const int y = y0 + gy * 2 + sy;
          if (!s.containsRow(y)) continue;
          uint16_t *row = s.row(y);
          for (int gx = 0; gx < 5; ++gx) {
            if (!(bitrow & (1u << gx))) continue;
            const int x = x0 + gx * 2;
            if (x < 0 || x + 1 >= gfx::W) continue;
            row[x] = col;
            row[x + 1] = col;
          }
        }
      }
    }
  }
}

}  // namespace fx
