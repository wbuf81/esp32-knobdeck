#pragma once

// A transient message on the bottom strip.
//
// This exists because showToast() spent the whole project writing to a field no
// renderer read. toastActive() had zero callers and NowPlaying::prepare() only
// ever received PlaybackState, so it could not see the toast even in principle.
// Six call sites were silent - "No active device", "Nothing playing", "Volume
// not supported", "Not allowed", "Command failed", "Bad response" - and that is
// worse than having no error reporting at all: the code LOOKS like it reports
// errors, so nobody goes looking for why a gesture did nothing.
//
// prepare()/render() split like NowPlaying, for the same measured reason:
// measuring and fitting a string eighteen times a frame once cost half the
// frame rate.
//
// Drawn in PLACE OF the time row, not over it. Two strings sharing one baseline
// is unreadable, and between a timecode you can infer and an error you cannot,
// the error wins.

#include <cstdint>

#include "gfx/Surface.h"

namespace shell {

class Toast {
 public:
  // Once per frame. `msg` may be null or empty, and `active` is the caller's
  // deadline check - BOTH must hold, because showToast never clears the text,
  // only the Deadline. Drawing on non-empty text alone would leave the last
  // error on screen forever.
  void prepare(const char *msg, bool active);

  bool visible() const { return visible_; }

  // Once per band. Draws only; measures nothing.
  void render(gfx::Surface &s, uint16_t tint) const;

 private:
  char text_[64] = {};
  int max_w_ = 0;
  bool visible_ = false;
};

}  // namespace shell
