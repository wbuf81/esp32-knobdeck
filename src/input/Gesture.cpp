#include "Gesture.h"

namespace input {

const char *gestureName(Gesture g) {
  switch (g) {
    case Gesture::None: return "none";
    case Gesture::Tap: return "tap";
    case Gesture::SwipeLeft: return "swipe-left";
    case Gesture::SwipeRight: return "swipe-right";
    case Gesture::SwipeUp: return "swipe-up";
    case Gesture::SwipeDown: return "swipe-down";
    case Gesture::LongPress: return "long-press";
  }
  return "?";
}

Gesture GestureRecognizer::update(bool touching, int x, int y,
                                  uint32_t now_ms) {
  if (touching && !down_) {
    down_ = true;
    long_fired_ = false;
    x0_ = x;
    y0_ = y;
    max_dx_ = 0;
    max_dy_ = 0;
    down_at_ = now_ms;
    return Gesture::None;
  }

  if (touching && down_) {
    const int dx = x - x0_;
    const int dy = y - y0_;
    // Track the FURTHEST the finger got, not where it is now. A swipe that
    // curls back before release is still a swipe, and reading only the release
    // point loses it.
    if ((dx < 0 ? -dx : dx) > (max_dx_ < 0 ? -max_dx_ : max_dx_)) max_dx_ = dx;
    if ((dy < 0 ? -dy : dy) > (max_dy_ < 0 ? -max_dy_ : max_dy_)) max_dy_ = dy;

    const int adx = max_dx_ < 0 ? -max_dx_ : max_dx_;
    const int ady = max_dy_ < 0 ? -max_dy_ : max_dy_;
    const bool moved = adx >= SWIPE_MIN_PX || ady >= SWIPE_MIN_PX;

    // A long press only counts if the finger stayed put. Holding at the end of
    // a drag is not a long press, and treating it as one means every slow swipe
    // also likes the track.
    if (!long_fired_ && !moved && (now_ms - down_at_) >= LONG_MS) {
      long_fired_ = true;
      return Gesture::LongPress;
    }
    return Gesture::None;
  }

  if (!touching && down_) {
    down_ = false;
    // A long press already reported. Releasing must NOT also emit a tap, or
    // every like would additionally toggle playback.
    if (long_fired_) return Gesture::None;

    const int adx = max_dx_ < 0 ? -max_dx_ : max_dx_;
    const int ady = max_dy_ < 0 ? -max_dy_ : max_dy_;
    if (adx >= SWIPE_MIN_PX || ady >= SWIPE_MIN_PX) {
      // Dominant axis wins, so a diagonal resolves rather than doing both.
      if (adx >= ady) return max_dx_ > 0 ? Gesture::SwipeRight : Gesture::SwipeLeft;
      return max_dy_ > 0 ? Gesture::SwipeDown : Gesture::SwipeUp;
    }
    if ((now_ms - down_at_) <= TAP_MAX_MS) return Gesture::Tap;
    // Held too long to be a tap but not still enough to be a long press: a
    // slow smeared touch. Reporting nothing is better than guessing.
    return Gesture::None;
  }

  return Gesture::None;
}

}  // namespace input
