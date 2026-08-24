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

  if (host_asleep) {
    // No idle timer involved: the machine next to it is asleep, so waiting
    // another four minutes to agree would be silly.
    state_ = ScreenState::Off;
    just_woke_ = false;
    return;
  }

  // Unsigned subtraction, so this is correct across the 49.7-day millis wrap -
  // a device left plugged in reaches that on a timer, not by chance.
  const uint32_t idle = now_ms - last_input_ms;

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
