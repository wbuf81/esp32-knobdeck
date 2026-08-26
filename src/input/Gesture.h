#pragma once

// Touch gestures, as a pure state machine over (touching, x, y, now_ms).
//
// Deliberately takes raw state and a timestamp rather than reading hardware, so
// it is exercised by host tests with synthetic traces instead of by poking a
// screen and squinting. The ancestor project's ButtonLogic had the same shape
// for the same reason, and its test suite is the reason a long press never also
// fired a tap.

#include <cstdint>

namespace input {

enum class Gesture : uint8_t {
  None,
  Tap,
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
  LongPress,
};

const char *gestureName(Gesture g);

class GestureRecognizer {
 public:
  // A swipe has to travel far enough that a slightly smeared tap is not one.
  // 40 px on a 360 px disc is about 11% of the width.
  static constexpr int SWIPE_MIN_PX = 40;
  // Longest a touch can last and still be a tap.
  static constexpr uint32_t TAP_MAX_MS = 400;
  // How long before a held touch becomes a long press.
  static constexpr uint32_t LONG_MS = 600;

  // Feed every frame. Returns at most one gesture per call.
  Gesture update(bool touching, int x, int y, uint32_t now_ms);

  // Where the CURRENT OR LAST touch began. This is the truth about what was
  // tapped: gestures fire on release, when touchRead already reads false and
  // the caller's coordinates have reset - the log's famous "tap at (0,0)".
  // Origin rather than endpoint, deliberately: a button panel asks which
  // target the gesture STARTED in, and a swipe that slides off a button still
  // belongs to the button it began on. Stable until the next touch-down.
  int tapX() const { return x0_; }
  int tapY() const { return y0_; }

 private:
  bool down_ = false;
  bool long_fired_ = false;
  int x0_ = 0, y0_ = 0;
  int max_dx_ = 0, max_dy_ = 0;
  uint32_t down_at_ = 0;
};

}  // namespace input
