#pragma once

// The meeting controller: two giant tappable halves, mic left, camera right.
//
// The first screen that uses the panel as a BUTTON SURFACE rather than a
// gesture pad - each half is drawn in its live state and tapping it toggles
// that state through Teams' own local API. The state display IS the button, so
// there is nothing to memorise mid-meeting.
//
// Three honesty rules, each with a pixel test:
//   - A live mic is unmissably different from a muted one; the whole screen is
//     the across-the-room glance before you speak.
//   - Unknown renders as unknown - a grey question, never a confident stale
//     state, because a green lie over a hot microphone is the failure this
//     project's oldest invariant exists to forbid.
//   - Pending (tapped, not yet confirmed by Teams) dims the half rather than
//     flipping it; the display only changes when Teams echoes the new truth.

#include <cstdint>

#include "gfx/Surface.h"

namespace views {

class TeamsScreen {
 public:
  void begin();

  // Once per frame. muted/camera are tri-state: -1 unknown, 0, 1.
  // call_s is seconds in the call, -1 for unknown.
  void prepare(int muted, int camera, bool mic_pending, bool cam_pending,
               int call_s);

  // Once per band. Writes every pixel it owns.
  void renderBand(gfx::Surface &s);

 private:
  int muted_ = -1;
  int camera_ = -1;
  bool mic_pending_ = false;
  bool cam_pending_ = false;
  char timer_[12] = {};
};

}  // namespace views
