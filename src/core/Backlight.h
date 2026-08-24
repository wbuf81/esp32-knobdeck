#pragma once

// When the screen is bright, dim, or off.
//
// Deliberately a small state machine with its inputs passed in, rather than
// something that reads a clock and the playback state itself: it is then
// host-testable, and the rules are visible in one place instead of scattered
// through the render loop.
//
// The rule that matters: while something is PLAYING the screen stays bright, no
// matter how long since anyone touched it. This is a now-playing display, and
// dimming what it exists to show would be a bug dressed as a power saving.

#include <cstdint>

namespace core {

enum class ScreenState : uint8_t { Bright, Dim, Off };

class Backlight {
 public:
  static constexpr uint8_t BRIGHT = 210;
  static constexpr uint8_t DIM = 34;
  static constexpr uint8_t OFF = 0;

  // Idle only starts counting once playback has stopped.
  static constexpr uint32_t DIM_AFTER_MS = 45000;
  static constexpr uint32_t OFF_AFTER_MS = 240000;

  // Called every frame.
  //
  // Takes the TIMESTAMP of the last input rather than a "was there input just
  // now" flag. With a flag the idle clock started on the first frame that saw
  // no input, so any gap in calls silently restarted it - the timer measured
  // "how long since we noticed nothing happening" instead of "how long since
  // something happened". The timestamp cannot drift that way.
  //
  // `host_asleep` overrides everything: if the computer this sits next to is
  // asleep or locked, so is this.
  void update(uint32_t now_ms, bool playing, uint32_t last_input_ms,
              bool host_asleep);

  ScreenState state() const { return state_; }
  uint8_t duty() const;

  // True on the frame the screen came back. The caller swallows the input that
  // caused it, the way a phone does - otherwise the tap that wakes the device
  // also pauses the music.
  bool justWoke() const { return just_woke_; }

 private:
  ScreenState state_ = ScreenState::Bright;
  bool just_woke_ = false;
};

}  // namespace core
