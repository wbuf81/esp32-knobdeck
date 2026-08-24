#include "Tetris.h"

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace fx {
namespace {

// The seven tetrominoes, four rotations each, as cell offsets from the piece's
// top-left. Written as coordinates rather than as 4x4 bitmasks: the masks are
// more compact and completely unreadable, and a transposed rotation is a bug
// you can only find by looking at the screen.
//
// Diagrams are the rotation-0 state. x runs right, y runs down.
struct Cell { int8_t x, y; };

const Cell SHAPE[7][4][4] = {
    // I  ####
    {{{0, 1}, {1, 1}, {2, 1}, {3, 1}},
     {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
     {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
     {{1, 0}, {1, 1}, {1, 2}, {1, 3}}},
    // O  ##
    //    ##
    {{{1, 0}, {2, 0}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {2, 1}}},
    // T   #
    //    ###
    {{{1, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
     {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
     {{1, 0}, {0, 1}, {1, 1}, {1, 2}}},
    // S   ##
    //    ##
    {{{1, 0}, {2, 0}, {0, 1}, {1, 1}},
     {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
     {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
     {{0, 0}, {0, 1}, {1, 1}, {1, 2}}},
    // Z  ##
    //     ##
    {{{0, 0}, {1, 0}, {1, 1}, {2, 1}},
     {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
     {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
     {{1, 0}, {0, 1}, {1, 1}, {0, 2}}},
    // J  #
    //    ###
    {{{0, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
     {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
     {{1, 0}, {1, 1}, {0, 2}, {1, 2}}},
    // L    #
    //    ###
    {{{2, 0}, {0, 1}, {1, 1}, {2, 1}},
     {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
     {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
     {{0, 0}, {1, 0}, {1, 1}, {1, 2}}},
};

// The classic colours. Deliberately NOT derived from the album palette, unlike
// everything else in this renderer: cyan-I and yellow-O are most of what makes
// a falling block read as Tetris rather than as a coloured square, and an
// album-tinted tetromino is just a square.
const uint16_t COLOR[7] = {
    gfx::rgb565(0, 240, 240),    // I cyan
    gfx::rgb565(240, 216, 0),    // O yellow
    gfx::rgb565(168, 0, 240),    // T purple
    gfx::rgb565(0, 224, 0),      // S green
    gfx::rgb565(240, 0, 0),      // Z red
    gfx::rgb565(0, 0, 240),      // J blue
    gfx::rgb565(240, 144, 0),    // L orange
};

constexpr int PIECE_PX = 4 * Tetris::CELL;

}  // namespace

void Tetris::respawn(int i, core::Rng &rng, bool above) {
  Piece &p = p_[i];
  p.shape = static_cast<uint8_t>(rng.next() % SHAPES);
  p.rot = static_cast<uint8_t>(rng.next() & 3u);
  // Anywhere across the width, snapped to the cell grid so pieces line up with
  // each other the way they would on a board.
  const int cols = gfx::W / CELL;
  p.x = static_cast<int16_t>(
      (static_cast<int>(rng.next() % static_cast<uint32_t>(cols)) * CELL) -
      PIECE_PX / 2);
  p.speed = 42.0f + rng.unit() * 78.0f;
  // The first fill is scattered ACROSS the screen, not stacked above it.
  //
  // Staggering them above the top looked right and was wrong: at 42-120 px/s a
  // piece needs several seconds to travel 360 pixels, so selecting this theme
  // showed an empty disc for about six seconds before the first block arrived.
  // After that they re-enter from just off the top, which is the only place a
  // recycled piece can appear from without popping into view mid-screen.
  p.y = above ? rng.unit() * static_cast<float>(gfx::H + PIECE_PX) -
                    static_cast<float>(PIECE_PX)
              : -static_cast<float>(PIECE_PX);
}

void Tetris::begin(core::Rng &rng) {
  for (int i = 0; i < MAX; ++i) respawn(i, rng, /*above=*/true);
}

void Tetris::update(const audio::Modulation &m, float dt, core::Rng &rng) {
  // Louder music falls faster, but never stops: a visualiser that freezes in a
  // quiet passage looks broken rather than calm.
  const float scale = 0.75f + 0.9f * m.loudness;
  for (int i = 0; i < MAX; ++i) {
    Piece &p = p_[i];
    p.y += p.speed * scale * dt;
    // The quarter-turn lands on the beat, so the piece that turns is the one
    // the music just hit.
    if (m.onset) p.rot = static_cast<uint8_t>((p.rot + 1) & 3);
    if (p.y > static_cast<float>(gfx::H)) respawn(i, rng, /*above=*/false);
  }
}

void Tetris::drawBand(gfx::Surface &s) const {
  for (int i = 0; i < MAX; ++i) {
    const Piece &p = p_[i];
    const int py = static_cast<int>(p.y);
    // One rejection for the whole piece rather than four per-cell tests.
    if (s.rejectsRows(py, py + PIECE_PX)) continue;

    const uint16_t face = COLOR[p.shape];
    // A darker inset, so four adjacent cells read as four blocks rather than as
    // one blob. This is the entire difference between "tetromino" and "shape".
    const uint16_t edge = gfx::rgb565(20, 20, 26);

    for (int c = 0; c < 4; ++c) {
      const Cell &cell = SHAPE[p.shape][p.rot][c];
      const int x0 = p.x + cell.x * CELL;
      const int y0 = py + cell.y * CELL;
      for (int y = y0; y < y0 + CELL; ++y) {
        if (!s.containsRow(y)) continue;
        uint16_t *row = s.row(y);
        const bool y_edge = (y == y0) || (y == y0 + CELL - 1);
        for (int x = x0; x < x0 + CELL; ++x) {
          // Discarded per pixel, never clamped: folding an off-screen column
          // onto the boundary is the mistake three clipping bugs here were made
          // of, and on a grid it would stripe the edge of the disc.
          if (x < 0 || x >= gfx::W) continue;
          const bool x_edge = (x == x0) || (x == x0 + CELL - 1);
          row[x] = (y_edge || x_edge) ? edge : face;
        }
      }
    }
  }
}

}  // namespace fx
