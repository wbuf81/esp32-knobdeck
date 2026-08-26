#include "Backlight.h"

namespace core {

uint8_t Backlight::duty() const {
  switch (state_) {
    case ScreenState::Bright: return BRIGHT;
    case ScreenState::Dim: return DIM;
    default: return OFF;
  }
}

void Backlight::update(uint32_t now_ms, bool playing, uint32_t last_input_ms,
                       bool host_asleep) {
  const ScreenState was = state_;

  // Unsigned subtraction, so this is correct across the 49.7-day millis wrap -
  // a device left plugged in reaches that on a timer, not by chance.
  const uint32_t idle = now_ms - last_input_ms;

  // Track when the host's sleep signal ARRIVED, because the override below is
  // only owed to input that came after it.
  if (host_asleep && !was_host_asleep_) asleep_since_ = now_ms;
  was_host_asleep_ = host_asleep;

  if (host_asleep) {
    // Input at-or-after the transition, wrap-safe (same convention as the rest
    // of this file: unsigned subtraction, top half of the range means "before").
    const bool input_after =
        (last_input_ms - asleep_since_) < (UINT32_MAX / 2);
    // Off immediately unless a person has touched the device SINCE the lock -
    // that person is the safety valve against a wrong or stale signal, which
    // twice during development was the only way to get the screen back. Input
    // from before the lock earns nothing; and even the valve lapses, so a real
    // lock always wins in the end.
    if (!input_after || idle >= INPUT_OVERRIDE_MS) {
      state_ = ScreenState::Off;
      just_woke_ = false;
      return;
    }
  }

  if (playing || idle < DIM_AFTER_MS) {
    // Playback counts as something happening: this screen's whole job is to
    // show it, so it stays bright however long since anyone touched it.
    state_ = ScreenState::Bright;
  } else if (idle < OFF_AFTER_MS) {
    state_ = ScreenState::Dim;
  } else {
    state_ = ScreenState::Off;
  }

  just_woke_ = was != ScreenState::Bright && state_ == ScreenState::Bright;
}

}  // namespace core
