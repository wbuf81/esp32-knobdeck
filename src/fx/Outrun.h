#pragma once

// A synthwave horizon: sun, grid, vanishing point.
//
// This is a BACKDROP, not an overlay. It replaces the radial gradient rather
// than drawing over it, and that is the only way it can exist here: the rule in
// this project is never to add a pass that reads and writes every pixel, and
// the radial backdrop already owns every pixel at a measured 11.3ms. So this
// has to fit inside that budget, not beside it.
//
// It can, because the shape is a function of the ROW. Sky colour, ground
// colour, grid spacing and the sun's width all depend only on y, so each row is
// one computed value and a run of stores - which is strictly less work per
// pixel than the radial table's read, saturating add and dither index.
//
// The horizon sits above centre so the grid gets most of the disc. On a round
// screen the widest rows are the middle ones, and a grid is only legible where
// it is wide.

#include <cstdint>

#include "audio/Modulation.h"
#include "gfx/Surface.h"

namespace fx {

class Outrun {
 public:
  static constexpr int HORIZON = 150;
  // How many receding lines are tracked. Beyond about twenty they land closer
  // together than a pixel near the horizon and stop being visible at all.
  static constexpr int LINES = 22;

  void begin();
  void update(const audio::Modulation &m, float dt);
  // Writes every pixel it is given.
  void drawBand(gfx::Surface &s, uint16_t tint) const;

 private:
  float phase_ = 0.0f;  // 0..1, how far between one line and the next
  float speed_ = 1.0f;
};

}  // namespace fx
