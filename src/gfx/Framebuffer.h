#pragma once

// The frame, as plain memory.
//
// Native-endian RGB565, one allocation for the life of the program. The byte
// swap the panel bus wants happens once at presentation and nowhere else, so
// every blend, blur and texture fetch in this project operates on pixels whose
// channels are where they look like they are. The ancestor project stored
// sprites byte-swapped and documented that as the single most confusing trap on
// its board, having been bitten twice; this is that trap designed out.
//
// at()/set() are bounds-checked and are for tests and cold paths. Hot loops
// take pixels() and index it directly, having already clipped.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Geometry.h"

namespace gfx {

class Framebuffer {
 public:
  Framebuffer() : px_(count(), 0) {}

  static constexpr int width() { return W; }
  static constexpr int height() { return H; }
  static constexpr size_t count() { return static_cast<size_t>(W) * H; }

  uint16_t *pixels() { return px_.data(); }
  const uint16_t *pixels() const { return px_.data(); }

  void fill(uint16_t c);

  uint16_t at(int x, int y) const {
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;
    return px_[static_cast<size_t>(y) * W + x];
  }

  void set(int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    px_[static_cast<size_t>(y) * W + x] = c;
  }

 private:
  std::vector<uint16_t> px_;
};

}  // namespace gfx
