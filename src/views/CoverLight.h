#pragma once

// "Cover Light": the album art as the light source.
//
// Composited per band, in screen coordinates, so the identical source renders a
// 40-row device band and a full desktop frame.
//
// The backdrop is a function of RADIUS ALONE, which on this board is worth a
// great deal. It means the whole backdrop - falloff, colour, and any number of
// concurrent expanding shockwave rings - is baked into one lookup table per
// frame, and costs a shift and a table read per pixel no matter how much is
// happening in it. Radially symmetric effects are effectively free here, and
// that shaped what this view does.

#include <cstdint>

#include "audio/Modulation.h"
#include "core/Rng.h"
#include "fx/Particles.h"
#include "gfx/BloomBand.h"
#include "gfx/Surface.h"

namespace views {

class CoverLight {
 public:
  // The radial table is indexed by squared distance >> 6. Squared distance
  // avoids a per-pixel sqrt, and the shift sets how much work rebuilding the
  // table costs every frame - which is real: it is rebuilt each frame because
  // it is beat-reactive. At >>6 the table is 1024 entries, which still puts
  // about 34 entries across a 7-pixel shockwave ring at mid radius, and the
  // only thing near the centre is a smooth glow where coarseness is invisible.
  static constexpr int GRAD_N = 1024;
  static constexpr int GRAD_SHIFT = 6;
  // Leading guard entries, so the +-1 radial dither offset can never produce a
  // negative index and the inner loop needs no clamp branch.
  static constexpr int GRAD_PAD = 2;
  static constexpr int MAX_RINGS = 6;

  void begin(uint32_t track_seed);

  // Once per frame, before any band.
  void update(const audio::Modulation &m, float dt, core::Rng &rng);
  // Once per band.
  void renderBand(gfx::Surface &s);
  // Once per frame, after the last band.
  void endFrame() { bloom_.endFrame(); }

  int particleCount() const { return parts_.live(); }
  void setBloomStrength(uint8_t s) { bloom_.setStrength(s); }

  // Per-pass microsecond accumulators. Present because guessing which pass
  // costs what was wrong twice: the first optimisation pass targeted the
  // gradient, which turned out to be 1 ms of a 114 ms frame.
  struct Timing {
    uint64_t backdrop = 0, particles = 0, bloom = 0;
    uint32_t frames = 0;
  };
  Timing &timing() { return t_; }

 private:
  void buildPalette(uint32_t seed);
  void buildGradient(const audio::Modulation &m);

  struct Ring {
    float r = 0.0f;     // current radius, pixels
    float speed = 0.0f;
    float life = 0.0f;  // 1 at birth, 0 when gone
  };

  fx::Particles parts_;
  gfx::BloomBand bloom_;

  // The radial gradient, packed RGB565.
  //
  // An RGB888 version of this table was tried and reverted: it removed an
  // unpack but forced a multiply-by-three index and three byte loads per pixel,
  // and measured slower. Packed means the whole backdrop write is two aligned
  // 16-bit loads, one saturating add and one store.
  uint16_t grad_[GRAD_PAD + GRAD_N] = {};
  // Normalised radius per table entry, precomputed once. The sqrt is the only
  // expensive part of the radial mapping and does not depend on anything that
  // changes per frame.
  float r01_[GRAD_N] = {};
  // Half-chord per screen row: the disc spans x in [CX-h, CX+h]. Looping only
  // inside it skips the 21% of the square that is never visible, and removes
  // the need to mask at all.
  int16_t half_[gfx::H] = {};
  uint16_t dx2_[gfx::W] = {};  // dx*dx, so the inner loop is an add and a shift
  uint16_t palette_[16] = {};
  Ring rings_[MAX_RINGS];

  float hue_ = 0.0f;
  float clock_ = 0.0f;
  float emit_acc_ = 0.0f;
  Timing t_;
};

}  // namespace views
