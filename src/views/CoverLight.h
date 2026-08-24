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
#include "art/Image.h"
#include "fx/Particles.h"
#include "fx/Matrix.h"
#include "fx/Outrun.h"
#include "fx/Tetris.h"
#include "fx/Themes.h"
#include "gfx/BloomBand.h"
#include "gfx/Quad3D.h"
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
  static constexpr int MAX_RINGS = 8;

  void begin(uint32_t track_seed);

  // Swaps the emission strategy without disturbing anything expensive: the
  // pool, the gradient table and the bloom accumulator all stay put. Live
  // particles are left to die off naturally rather than cleared, so a change
  // mid-track crossfades instead of blinking.
  void setTheme(fx::ThemeId id);
  fx::ThemeId theme() const { return theme_; }

  // Borrowed, not owned: the cover lives in PSRAM and outlives any one view.
  // Null renders the backdrop and particles alone, which must look deliberate
  // rather than broken - a cover is missing for the first second or two of every
  // uncached album.
  void setCover(const art::Image *cover) { cover_ = cover; }

  // Once per frame, before any band.
  void update(const audio::Modulation &m, float dt, core::Rng &rng);
  // Once per band.
  void renderBand(gfx::Surface &s);
  // Once per frame, after the last band.
  void endFrame() { bloom_.endFrame(); }

  int particleCount() const { return parts_.live(); }
  // The cover's current half-extent in local units. Exposed for the same reason
  // particleCount() is: the slow breath is a number, and asserting on the number
  // is honest where asserting on pixels is not - the orbit's foreshortening
  // moves the cover's on-screen width by more than the breath does, so a pixel
  // measurement cannot separate the two.
  float coverHalf() const { return cover_half_; }
  // The album-derived accent colour, so the shell's progress ring belongs to
  // the same image rather than being a fixed brand colour laid over it.
  uint16_t tint() const { return tint_; }
  void setBloomStrength(uint8_t s) {
    bloom_.setStrength(s);
    bloom_locked_ = true;  // stop update() from overriding a deliberate override
  }
  // The harness turns the particle layer off to prove it is really contributing
  // rather than being drawn over by a later pass.
  void setParticlesEnabled(bool on) { particles_on_ = on; }

  // Recede, so something can be read on top.
  //
  // A translucent scrim over the disc would be the obvious way to make the
  // browser legible, and it would be a full-frame read-modify-write - the single
  // most expensive thing this renderer can do. Turning the scene down instead
  // costs nothing: the cover is skipped, the field thins out and the vignette
  // drops. It also reads better, as the visuals stepping back rather than
  // something being laid over them.
  void setAmbient(bool on) { ambient_ = on; }
  bool ambient() const { return ambient_; }

  // Per-pass microsecond accumulators. Present because guessing which pass
  // costs what was wrong twice: the first optimisation pass targeted the
  // gradient, which turned out to be 1 ms of a 114 ms frame.
  struct Timing {
    uint64_t backdrop = 0, cover = 0, particles = 0, bloom = 0;
    uint32_t frames = 0;
  };
  Timing &timing() { return t_; }

 private:
  void buildPalette(uint32_t seed);
  void buildGradient(const audio::Modulation &m);

  struct Ring {
    float r = 0.0f;      // current radius, pixels
    float speed = 0.0f;
    float width = 12.0f; // half-width of the crest, widens as it travels
    float power = 1.0f;  // peak brightness contribution
    float life = 0.0f;   // 1 at birth, 0 when gone
  };

  void spawnRing(float strength, float bass);

  // Local (lx, ly) on the cover plane to view space, through the current orbit
  // and tilt.
  gfx::Vec3 toView(float lx, float ly) const;
  void drawCover(gfx::Surface &s);

  const art::Image *cover_ = nullptr;
  gfx::Quad3D quad_;
  float orbit_ = 0.0f;
  float tilt_ = 0.0f;
  float cover_half_ = 0.21f;
  // The cover sits above centre so its reflection has somewhere to go. On a
  // round screen a centred subject with a reflection underneath runs the
  // reflection straight off the bottom of the disc.
  static constexpr float GROUP_Y = -0.075f;
  // How far the reflection extends, as a fraction of the cover's height, and
  // how many slices it fades over. Quad3D has no per-pixel gradient, so the
  // fade is four quads with falling tint - which costs nothing extra per pixel
  // and is indistinguishable from a smooth ramp at this size.
  static constexpr float REFL_EXTENT = 0.85f;
  static constexpr int REFL_SLICES = 4;
  fx::Particles parts_;
  // ~200 bytes, so it is simply always here rather than being conjured on a
  // theme change - which would mean allocating on a fragmenting heap mid-track.
  fx::Tetris tetris_;
  fx::Outrun outrun_;
  fx::Matrix matrix_;
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
  uint16_t tint_ = 0xFFFF;
  float clock_ = 0.0f;
  float emit_acc_ = 0.0f;
  bool half_beat_pending_ = false;
  // The contraction envelope: jumps to 1 on an onset, then releases. Driven by
  // dt like everything else here, so headless renders stay bit-exact.
  float thump_ = 0.0f;
  // Counts down to the second beat of a double pulse. Negative means none is
  // pending, which is distinguishable from "due this frame" at zero.
  float dub_in_ = -1.0f;
  Timing t_;
  bool bloom_locked_ = false;
  bool particles_on_ = true;
  fx::ThemeId theme_ = fx::ThemeId::CoverLight;
  bool ambient_ = false;
};

}  // namespace views
