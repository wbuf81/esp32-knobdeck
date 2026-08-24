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
// She also reacts to input. The idle screen is the one screen where a gesture
// has nothing to control - there is no track to skip and nothing to pause - so
// the dog IS the feedback. Each gesture gets its own mood, always the same one,
// because a random pick reads as a screensaver again rather than as a dog
// answering you.
//
// Bit-exact like every other effect here: the animation runs off accumulated
// dt, never a clock and never an RNG, so headless renders stay reproducible.
// That is why the reaction is a poke passed in rather than a touch read here.

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

  // What kind of input arrived, NOT which animation to play. The caller knows
  // it saw a swipe; it has no business knowing that a swipe means a tail wag.
  // Keeping the sprite vocabulary in here is what lets the mapping change
  // without touching the input code.
  enum class Reaction : uint8_t { Touch, Swipe, Hold, Turn };

  void begin();

  // Input arrived while idle. Wakes her into the mood for `r`.
  //
  // The same reaction again EXTENDS the current one instead of restarting it -
  // tapping twice in a second would otherwise snap the animation back to frame
  // 0, which is a visible stutter. A different reaction switches outright.
  void react(Reaction r);

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
  // Reacting loops the mood until its hold expires; Settling is the one pass of
  // drowsy on the way back down. Cutting straight from zoomies to a sleeping
  // dog reads as a dropped frame - the settle is what makes it look deliberate.
  enum class Mode : uint8_t { Sleeping, Reacting, Settling };

  float clock_ = 0.0f;       // since the last yawn, wake, or settle
  float anim_clock_ = 0.0f;  // within the current animation
  float hold_s_ = 0.0f;      // when the current reaction gives up the screen
  Mode mode_ = Mode::Sleeping;
  daisy::DaisyAnim anim_ = daisy::Daisy_Sleep;
  int frame_ = 0;
};

}  // namespace views
