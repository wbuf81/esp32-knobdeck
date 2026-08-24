#pragma once

// The idle view: Daisy asleep, shown when there is nothing playing.
//
// A particle field with no music behind it is a screensaver pretending to be a
// visualiser - it reacts to nothing, and the one honest thing a now-playing
// display can say when nothing is playing is that nothing is playing. A dog
// asleep says it better than a blank disc.
//
// The sprite is 55x41 8-bit indexed pixel art in flash (src/art/DaisyAssets.h),
// generated from the same GIFs as the ancestor project by tools/daisy_convert.py.
// Nothing here allocates and nothing is decoded at runtime: a cell is a palette
// index, and drawing is a table read per cell, not per pixel.
//
// Bit-exact like every other effect here: the animation runs off accumulated
// dt, never a clock, so headless renders stay reproducible.

#include <cstdint>

#include "art/DaisyAssets.h"
#include "gfx/Surface.h"

namespace views {

class DaisyIdle {
 public:
  // 5 puts the 55x41 sprite at 275x205, which is as large as the disc takes
  // before the dog's own outline starts meeting the bezel.
  static constexpr int SCALE = 5;
  // Sleep, mostly. A yawn every so often so the screen is not a still image -
  // a completely static frame reads as a crash on a device that is otherwise
  // always moving.
  static constexpr float YAWN_EVERY_S = 23.0f;

  void begin();
  // Once per frame.
  void update(float dt);
  // Once per band. Writes every pixel it owns, so no separate clear is needed.
  void renderBand(gfx::Surface &s);

  // The shell's ring colour while idle: a dim, calm blue that does not compete
  // with the sprite and cannot be mistaken for an album tint.
  uint16_t tint() const;

  daisy::DaisyAnim anim() const { return anim_; }
  int frame() const { return frame_; }

 private:
  float clock_ = 0.0f;
  float anim_clock_ = 0.0f;
  daisy::DaisyAnim anim_ = daisy::Daisy_Sleep;
  int frame_ = 0;
};

}  // namespace views
