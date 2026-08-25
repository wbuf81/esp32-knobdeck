#include "CoverLight.h"

#include <cmath>

#if defined(DEVICE)
#include <esp_timer.h>
#define NOW_US() ((uint64_t)esp_timer_get_time())
#else
#define NOW_US() ((uint64_t)0)
#endif

#include "core/Hash.h"
#include "gfx/Blend.h"
#include "gfx/Color.h"
#include "gfx/Hsv.h"

namespace views {
namespace {

constexpr float TAU = 6.28318530718f;

inline float unitOf(uint32_t v) {
  return static_cast<float>(v >> 8) * (1.0f / 16777216.0f);
}

inline uint8_t clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Bayer 4x4 applied to the radial table index, so the values are +-1 steps of
// index rather than +-8 steps of colour. Centred on zero so the frame neither
// brightens nor darkens overall.
const int8_t kBayer4[4][4] = {
    {-1, 0, -1, 1},
    {1, -1, 1, 0},
    {-1, 1, -1, 0},
    {1, 0, 1, -1},
};

}  // namespace

void CoverLight::buildPalette(uint32_t seed) {
  hue_ = unitOf(seed * 2654435761u);
  // A tight analogous spread rather than the whole wheel. A palette drawn from
  // every hue reads as confetti; holding within about 50 degrees reads as light
  // of one colour, which is the point of the view.
  // A mid-brightness, saturated version of the palette hue: readable against
  // both a dark vignette and a bright cover, which a raw dominant colour is not.
  tint_ = gfx::hsv565(hue_, 0.75f, 0.85f);
  for (int i = 0; i < 16; ++i) {
    const float t = static_cast<float>(i) / 15.0f;
    const float h = hue_ + (t - 0.5f) * 0.18f;
    // Particles are the subject, so they run bright and stay saturated. The
    // brightest few entries go nearly white, which is what gives the field
    // sparkle rather than a uniform haze of one colour.
    const float s = 0.85f - 0.55f * t;
    const float v = 0.70f + 0.30f * t;
    palette_[i] = gfx::hsv565(h, s, v);
  }
}

void CoverLight::begin(uint32_t track_seed) {
  buildPalette(track_seed);
  for (int x = 0; x < gfx::W; ++x) {
    const int dx = x - gfx::CX;
    dx2_[x] = static_cast<uint16_t>(dx * dx);
  }
  for (int y = 0; y < gfx::H; ++y) {
    const int dy = y - gfx::CY;
    const int rem = gfx::RADIUS_SQ - dy * dy;
    int h = 0;
    if (rem > 0) while ((h + 1) * (h + 1) <= rem) ++h;
    half_[y] = static_cast<int16_t>(h);
  }
  const int max_idx = (gfx::RADIUS_SQ >> GRAD_SHIFT);
  for (int i = 0; i < GRAD_N; ++i) {
    r01_[i] = i <= max_idx
                  ? std::sqrt(static_cast<float>(i) / static_cast<float>(max_idx))
                  : 1.0f;
  }
  bloom_.begin();
  // A high threshold so only particles and ring crests bloom. At 40 the
  // vignette itself was above threshold across most of the disc, so the glow
  // was a second copy of the backdrop rather than a halo around anything.
  bloom_.setThreshold(120);
  bloom_.setStrength(140);

  fx::SpawnParams p;
  fx::themeSpawn(theme_, palette_, &p);
  p.x = static_cast<float>(gfx::CX);
  p.y = static_cast<float>(gfx::CY);
  // The speeds themselves are per-theme and live in fx::Themes, alongside the
  // reasoning for each. The one thing that is NOT per-theme is that a particle
  // is drawn as a segment from its previous position to its current one, so
  // anything under about 52 px/s moves less than a pixel per frame at 60fps and
  // has no streak to draw. Every theme's speeds respect that or deliberately
  // trade it for something else.
  parts_.configure(p);
  parts_.clear();

  for (int i = 0; i < MAX_RINGS; ++i) rings_[i] = Ring();
  clock_ = 0.0f;
  emit_acc_ = 0.0f;

  // Reseeded per track like everything else derived from the track id, so a
  // song always looks the same way.
  core::Rng r(track_seed ^ 0x7E7215u);
  tetris_.begin(r);
  outrun_.begin();
  matrix_.begin(r);
  record_.begin();
}

void CoverLight::setTheme(fx::ThemeId id) {
  if (id == theme_) return;
  theme_ = id;
  fx::SpawnParams p;
  fx::themeSpawn(theme_, palette_, &p);
  // The origin is re-anchored to the cover on the very next update(), so what
  // is set here only has to be a sane starting point.
  p.x = static_cast<float>(gfx::CX);
  p.y = static_cast<float>(gfx::CY);
  parts_.configure(p);
  // Deliberately NOT parts_.clear(). Live particles die off on their own, so a
  // theme change crossfades over a second or two instead of blinking.
  emit_acc_ = 0.0f;
}

void CoverLight::buildGradient(const audio::Modulation &m) {
  // A dark vignette, NOT a light source.
  //
  // The first version of this ran centre_v up to 0.65 with a core holding 30-60%
  // of the radius flat, and the result was a white blob that swallowed every
  // particle in the field. The backdrop's job is to give the particles and the
  // album art something to be bright against; anything brighter than about a
  // quarter scale and it becomes the subject instead of the ground.
  const float amb = ambient_ ? 0.35f : 1.0f;
  const float centre_v = (0.07f + 0.22f * m.loudness) * amb;
  const float falloff = 2.2f + 2.6f * m.mid;
  const float core = 0.06f + 0.10f * m.bass;  // fraction of radius held flat
  const float inv_core = 1.0f / (1.0f - core);

  const int max_idx = (gfx::RADIUS_SQ >> GRAD_SHIFT);

  // Which falloff powers to blend, so the curve is shapeable without pow().
  // std::pow costs one to two microseconds here, and a thousand of them per
  // frame is a measurable slice of a 33 ms budget for something the eye reads
  // as "the edge is softer".
  const bool low = falloff <= 2.0f;
  const float mix = low ? (falloff - 1.0f) : (falloff - 2.0f) * 0.5f;

  // The guard entries mirror index 0 so a dithered lookup at the very centre
  // reads the centre colour rather than black.
  for (int i = 0; i < GRAD_N; ++i) {
    if (i > max_idx) {
      // Outside the visible disc. Never read, because the backdrop loop is
      // bounded by the half-chord table - but zeroed so a bounds slip shows up
      // as a black wedge rather than as garbage.
      grad_[GRAD_PAD + i] = 0x0000;
      continue;
    }
    const float r01 = r01_[i];
    float v;
    if (r01 < core) {
      v = centre_v;
    } else {
      const float u = 1.0f - (r01 - core) * inv_core;
      const float u2 = u * u;
      const float f = low ? (u + (u2 - u) * mix)
                          : (u2 + (u2 * u2 - u2) * mix);
      v = centre_v * f;
    }

    // Bake every live shockwave ring into the same table. A ring is a bump in a
    // radial function, so any number of them cost nothing per pixel - which is
    // why this view leans on them.
    float ring_add = 0.0f;
    const float rp = r01 * static_cast<float>(gfx::RADIUS);
    for (int k = 0; k < MAX_RINGS; ++k) {
      if (rings_[k].life <= 0.0f) continue;
      const float d = rp - rings_[k].r;
      // Wide, and widening as it ages. The first version used 7 px and read as a
      // drawn outline rather than as a wave passing through - a shockwave needs
      // enough width to have an inside and an outside.
      const float w = rings_[k].width;
      if (d > -w && d < w) {
        const float a = (d < 0 ? -d : d) / w;
        const float f = (1.0f - a) * (ambient_ ? 0.45f : 1.0f);
        // Cubic: a bright crest with a long trailing skirt, rather than the
        // symmetric bump a squared falloff gives.
        ring_add += f * f * f * rings_[k].life * rings_[k].power;
      }
    }

    float vv = v + ring_add;
    if (vv > 1.0f) vv = 1.0f;
    // Saturation stays high. Easing it off as brightness rose was what turned
    // the centre white: a desaturating highlight reads as blown-out rather than
    // as intense, and there is nothing else in frame to give it scale.
    grad_[GRAD_PAD + i] =
        gfx::hsv565(hue_ + 0.06f * r01, 0.92f - 0.12f * vv, vv);
  }
  for (int k = 0; k < GRAD_PAD; ++k) grad_[k] = grad_[GRAD_PAD];
}

void CoverLight::spawnRing(float strength, float bass) {
  for (int k = 0; k < MAX_RINGS; ++k) {
    if (rings_[k].life > 0.0f) continue;
    rings_[k].r = 6.0f;
    rings_[k].speed = (170.0f + 150.0f * bass) * (0.7f + 0.5f * strength);
    rings_[k].width = 14.0f * strength + 8.0f;
    rings_[k].power = 0.95f * strength;
    rings_[k].life = 1.0f;
    return;
  }
}

gfx::Vec3 CoverLight::toView(float lx, float ly) const {
  // Rotate about y, then about x, then push away from the camera. Two axes is
  // enough: a third would spin the cover's own plane, which reads as a gimmick
  // rather than as depth.
  const float ca = std::cos(orbit_), sa = std::sin(orbit_);
  const float ct = std::cos(tilt_), st = std::sin(tilt_);
  const float x1 = lx * ca;
  const float z1 = lx * sa;
  const float y2 = ly * ct - z1 * st;
  const float z2 = ly * st + z1 * ct;
  // 1.62 puts a 0.34 half-extent at roughly 71 pixels on screen, so the cover
  // occupies about 40% of the disc and leaves the particle field somewhere to
  // live.
  return gfx::Vec3{x1, y2 + GROUP_Y, 1.62f + z2};
}

void CoverLight::drawCover(gfx::Surface &s) {
  // Skipped entirely while browsing. It is also the most expensive pass, so the
  // browser runs faster than the player rather than slower - which is the right
  // way round for a screen you are scrolling.
  if (ambient_) return;
  if (!cover_ || !cover_->valid()) return;
  const float h = cover_half_;

  gfx::Vec3 face[4] = {toView(-h, -h), toView(h, -h), toView(h, h),
                       toView(-h, h)};
  quad_.draw(s, *cover_, face, 256, 255);

  // The reflection: mirrored about the cover's bottom edge, cropped, and faded
  // out over four slices.
  //
  // The vertical flip is a UV rect with v running downward rather than a second
  // set of geometry - v0 = 1 at the reflection's top edge is what makes its top
  // the cover's bottom, which is the whole difference between a reflection and
  // a second cover stacked underneath.
  const float total = REFL_EXTENT * 2.0f * h;
  const uint8_t tints[REFL_SLICES] = {132, 96, 62, 30};
  for (int k = 0; k < REFL_SLICES; ++k) {
    const float f0 = static_cast<float>(k) / REFL_SLICES;
    const float f1 = static_cast<float>(k + 1) / REFL_SLICES;
    const float y0 = h + total * f0;
    const float y1 = h + total * f1;
    // v walks back up the texture as the reflection walks down the screen.
    const float v0 = 1.0f - f0 * REFL_EXTENT;
    const float v1 = 1.0f - f1 * REFL_EXTENT;
    gfx::Vec3 r[4] = {toView(-h, y0), toView(h, y0), toView(h, y1),
                      toView(-h, y1)};
    gfx::QuadUv uv;
    uv.u0 = 0.0f;
    uv.u1 = 1.0f;
    uv.v0 = v0;
    uv.v1 = v1;
    quad_.draw(s, *cover_, r, 150, tints[k], uv, /*bilinear=*/false);
  }
}

void CoverLight::update(const audio::Modulation &m, float dt, core::Rng &rng) {
  clock_ += dt;

  // A bounded sway, NOT a continuous revolution.
  //
  // orbit_ used to grow without limit, so the cover turned fully edge-on twice
  // per revolution and all but vanished. An album cover is the subject of this
  // view; it should never disappear. +-0.5 rad is enough perspective for the
  // near edge to read as nearer without ever losing the face.
  orbit_ = std::sin(clock_ * 0.23f) * 0.50f;
  tilt_ = std::sin(clock_ * 0.31f + 1.1f) * 0.20f;
  // 0.21 puts the cover at roughly 100 px tall on screen. Larger was tried at
  // 0.30 and the cover plus its reflection ran off the top and bottom of the
  // disc - a round screen has far less usable vertical extent than its pixel
  // height suggests, and the reflection doubles whatever the subject occupies.
  // The release. The attack is in the onset branch below, and the size itself
  // is computed AFTER it - a contraction that landed a frame after its own beat
  // is a contraction you can see is late.
  thump_ -= thump_ * dt * 7.0f;
  if (thump_ < 0.0f) thump_ = 0.0f;

  // Anchor the field to the cover rather than to the middle of the screen.
  //
  // The cover hangs above centre so its reflection has somewhere to go, so an
  // origin at (CX, CY) emitted from below the art and the streaks fanned out
  // lopsided - more below the cover than above it. Projecting the cover's own
  // centre through the same transform the quad uses costs one divide a frame and
  // makes the field genuinely radiate from behind the album.
  if (theme_ == fx::ThemeId::Rain) {
    // Rain comes from the sky, not from the album. Spawned above the top of the
    // disc so a drop is already at full speed by the time it is visible -
    // starting them on screen makes the top edge look like a spawn line.
    parts_.setOrigin(static_cast<float>(gfx::CX), -24.0f);
  } else {
    const gfx::Vec3 c = toView(0.0f, 0.0f);
    parts_.setOrigin(
        static_cast<float>(gfx::CX) + c.x * gfx::Quad3D::FOCAL / c.z,
        static_cast<float>(gfx::CY) + c.y * gfx::Quad3D::FOCAL / c.z);
  }

  // Advance rings.
  for (int k = 0; k < MAX_RINGS; ++k) {
    if (rings_[k].life <= 0.0f) continue;
    rings_[k].r += rings_[k].speed * dt;
    rings_[k].width += 26.0f * dt;  // widens as it travels, like a real wave
    rings_[k].life -= dt * 0.95f;
    if (rings_[k].r - rings_[k].width > gfx::RADIUS) rings_[k].life = 0.0f;
  }

  if (m.onset) {
    thump_ = 1.0f;
    if (fx::themeDoublePulse(theme_)) dub_in_ = fx::themeDubDelay(theme_);
    spawnRing(fx::themeRingScale(theme_), m.bass);
    // Comet-fast, so the streaks actually streak. See SpawnParams below.
    const int burst = fx::themeBurst(theme_, m, ambient_);
    if (burst > 0) parts_.burst(burst, 0.85f + m.bass, rng);
    half_beat_pending_ = true;
  }

  // The DUB: a weaker ring a fixed moment after the first, which is what makes
  // a heartbeat a heartbeat rather than a pulse.
  if (dub_in_ >= 0.0f) {
    dub_in_ -= dt;
    if (dub_in_ <= 0.0f) {
      dub_in_ = -1.0f;
      thump_ = 0.62f;  // the second contraction, softer than the first
      spawnRing(0.62f * fx::themeRingScale(theme_), m.bass * 0.8f);
    }
  }

  // A smaller ring on the half beat. Rings are baked into the radial table, so
  // a second one costs nothing per pixel - which is the only reason it is worth
  // having a train of them rather than one.
  //
  // Suppressed for a double-pulse theme: the dub already occupies that gap, and
  // three rings per beat is a smear rather than a rhythm.
  if (half_beat_pending_ && m.beat_phase > 0.5f) {
    half_beat_pending_ = false;
    if (!fx::themeDoublePulse(theme_))
      spawnRing(0.5f * fx::themeRingScale(theme_), m.bass * 0.6f);
  }

  // A steady drizzle so the field never empties between beats. Accumulated as a
  // float so the rate is frame-rate independent rather than per-frame.
  // Now that this frame's thump is known.
  //
  // 0.235 rather than the original 0.21, and the ceiling is real: 0.30 ran the
  // cover and its reflection off the top and bottom of the disc, because a round
  // screen has far less usable vertical extent than its pixel height suggests.
  // The breath is a ~18s sine off accumulated dt - never a wall clock, or
  // headless renders would stop being bit-exact.
  const float breath = 1.0f + 0.04f * std::sin(clock_ * 0.35f);
  const float pulse = 1.0f + fx::themeCoverPulse(theme_) * thump_;
  cover_half_ = 0.235f * (1.0f + 0.05f * m.bass) * breath * pulse;

  const int n = fx::themeEmit(theme_, m, dt, ambient_, &emit_acc_);
  if (n > 0) parts_.emit(n, rng);

  // Only the theme that shows them pays for them.
  if (theme_ == fx::ThemeId::Tetris) tetris_.update(m, dt, rng);
  if (theme_ == fx::ThemeId::Outrun) outrun_.update(m, dt);
  if (theme_ == fx::ThemeId::Matrix) matrix_.update(m, dt, rng);
  // No Modulation and no rng: the spin is constant by design, so this theme is
  // a pure function of accumulated dt.
  if (theme_ == fx::ThemeId::Record) record_.update(dt);

  parts_.update(dt);
  buildGradient(m);

  if (!bloom_locked_)
    bloom_.setStrength(static_cast<uint8_t>(
        (105 + 110.0f * m.loudness) * (ambient_ ? 0.45f : 1.0f)));
}

void CoverLight::renderBand(gfx::Surface &s) {
  uint64_t t0 = NOW_US();

  // A theme that owns the backdrop REPLACES this pass rather than adding to it.
  //
  // That is the whole reason it is affordable. The radial sweep below writes
  // every pixel at a measured 11.3ms; drawing a second full-screen layer over it
  // would be the read-and-write-every-pixel pass this renderer refuses to have.
  if (fx::themeOwnsBackdrop(theme_)) {
    if (theme_ == fx::ThemeId::Matrix) matrix_.drawBand(s);
    else if (theme_ == fx::ThemeId::Record) record_.drawBand(s, cover_, tint_);
    else outrun_.drawBand(s, tint_);
    const uint64_t tb = NOW_US();
    t_.backdrop += tb - t0;
    // The rest of the frame is unchanged: field, cover, bloom.
    if (particles_on_) parts_.render(s);
    const uint64_t tp = NOW_US();
    t_.particles += tp - tb;
    // Record draws the art ITSELF, as the disc. Drawing the flat cover over it
    // as well would be saying the same thing twice, with the second one
    // covering the first.
    if (theme_ != fx::ThemeId::Record) drawCover(s);
    const uint64_t tc = NOW_US();
    t_.cover += tc - tp;
    bloom_.accumulateBand(s);
    t_.bloom += NOW_US() - tc;
    ++t_.frames;
    return;
  }

  // Pass 1: backdrop, glow and dither, in one WRITE-ONLY sweep.
  //
  // Fused deliberately. Measured on this board, a separate read-modify-write
  // pass over every pixel to add the glow cost 84 ms of a 115 ms frame. This
  // loop touches each pixel exactly once, never reads it back, and only covers
  // the visible disc.
  for (int y = s.y0; y < s.yEnd(); ++y) {
    const int dy = y - gfx::CY;
    const int dy2 = dy * dy;
    const int h = half_[y];
    uint16_t *row = s.row(y);

    const int x0 = gfx::CX - h;
    const int x1 = gfx::CX + h;
    // Outside the disc: never visible, but cleared so nothing stale shows if
    // the geometry is ever wrong.
    for (int x = 0; x < x0; ++x) row[x] = 0;
    for (int x = x1 + 1; x < gfx::W; ++x) row[x] = 0;
    if (h <= 0) continue;

    const uint16_t *gl = bloom_.glowRow(y);
    const int8_t *bay = kBayer4[y & 3];

    // Two pixels per iteration, written as one 32-bit store.
    //
    // The radial index is computed once for the pair. Measured, internal SRAM
    // does 181 MB/s on 32-bit stores against 91 on 16-bit, so the store alone is
    // twice as fast - and the gradient and glow both vary far too slowly for a
    // two-pixel horizontal step to be visible, especially under dither. The band
    // buffer is malloc'd and the row stride is 360, so an even x is always
    // 4-byte aligned.
    int x = x0;
    if (x & 1) {  // odd start: one single pixel to reach alignment
      const int idx = ((dx2_[x] + dy2) >> GRAD_SHIFT) + bay[x & 3];
      row[x] = gfx::addSat(grad_[GRAD_PAD + idx], gl[x]);
      ++x;
    }
    // One addSat per PAIR, not per pixel.
    //
    // buildGlowRow writes four identical values per small pixel, so for any even
    // x, gl[x] and gl[x+1] are always the same entry - both halves of a pair
    // always land inside one four-pixel glow block. That makes the second
    // saturating add provably redundant rather than approximately redundant.
    uint32_t *out = reinterpret_cast<uint32_t *>(&row[x]);
    // Deliberately NOT hand-unrolled.
    //
    // Two pairs per iteration was tried, on the theory that interleaving
    // independent load chains would hide the load latency this loop is bound by.
    // It measured slower (10.67 ms to 11.31 ms): -funroll-loops already
    // schedules this better than the hand version, which only added register
    // pressure. Left simple on the strength of the measurement.
    for (; x + 1 <= x1; x += 2, ++out) {
      const int idx = ((dx2_[x] + dy2) >> GRAD_SHIFT) + bay[x & 3];
      const uint16_t v = gfx::addSat(grad_[GRAD_PAD + idx], gl[x]);
      *out = (static_cast<uint32_t>(v) << 16) | v;
    }
    for (; x <= x1; ++x) {  // odd tail
      const int idx = ((dx2_[x] + dy2) >> GRAD_SHIFT) + bay[x & 3];
      row[x] = gfx::addSat(grad_[GRAD_PAD + idx], gl[x]);
    }
  }
  const uint64_t t1 = NOW_US();

  // Pass 2: particles, additive, BEHIND the cover.
  //
  // They used to be drawn last, on the theory that light in front of the subject
  // reads better than texture on it. On real artwork it did not: the field is
  // dense enough at the centre - where it is emitted - that streaks crossed the
  // face continuously and the album became hard to look at. Occluded by the
  // cover, the same field reads as light coming from behind it, which is what
  // the view is called.
  //
  // Nothing else changes: the cover is opaque, so the pixels it owns are simply
  // written after. The pass costs exactly what it did.
  if (particles_on_) parts_.render(s);
  // Behind the cover for the same reason the field is: the album is the
  // subject, and blocks crossing its face would be the wash-out this renderer
  // just spent a commit fixing.
  // Drawn in ambient too, unlike the cover. On the THEMES screen the backdrop
  // IS the preview, and a Tetris row that previewed as an empty disc would be
  // the one row on the menu that lied about itself.
  if (theme_ == fx::ThemeId::Tetris) tetris_.drawBand(s);
  const uint64_t t15 = NOW_US();

  // Pass 3: the cover and its reflection, over the field.
  drawCover(s);
  const uint64_t t2 = NOW_US();

  // Pass 3: bright-pass contribution for next frame's glow. Read-only and
  // subsampled to a quarter of the pixels.
  bloom_.accumulateBand(s);
  const uint64_t t3 = NOW_US();

  t_.backdrop += t1 - t0;
  t_.particles += t15 - t1;
  t_.cover += t2 - t15;
  t_.bloom += t3 - t2;
  ++t_.frames;
}

}  // namespace views
