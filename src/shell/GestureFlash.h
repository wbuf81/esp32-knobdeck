#pragma once

// A glyph flashed over the middle of the disc, to answer a gesture.
//
// This exists because of a measured silence. A long-press was tried twice and
// looked like nothing happened both times: once the command was correctly
// dropped because nothing was playing, and once it genuinely toggled the saved
// state - which no renderer in this project read. A device with one screen and
// no buttons has to say that it heard you, and it has to say it whether the
// answer was yes or no.
//
// Held at full strength, then faded. The hold is what makes it legible on a
// screen you are looking away from; the fade is what stops it being furniture.

#include <cstdint>

#include "Glyphs.h"
#include "gfx/Surface.h"

namespace shell {

class GestureFlash {
 public:
  // Long enough to read at a glance, short enough not to sit on the artwork.
  static constexpr uint32_t HOLD_MS = 260;
  static constexpr uint32_t FADE_MS = 340;
  static constexpr int HALF = 46;

  void show(Glyph g, uint32_t now_ms);
  // Subtraction, so this is correct across the 32-bit millis wrap. The device
  // is meant to run for months.
  bool visible(uint32_t now_ms) const;

  // Once per frame.
  void prepare(uint32_t now_ms);
  // Once per band.
  void render(gfx::Surface &s, uint16_t tint) const;

 private:
  Glyph what_ = Glyph::Play;
  uint32_t shown_ms_ = 0;
  bool armed_ = false;
  uint16_t alpha_ = 0;  // 0..256, computed in prepare()
};

}  // namespace shell
