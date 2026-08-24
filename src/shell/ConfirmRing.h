#pragma once

// "Play this one?" - the commit gate in front of a queue jump.
//
// The control is the RING, not a pair of buttons. A tick and a cross sit at the
// bottom-left and bottom-right of the disc with an arc under each, and turning
// the knob swings a bright marker between them. That is deliberate: the bezel is
// the only control this device has, so the confirmation should look like the
// thing your fingers are already on rather than like a dialog box borrowed from
// a screen with a mouse.
//
// The glyphs are PLOTTED, not typeset. The fonts here are generated at
// 0x20-0xFF, so U+2713 and U+2717 would come out as the fallback box - and a
// tofu glyph on the one screen that asks "are you sure?" is the worst possible
// place for it. Two strokes each, drawn parametrically, cost no flash and stay
// crisp at any size.
//
// prepare()/render() split for the same reason as ListView and NowPlaying:
// measuring and wrapping the title once per band cost half the frame rate when
// that rule was broken.

#include <cstdint>

#include "gfx/Surface.h"

namespace shell {

class ConfirmRing {
 public:
  // Same centreline as the progress ring, so the two never fight for the eye:
  // the player's ring and this one occupy the same groove.
  static constexpr int ARC_R = 170;
  static constexpr int ARC_THICK = 9;
  static constexpr int MARKER_THICK = 17;

  // Angles in turns, 0 = twelve o'clock, increasing clockwise - drawArc's
  // convention. Both arcs live in the bottom half, under the text and away from
  // the heading.
  static constexpr float NO_A0 = 0.560f;
  static constexpr float NO_A1 = 0.720f;
  static constexpr float YES_A0 = 0.280f;
  static constexpr float YES_A1 = 0.440f;
  static constexpr float MARKER_HALF = 0.013f;

  // Where the two glyphs sit, on a radius inside their arcs.
  static constexpr int GLYPH_R = 132;
  static constexpr int GLYPH_HALF = 13;

  static constexpr int HEADING_BASELINE = 116;
  static constexpr int LINE1_BASELINE = 172;
  static constexpr int LINE2_BASELINE = 202;
  static constexpr int MARGIN = 26;

  // Once per frame. `choice` is 0 at the cross and 1 at the tick, and is a float
  // because the marker glides to the knob rather than snapping - the same
  // reasoning as ListView's fractional scroll position.
  void prepare(const char *track, float choice);
  // Once per band.
  void render(gfx::Surface &s, uint16_t tint) const;

 private:
  char line1_[56] = {};
  char line2_[56] = {};
  int line1_x_ = 0;
  int line2_x_ = 0;
  int heading_x_ = 0;
  float choice_ = 0.0f;
};

}  // namespace shell
