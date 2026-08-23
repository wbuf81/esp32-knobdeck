#pragma once

// The transport furniture: rings and arcs, drawn on top of whatever view is
// running.
//
// Everything here is drawn PARAMETRICALLY - stepped along an angle and plotted -
// rather than by testing every pixel for whether it falls inside an annulus. A
// ring at r=168 covers a few thousand pixels; the disc has a hundred thousand.
// Walking the arc costs what the arc costs, and it clips to a band for free.
//
// This is the descendant of the ancestor project's StatusStrip, which was a
// 48-pixel band across the bottom of a rectangular screen. On a circle the same
// information wants to be a ring, and a ring is also what the knob is.

#include <cstdint>

#include "core/Deadline.h"
#include "gfx/Surface.h"

namespace shell {

class RadialShell {
 public:
  // Progress ring, just inside the bezel.
  static constexpr int PROGRESS_R = 170;
  static constexpr int PROGRESS_THICK = 3;
  // The volume ring sits on the same centreline, thicker, and only while the
  // knob is being turned.
  static constexpr int VOLUME_THICK = 9;
  static constexpr uint32_t VOLUME_SHOW_MS = 1400;

  // Called when the knob moves. `pct` may be -1, meaning the active device did
  // not report a volume - which must render as unknown rather than as zero.
  void showVolume(int pct, uint32_t now_ms);
  bool volumeVisible(uint32_t now_ms) const;

  // `progress01` and `tint` come from playback state; `beat` adds a subtle
  // pulse so the ring is alive even when paused.
  void render(gfx::Surface &s, float progress01, uint16_t tint, int volume_pct,
              uint32_t now_ms, float beat);

 private:
  Deadline volume_for_;
  int volume_pct_ = -1;
};

// Plots an arc of `thickness` pixels centred on radius `r`, from `a0` to `a1` in
// turns (0 = twelve o'clock, increasing clockwise), additively.
void drawArc(gfx::Surface &s, int r, int thickness, float a0, float a1,
             uint16_t color);

}  // namespace shell
