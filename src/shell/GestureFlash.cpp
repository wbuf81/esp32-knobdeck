#include "GestureFlash.h"

#include "gfx/Geometry.h"

namespace shell {

bool transportFeedbackVisible(bool has_track, bool has_device) {
  return has_track || has_device;
}

void GestureFlash::show(Glyph g, uint32_t now_ms) {
  what_ = g;
  shown_ms_ = now_ms;
  armed_ = true;
}

bool GestureFlash::visible(uint32_t now_ms) const {
  if (!armed_) return false;
  return (now_ms - shown_ms_) <= HOLD_MS + FADE_MS;
}

void GestureFlash::prepare(uint32_t now_ms) {
  if (!armed_) {
    alpha_ = 0;
    return;
  }
  const uint32_t age = now_ms - shown_ms_;
  if (age <= HOLD_MS) {
    alpha_ = 256;
  } else if (age <= HOLD_MS + FADE_MS) {
    const uint32_t into = age - HOLD_MS;
    alpha_ = static_cast<uint16_t>(256u - (into * 256u) / FADE_MS);
  } else {
    alpha_ = 0;
  }
}

void GestureFlash::render(gfx::Surface &s, uint16_t tint) const {
  if (alpha_ == 0) return;
  // Near-white rather than the album tint: this is the device answering, not
  // part of the artwork, and it has to read against every cover.
  (void)tint;
  drawGlyph(s, what_, gfx::CX, gfx::CY, HALF, 0xFFFF, alpha_);
}

}  // namespace shell
