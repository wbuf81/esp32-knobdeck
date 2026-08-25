#pragma once

// What the device shows when it has stopped trusting its own boot path.
//
// Deliberately the dumbest drawing code in the project: a flat backdrop, a
// warning ring, and five lines of text. No effects, no particles, no cover
// decode, no allocation, no clock. A diagnostic screen that can itself crash
// or stall is worth less than nothing, because it would be indistinguishable
// from the fault it is trying to report.
//
// It exists because a crash loop takes the USB-CDC serial down with it (see
// CLAUDE.md): once the board is looping there is no log to read and esptool
// cannot sync, so the only channel left is the panel. This is that channel.

#include <cstdint>

#include "gfx/Surface.h"

namespace views {

class SafeScreen {
 public:
  // `reset_reason` may be null - esp_reset_reason() has a default branch, and a
  // screen that faults while reporting a fault is the worst bug this file could
  // have. Nothing here is retained by pointer; the text is copied.
  void begin(const char *reset_reason, int streak);

  // Once per band. Writes every pixel it owns, like every other view here, so
  // no separate clear is needed.
  void renderBand(gfx::Surface &s);

 private:
  char reason_[32] = {};
  char streak_[32] = {};
};

}  // namespace views
