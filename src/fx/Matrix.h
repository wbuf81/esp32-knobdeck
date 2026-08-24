#pragma once

// Falling glyph columns.
//
// The other backdrop-owning theme, and the expensive-looking one. Drawing the
// glyphs with the real font was the obvious route and is the wrong one:
// gfx::drawText walks a proportional font's metrics per call, and this needs
// several hundred calls a frame. Instead the glyphs are a tiny procedural
// table - sixteen shapes, five by seven bits each, 112 bytes total - so a glyph
// costs a bit test per pixel and nothing per call.
//
// Which glyph a cell shows is HASHED from its column, its row and a slowly
// advancing churn counter, not stored. That is 660 cells of state avoided, and
// it makes the mutation free: bump the counter and the characters flicker.
//
// Like Outrun, this REPLACES the radial backdrop rather than drawing over it.

#include <cstdint>

#include "audio/Modulation.h"
#include "core/Rng.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"

namespace fx {

class Matrix {
 public:
  // 5x7 glyphs at 2x, plus a little air. 30 columns of 22 rows on a 360 disc.
  static constexpr int CELL_W = 12;
  static constexpr int CELL_H = 16;
  static constexpr int COLS = gfx::W / CELL_W;
  static constexpr int ROWS = gfx::H / CELL_H;

  void begin(core::Rng &rng);
  void update(const audio::Modulation &m, float dt, core::Rng &rng);
  // Writes every pixel it is given.
  void drawBand(gfx::Surface &s) const;

 private:
  void respawn(int c, core::Rng &rng);

  float head_[COLS] = {};   // leading cell, fractional so it glides
  float speed_[COLS] = {};  // cells per second
  uint8_t len_[COLS] = {};  // trail length in cells
  uint8_t seed_[COLS] = {};
  uint16_t churn_ = 0;
  float churn_acc_ = 0.0f;
};

}  // namespace fx
