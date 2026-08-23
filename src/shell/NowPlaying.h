#pragma once

// Title, artist and timecodes, laid out for a circle.
//
// Separate from RadialShell because the constraint is different: rings care
// about radius, text cares about the CHORD. The usable width at a given row is
// 2*sqrt(R^2 - dy^2), which where text naturally wants to sit is far narrower
// than the 360-pixel panel suggests - so every string is measured against the
// chord at its own baseline, not against the screen.
//
// SPLIT INTO prepare() AND render() ON PURPOSE. Everything that does not depend
// on which band is being drawn - measuring, truncating, formatting timecodes,
// solving the chord - happens once per frame in prepare(). Doing it inside the
// per-band draw cost half the frame rate: eighteen bands meant eighteen rounds
// of UTF-8 measurement, ellipsis fitting and snprintf per frame. It is the same
// lesson as the bloom's glow row, learned twice.

#include <cstdint>

#include "core/PlaybackState.h"
#include "gfx/Surface.h"

namespace shell {

class NowPlaying {
 public:
  // Baselines, chosen so the block sits under the cover and above the ring.
  static constexpr int TITLE_BASELINE = 258;
  static constexpr int ARTIST_BASELINE = 280;
  static constexpr int TIME_BASELINE = 308;
  // Inset from the disc edge, so text never runs into the bezel or the ring.
  static constexpr int MARGIN = 30;

  // Once per frame, before any band.
  void prepare(const PlaybackState &pb, uint32_t shown_ms);
  // Once per band. Draws only; measures nothing.
  void render(gfx::Surface &s, uint16_t tint) const;

 private:
  // Pre-fitted strings and their left edges, ready to draw.
  char title_[200] = {};
  char artist_[200] = {};
  char elapsed_[14] = {};
  char remaining_[16] = {};
  int title_x_ = 0;
  int artist_x_ = 0;
  int elapsed_x_ = 0;
  int remaining_x_ = 0;
  bool have_track_ = false;
  bool have_times_ = false;
};

}  // namespace shell
