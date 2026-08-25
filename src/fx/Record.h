#pragma once

// The album art as a spinning record.
//
// The third backdrop-owning theme, and the cheapest of the three. Nothing here
// is a particle: the whole frame is one pass that writes every pixel exactly
// once, replacing the radial sweep rather than drawing over it. That is the only
// reason a full-disc texture read per frame is affordable at all - the sweep it
// displaces was measured at 11.3 ms.
//
// The rotation is an AFFINE texture walk, not a per-pixel transform. Per frame
// there is one cos and one sin; per row, one starting (u, v); per pixel, two
// adds. Rotating each pixel's coordinates independently would be the same
// picture for a few hundred times the trigonometry, and this project's rule
// about never doing per-pixel what can be per-row exists because that mistake
// cost half the frame rate twice.
//
// The spin is CONSTANT at 33 1/3 RPM and does not react to the music. That is
// deliberate: a record that speeds up on the downbeat does not read as a record,
// it reads as a broken one. The liveliness comes from the particle field and the
// bloom, which still draw on top of this backdrop like they do over Matrix.
//
// Bit-exact like every other effect: the angle accumulates from dt and never
// reads a clock.

#include <cstdint>

#include "art/Image.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"

namespace fx {

class Record {
 public:
  // Turns per second. NINE SECONDS per revolution.
  //
  // Not 33 1/3 RPM, which was the first version and was far too fast to look
  // at. A real record spins at 1.8 s per revolution and reads as slow because
  // it is twelve inches across and sitting in the corner of the room; the same
  // rate on a 360-pixel disc filling your whole field of view reads as a
  // washing machine. The trivia lost to the eye, which is the right way round.
  static constexpr float TURNS_PER_S = 1.0f / 9.0f;

  // Outer edge of the disc, just inside where the progress ring lives so the
  // two do not touch.
  static constexpr int OUTER_R = 165;
  // Where the art stops and the paper label starts.
  static constexpr int LABEL_R = 46;
  // The spindle hole. Small enough to read as a hole rather than a dot.
  static constexpr int SPINDLE_R = 6;

  void begin();

  // 16.16 fixed point throughout the sampler. The first version used floats and
  // measured 23.8 ms for the backdrop pass - twice the radial sweep it replaces -
  // because two float-to-int conversions per pixel on an in-order core cost more
  // than all the arithmetic around them.
  static constexpr int FP = 16;

  // Once per frame. Takes no Modulation: the spin is constant by design.
  void update(float dt);

  // Once per band. Writes every pixel it is given, so no separate clear is
  // needed. `cover` may be NULL - before the first artwork lands on any track
  // change it is - and then the disc draws grooves and a label and invents no
  // art, because a plausible-looking picture would be a confident lie.
  void drawBand(gfx::Surface &s, const art::Image *cover, uint16_t tint) const;

  // Turns completed. Exposed for the frame-rate-independence test; a wrapped
  // value would make that assertion meaningless, so this does not wrap.
  float turns() const { return turns_; }

 private:
  float turns_ = 0.0f;
  // dx*dx per column. Constant for the life of the object, so the disc test is
  // an add and a compare rather than a multiply. Same trick as CoverLight's.
  uint16_t dx2_[gfx::W] = {};
};

}  // namespace fx
