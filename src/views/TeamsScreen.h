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

#include "core/Rng.h"
#include "fx/Particles.h"
#include "gfx/Surface.h"

namespace views {

class TeamsScreen {
 public:
  void begin();

  // Once per frame. muted/camera are tri-state: -1 unknown, 0, 1.
  // call_s is seconds in the call, -1 for unknown.
  // Detects confirmed state flips (known -> known) and queues a burst for
  // update(); unknown -> known is the link finding its feet, never a burst.
  void prepare(int muted, int camera, bool mic_pending, bool cam_pending,
               int call_s);

  // Once per frame, after prepare(). Drives the particle layer: flip bursts,
  // the live-mic ambient field, and the pending swirl. Takes dt and a seeded
  // rng, never a clock, so headless runs stay bit-exact.
  void update(float dt, core::Rng &rng);

  // Once per band. Writes every pixel it owns.
  void renderBand(gfx::Surface &s);

  int liveParticles() const { return parts_.live(); }

 private:
  int muted_ = -1;
  int camera_ = -1;
  bool mic_pending_ = false;
  bool cam_pending_ = false;
  int call_s_ = -1;
  char timer_[12] = {};
  // Queued confirmed flips, consumed by the next update(). 0 = none;
  // +1 = turned on-air (mic went LIVE / camera ON), -1 = the calm direction.
  int mic_flip_ = 0;
  int cam_flip_ = 0;
  float ember_acc_ = 0.0f;  // fractional embers owed, metered against dt
  float swirl_acc_ = 0.0f;  // same metering for the pending swirl
  // Sized to the worst case actually reachable here (a flip burst over the
  // ember field and a swirl, ~250 live), NOT the music views' 1000: this
  // object is a global, globals are internal RAM, and a second 38KB pool
  // starved the TLS stack - handshakes failed while the screen ran fine.
  fx::BasicParticles<320> parts_;
};

}  // namespace views
