#pragma once

// The visual themes, as strategies rather than as views.
//
// A theme is NOT its own renderer, and that is a memory decision, not a taste
// one. fx::Particles is about 38KB - a thousand particles across nine float
// arrays - and internal SRAM on this board is already 56% used. Six themes each
// owning a pool would be 228KB and simply would not fit. So CoverLight keeps
// the expensive parts - the pool, the gradient table, the bloom accumulator,
// the cover quad - and a theme is a small object that steers them. Six themes
// then cost about what one does.
//
// Nothing here allocates and nothing here holds state that outlives a track.

#include <cstdint>

#include "audio/Modulation.h"
#include "core/Rng.h"
#include "fx/Particles.h"

namespace fx {

enum class ThemeId : uint8_t {
  CoverLight = 0,  // the original: comets from behind the album
  Heartbeat,
  Rain,
  Count,
};

// The row order of the THEMES screen, with Shuffle sitting above them all.
const char *themeName(ThemeId id);

// How a theme wants the pool configured. Applied once when the theme is
// selected, not per frame.
void themeSpawn(ThemeId id, const uint16_t palette[16], SpawnParams *out);

// The steady per-frame emission. Returns how many particles to emit this frame;
// `acc` carries the fractional remainder so the rate is frame-rate independent
// rather than per-frame, which is the same reason CoverLight accumulates.
int themeEmit(ThemeId id, const audio::Modulation &m, float dt, bool ambient,
              float *acc);

// How hard a beat hits. Returns the burst size; 0 means this theme does not
// burst on onsets.
int themeBurst(ThemeId id, const audio::Modulation &m, bool ambient);

// Multiplier on the shockwave ring's strength, so a theme can lean on the
// radial backdrop or stay out of it entirely. Rings are baked into a lookup
// table, so this costs nothing per pixel however hard it is used.
float themeRingScale(ThemeId id);

}  // namespace fx
