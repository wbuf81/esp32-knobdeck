#include "DaisyIdle.h"

#include "gfx/Color.h"
#include "gfx/Geometry.h"

namespace views {
namespace {

// Where the sprite's top-left cell lands on screen.
//
// Centred horizontally; pushed BELOW centre vertically, because the sleeping
// pose is bottom-heavy - the dog lies along the ground and the zzz rise above
// her - so a geometrically centred sprite reads as sitting too high on a round
// screen.
constexpr int SPRITE_W = daisy::SPRITE_COLS * DaisyIdle::SCALE;
constexpr int SPRITE_H = daisy::SPRITE_ROWS * DaisyIdle::SCALE;
constexpr int LEFT = gfx::CX - SPRITE_W / 2;
constexpr int TOP = gfx::CY - SPRITE_H / 2 + 14;

// The mood for each kind of input, and how long she holds it.
//
// The hold is not "play once through". Zoomies is four frames at 100 ms - 0.4 s,
// which is shorter than the haptic click that triggered it and reads as a
// flicker rather than as a dog. Cyclic animations LOOP until the hold expires;
// alert is the one that is already slow enough for a single pass to land.
daisy::DaisyAnim animFor(DaisyIdle::Reaction r) {
  switch (r) {
    case DaisyIdle::Reaction::Touch: return daisy::Daisy_Alert;
    case DaisyIdle::Reaction::Swipe: return daisy::Daisy_Wag;
    case DaisyIdle::Reaction::Hold: return daisy::Daisy_Zoomies;
    case DaisyIdle::Reaction::Turn: return daisy::Daisy_Sniff;
  }
  return daisy::Daisy_Alert;
}

float holdFor(DaisyIdle::Reaction r) {
  switch (r) {
    case DaisyIdle::Reaction::Touch: return 1.4f;   // one pass of alert
    case DaisyIdle::Reaction::Swipe: return 1.8f;
    case DaisyIdle::Reaction::Hold: return 2.5f;    // earn the belly rub
    case DaisyIdle::Reaction::Turn: return 1.8f;
  }
  return 1.4f;
}

}  // namespace

void DaisyIdle::begin() {
  clock_ = 0.0f;
  anim_clock_ = 0.0f;
  hold_s_ = 0.0f;
  mode_ = Mode::Sleeping;
  anim_ = daisy::Daisy_Sleep;
  frame_ = 0;
}

void DaisyIdle::react(Reaction r) {
  // Whatever she was doing, she is not about to yawn: the yawn timer was
  // possibly most of the way to firing, and a yawn two seconds after a poke
  // looks like the poke broke something.
  clock_ = 0.0f;

  const daisy::DaisyAnim want = animFor(r);
  if (mode_ == Mode::Reacting && anim_ == want) {
    // Same poke again. Push the expiry out from where the animation actually
    // is, leaving anim_clock_ and the current frame alone.
    hold_s_ = anim_clock_ + holdFor(r);
    return;
  }
  anim_ = want;
  anim_clock_ = 0.0f;
  hold_s_ = holdFor(r);
  mode_ = Mode::Reacting;
  frame_ = 0;
}

void DaisyIdle::update(float dt) {
  clock_ += dt;
  anim_clock_ += dt;

  const daisy::AnimData &a = daisy::ANIMS[anim_];
  const float frame_s = static_cast<float>(a.frame_ms) * 0.001f;
  const int total = static_cast<int>(anim_clock_ / frame_s);
  frame_ = a.frame_count > 0 ? total % a.frame_count : 0;

  // All three modes are driven off the same accumulated clocks rather than off
  // timers, so the whole view stays a pure function of the dt and poke
  // sequences.
  switch (mode_) {
    case Mode::Reacting:
      if (anim_clock_ >= hold_s_) {
        anim_clock_ = 0.0f;
        anim_ = daisy::Daisy_Drowsy;
        frame_ = 0;
        mode_ = Mode::Settling;
      }
      break;

    case Mode::Settling:
      if (total >= a.frame_count) {
        // Drowsy has played once through. Down she goes, and the yawn timer
        // starts from a full interval rather than from wherever it was.
        clock_ = 0.0f;
        anim_clock_ = 0.0f;
        anim_ = daisy::Daisy_Sleep;
        frame_ = 0;
        mode_ = Mode::Sleeping;
      }
      break;

    case Mode::Sleeping:
      // A yawn interrupts the sleep loop, then hands back.
      if (anim_ == daisy::Daisy_Sleep) {
        if (clock_ >= YAWN_EVERY_S) {
          clock_ = 0.0f;
          anim_clock_ = 0.0f;
          anim_ = daisy::Daisy_Yawn;
          frame_ = 0;
        }
      } else if (total >= a.frame_count) {
        anim_clock_ = 0.0f;
        anim_ = daisy::Daisy_Sleep;
        frame_ = 0;
      }
      break;
  }
}

uint16_t DaisyIdle::tint() const { return gfx::rgb565(70, 96, 150); }

void DaisyIdle::renderBand(gfx::Surface &s) {
  const daisy::AnimData &a = daisy::ANIMS[anim_];
  const uint8_t *cells = a.frames[frame_];

  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);

    // The ground: one write per pixel, no read. This replaces the radial
    // backdrop rather than drawing over it, so the idle view costs less than the
    // player view rather than more - which is the right way round for a screen
    // nobody is watching.
    const int dy = y - gfx::CY;
    const int v = 10 + (dy > 0 ? dy : -dy) / 18;
    const uint16_t ground = gfx::rgb565(static_cast<uint8_t>(v),
                                        static_cast<uint8_t>(v),
                                        static_cast<uint8_t>(v + 6));
    for (int x = 0; x < gfx::W; ++x) row[x] = ground;

    // Which sprite row this screen row samples. Rows outside the sprite are
    // ground only.
    const int sy = (y - TOP) / SCALE;
    if (y < TOP || sy < 0 || sy >= daisy::SPRITE_ROWS) continue;
    const uint8_t *cell_row = cells + sy * daisy::SPRITE_COLS;

    for (int cx = 0; cx < daisy::SPRITE_COLS; ++cx) {
      const uint8_t idx = cell_row[cx];
      if (idx == 0) continue;  // index 0 is the transparent key
      if (idx >= a.palette_count) continue;
      const uint16_t col = a.palette[idx];
      int x0 = LEFT + cx * SCALE;
      int x1 = x0 + SCALE;
      // A span entirely off-screen is DISCARDED; a straddling one has its
      // endpoints trimmed. Trimming a run's ends is not the clamping mistake
      // that cost this project three clipping bugs - that was folding an
      // off-screen coordinate onto the boundary, which would pile this cell up
      // against the edge instead of dropping it.
      if (x1 <= 0 || x0 >= gfx::W) continue;
      if (x0 < 0) x0 = 0;
      if (x1 > gfx::W) x1 = gfx::W;
      for (int x = x0; x < x1; ++x) row[x] = col;
    }
  }
}

}  // namespace views
