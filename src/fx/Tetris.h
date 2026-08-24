#pragma once

// Falling tetrominoes, rotating on the beat.
//
// Not a particle effect and not a game. There is no stack and no collision: a
// board that filled up would either need line clears and a lose condition, or
// it would settle into a still image - and a still image is the one thing a
// visualiser cannot be. Pieces fall past, rotate, and are recycled at the top.
//
// Rotation SNAPS through the four states of a real tetromino rather than
// spinning smoothly, because that discrete quarter-turn is what makes the shape
// read as Tetris rather than as a tumbling polygon. The turns land on beat
// onsets, so the piece that rotates is the one the music just hit.
//
// Cheap on purpose: twelve pieces of four cells is 48 filled squares a frame,
// which is a few thousand pixels against the backdrop's hundred thousand. This
// is what makes a themed backdrop affordable at all on this board.

#include <cstdint>

#include "audio/Modulation.h"
#include "core/Rng.h"
#include "gfx/Surface.h"

namespace fx {

class Tetris {
 public:
  static constexpr int MAX = 12;
  // 13px cells put a piece at 52px across - big enough to read the shape on a
  // 1.8-inch dial, small enough that four of them are not the whole screen.
  static constexpr int CELL = 13;
  static constexpr int SHAPES = 7;

  void begin(core::Rng &rng);
  void update(const audio::Modulation &m, float dt, core::Rng &rng);
  void drawBand(gfx::Surface &s) const;

  int live() const { return MAX; }

 private:
  void respawn(int i, core::Rng &rng, bool above);

  struct Piece {
    float y = 0.0f;      // top-left cell's y, in pixels
    int16_t x = 0;       // top-left cell's x, in pixels
    float speed = 60.0f;
    uint8_t shape = 0;
    uint8_t rot = 0;
  };
  Piece p_[MAX];
};

}  // namespace fx
