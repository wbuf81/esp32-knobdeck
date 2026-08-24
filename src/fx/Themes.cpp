#include "Themes.h"

#include <cmath>

namespace fx {

const char *themeName(ThemeId id) {
  switch (id) {
    case ThemeId::CoverLight: return "Cover Light";
    case ThemeId::Heartbeat: return "Heartbeat";
    case ThemeId::Rain: return "Rain";
    case ThemeId::Tetris: return "Tetris";
    case ThemeId::Outrun: return "Outrun";
    case ThemeId::Count: break;
  }
  return "?";
}

void themeSpawn(ThemeId id, const uint16_t palette[16], SpawnParams *out) {
  SpawnParams p;
  p.x = 180.0f;
  p.y = 180.0f;
  for (int i = 0; i < 16; ++i) p.colors[i] = palette[i];
  p.color_count = 16;

  switch (id) {
    case ThemeId::CoverLight:
      // The original, unchanged. Comet speeds with heavy drag: at 60fps
      // anything under about 52 px/s moves less than a pixel per frame and
      // there is no streak to draw at all.
      p.spread = 54.0f;
      p.speed_min = 40.0f;
      p.speed_max = 210.0f;
      p.life_min = 1.5f;
      p.life_max = 4.0f;
      p.size_min = 1.0f;
      p.size_max = 3.4f;
      p.drag = 0.30f;
      p.gravity_y = 0.0f;
      break;

    case ThemeId::Heartbeat:
      // Slow, heavy, short-lived motes that barely drift. Everything visible
      // here comes from the BEAT rather than from the drizzle, so the steady
      // emission is deliberately close to nothing - see themeEmit.
      p.spread = 30.0f;
      p.speed_min = 30.0f;
      p.speed_max = 120.0f;
      p.life_min = 0.6f;
      p.life_max = 1.5f;
      p.size_min = 1.4f;
      p.size_max = 4.0f;
      p.drag = 0.12f;  // sheds speed fast, so a beat lands and stops
      p.gravity_y = 0.0f;
      break;

    case ThemeId::Rain:
      // Falls. Spawned across the full width above the disc with almost no
      // horizontal speed, so gravity does the work and every streak is
      // vertical - which is what makes it read as rain rather than as a
      // differently-tuned starfield.
      p.spread = 178.0f;
      p.speed_min = 10.0f;
      p.speed_max = 45.0f;
      p.life_min = 1.4f;
      p.life_max = 2.6f;
      // Fatter than the comet themes. A one-pixel streak reads as noise against
      // a bright album; rain has to be legible as individual drops, and the
      // pool has the room - see the rate below.
      p.size_min = 1.4f;
      p.size_max = 3.0f;
      p.drag = 0.92f;      // almost none: rain does not decelerate
      p.gravity_y = 260.0f;
      break;

    case ThemeId::Tetris:
      // The blocks are the effect. What is left is a faint dust so the disc is
      // not dead between pieces - slow, tiny, and drifting nowhere in
      // particular, which is deliberately the opposite of every other theme
      // here so it never competes with the falling shapes.
      p.spread = 150.0f;
      p.speed_min = 4.0f;
      p.speed_max = 22.0f;
      p.life_min = 2.0f;
      p.life_max = 5.0f;
      p.size_min = 1.0f;
      p.size_max = 1.6f;
      p.drag = 0.70f;
      p.gravity_y = 6.0f;
      break;

    case ThemeId::Outrun:
      // Sparse embers drifting UP, against the grid's downward rush. Motion in
      // the opposite direction is what gives the road its speed; particles
      // falling with it would just look like more grid.
      p.spread = 150.0f;
      p.speed_min = 14.0f;
      p.speed_max = 60.0f;
      p.life_min = 1.6f;
      p.life_max = 3.4f;
      p.size_min = 1.0f;
      p.size_max = 2.0f;
      p.drag = 0.80f;
      p.gravity_y = -34.0f;
      break;

    case ThemeId::Count:
      break;
  }
  *out = p;
}

int themeEmit(ThemeId id, const audio::Modulation &m, float dt, bool ambient,
              float *acc) {
  float rate = 0.0f;
  switch (id) {
    case ThemeId::CoverLight:
      rate = 44.0f + 76.0f * m.loudness;
      break;
    case ThemeId::Heartbeat:
      // Almost nothing between beats. The whole theme is the pulse.
      rate = 6.0f + 10.0f * m.loudness;
      break;
    case ThemeId::Rain:
      // Steady, and only mildly louder in loud music - rain that thinned out
      // during a quiet passage would read as a bug in the rain.
      //
      // 210 with a ~2s life settles at roughly 420 live drops, against the ~800
      // CoverLight runs at, so this is well inside the particle pass's measured
      // budget even though it looks like the busier effect.
      rate = 210.0f + 90.0f * m.loudness;
      break;
    case ThemeId::Tetris:
      rate = 14.0f + 18.0f * m.loudness;
      break;
    case ThemeId::Outrun:
      rate = 22.0f + 30.0f * m.loudness;
      break;
    case ThemeId::Count:
      break;
  }
  *acc += rate * (ambient ? 0.30f : 1.0f) * dt;
  const int n = static_cast<int>(*acc);
  if (n > 0) *acc -= static_cast<float>(n);
  return n > 0 ? n : 0;
}

int themeBurst(ThemeId id, const audio::Modulation &m, bool ambient) {
  float n = 0.0f;
  switch (id) {
    case ThemeId::CoverLight:
      n = 90.0f + 110.0f * m.bass;
      break;
    case ThemeId::Heartbeat:
      // Twice the punch and nothing in between: the point is the gap.
      n = 150.0f + 220.0f * m.bass;
      break;
    case ThemeId::Rain:
      // Rain does not react to the beat. A splash on every kick drum would be
      // two effects fighting.
      n = 0.0f;
      break;
    case ThemeId::Tetris:
      // The beat already turns every piece. Bursting as well would be the same
      // event said twice.
      n = 0.0f;
      break;
    case ThemeId::Outrun:
      n = 30.0f + 50.0f * m.bass;
      break;
    case ThemeId::Count:
      break;
  }
  return static_cast<int>(n * (ambient ? 0.25f : 1.0f));
}

bool themeDoublePulse(ThemeId id) { return id == ThemeId::Heartbeat; }

bool themeOwnsBackdrop(ThemeId id) { return id == ThemeId::Outrun; }

float themeDubDelay(ThemeId id) {
  return id == ThemeId::Heartbeat ? 0.17f : 0.0f;
}

float themeCoverPulse(ThemeId id) {
  // The album contracts like a muscle: a sharp jump on the onset, then a slow
  // release. 10% is large next to the 5% bass follow every theme already has,
  // which is the point - you should be able to see the thing beat.
  return id == ThemeId::Heartbeat ? 0.10f : 0.0f;
}

float themeRingScale(ThemeId id) {
  switch (id) {
    case ThemeId::CoverLight: return 1.0f;
    // The ring IS the heartbeat: a shockwave leaving the album on every onset.
    // Rings are baked into the radial lookup table, so leaning on them costs
    // nothing per pixel - which is the only reason this theme is affordable.
    case ThemeId::Heartbeat: return 1.7f;
    case ThemeId::Rain: return 0.25f;
    // No shockwave at all: a ring expanding through a grid of falling blocks
    // reads as a rendering fault rather than as a pulse.
    case ThemeId::Tetris: return 0.0f;
    // No ring: the backdrop is opaque and painted last-to-first per row, so a
    // shockwave baked into the radial table would simply not be visible.
    case ThemeId::Outrun: return 0.0f;
    case ThemeId::Count: break;
  }
  return 1.0f;
}

}  // namespace fx
