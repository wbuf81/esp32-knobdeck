#include "TeamsScreen.h"

#include <cstdio>

#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"
#include "shell/RadialShell.h"

namespace views {
namespace {

// The split. Left of this is the mic's button, right is the camera's.
constexpr int SPLIT_X = gfx::CX;

// 16x16 pixel-art icons, one uint16_t per row, MSB = leftmost pixel. Drawn
// scaled 6x as crisp blocks - the same species as Matrix's bit-table glyphs
// and Daisy's sprite cells, which is this device's native art style. The first
// version drew icons with per-row circle math and earned the review "the icons
// look horrible"; bit tables are designable, checkable, and cheap.
constexpr uint16_t MIC_ICON[16] = {
    0b0000011110000000,
    0b0000111111000000,
    0b0000110011000000,
    0b0000111111000000,
    0b0000110011000000,
    0b0000111111000000,
    0b0000111111000000,
    0b0110011110011000,
    0b0110000000011000,
    0b0011000000110000,
    0b0001111111100000,
    0b0000001100000000,
    0b0000001100000000,
    0b0000111111000000,
    0b0000000000000000,
    0b0000000000000000,
};

constexpr uint16_t CAM_ICON[16] = {
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0111111111000000,
    0b1111111111100000,
    0b1111111111110100,
    0b1111111111111100,
    0b1111111111111100,
    0b1111111111111100,
    0b1111111111110100,
    0b1111111111100000,
    0b0111111111000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
    0b0000000000000000,
};

constexpr int ICON_SCALE = 6;
constexpr int ICON_PX = 16 * ICON_SCALE;  // 96

// Half backgrounds as a vertical gradient: brighter at the icon's altitude,
// falling off above and below, so each button reads as a lit surface rather
// than a paint swatch. One integer lerp per row - per-frame cost is the same
// flat fill it replaces.
struct HalfStyle {
  uint8_t r, g, b;      // hue at the brightest row
  bool slash;           // state is OFF/MUTED: slash the icon
  bool bright_icon;     // white icon vs soft grey
};

HalfStyle micStyle(int muted) {
  if (muted < 0) return {70, 70, 84, false, false};   // unknown: cool grey
  if (muted == 1) return {24, 46, 30, true, false};   // muted: calm green-dark
  return {120, 26, 26, false, true};                  // LIVE: red, loud
}

HalfStyle camStyle(int camera) {
  // Same grammar as the mic, deliberately: on-air is red and loud, calm is
  // green and dark, one colour language across the whole glance.
  if (camera < 0) return {70, 70, 84, false, false};
  if (camera == 1) return {120, 26, 26, false, true};  // on-air: red, loud
  return {24, 46, 30, true, false};                    // off: calm green-dark
}

void fillHalfRow(uint16_t *row, int x0, int x1, int y, const HalfStyle &st,
                 bool pending) {
  // Peak brightness at the icon centreline, y=150.
  int dy = y - 150;
  if (dy < 0) dy = -dy;
  int f = 256 - dy * 5 / 4;  // fades to ~0 at the rim
  if (f < 64) f = 64;
  if (pending) f /= 2;  // pending dims the whole half: waiting for the echo
  const uint16_t c = gfx::rgb565(static_cast<uint8_t>(st.r * f / 256),
                                 static_cast<uint8_t>(st.g * f / 256),
                                 static_cast<uint8_t>(st.b * f / 256));
  for (int x = x0; x < x1; ++x) row[x] = c;
}

// One scaled icon row, plus the slash overlay where the state is off.
void drawIconRow(uint16_t *row, int y, int cx, const uint16_t *icon,
                 const HalfStyle &st) {
  const int top = 150 - ICON_PX / 2;
  const int iy = (y - top) / ICON_SCALE;
  if (iy < 0 || iy > 15) return;
  const int left = cx - ICON_PX / 2;
  const uint16_t ink = st.bright_icon ? gfx::rgb565(255, 255, 255)
                                      : gfx::rgb565(176, 176, 188);
  const uint16_t bits = icon[iy];
  for (int ix = 0; ix < 16; ++ix) {
    if (!(bits & (0x8000 >> ix))) continue;
    const int x0 = left + ix * ICON_SCALE;
    for (int x = x0; x < x0 + ICON_SCALE; ++x) row[x] = ink;
  }
  if (st.slash) {
    // A clean 45-degree bar, corner to corner across the icon box, 8px wide,
    // in the half's own alarm-adjacent tone so it reads as "off" not "error".
    const int rel = y - top;                  // 0..95 down the icon
    const int sx = left + ICON_PX - rel;      // right-to-left descent
    const uint16_t bar = gfx::rgb565(232, 90, 80);
    for (int x = sx - 4; x < sx + 4; ++x) {
      if (x >= left - 6 && x < left + ICON_PX + 6) row[x] = bar;
    }
  }
}

}  // namespace

// Icon centrelines: where bursts and swirls anchor.
constexpr float MIC_CX = static_cast<float>(gfx::CX) / 2.0f;
constexpr float CAM_CX = static_cast<float>(gfx::CX) + gfx::CX / 2.0f;
constexpr float ICON_CY = 150.0f;

// The call-length arc rides the same groove as the player's progress ring.
constexpr int ARC_R = 170;
constexpr int ARC_THICK = 3;

namespace {

// Burst palettes. Going on-air is loud; going calm is quiet - the particle
// grammar matches the colour grammar of the halves themselves.
fx::SpawnParams burstParams(int dir) {
  fx::SpawnParams p;
  p.spread = 12.0f;
  p.speed_min = 34.0f;
  p.speed_max = 90.0f;
  p.life_min = 0.7f;
  p.life_max = 1.8f;
  p.size_min = 2.2f;
  p.size_max = 4.6f;
  p.drag = 0.30f;
  // The camera speaks the mic's colour language: going on-air is a hot red
  // detonation whichever half it happens on, going calm is green.
  if (dir > 0) {
    p.colors[0] = gfx::rgb565(255, 80, 60);
    p.colors[1] = gfx::rgb565(255, 160, 60);
    p.colors[2] = gfx::rgb565(255, 230, 200);
    p.color_count = 3;
  } else {
    p.colors[0] = gfx::rgb565(70, 220, 110);
    p.colors[1] = gfx::rgb565(140, 255, 170);
    p.color_count = 2;
  }
  return p;
}

// The pending swirl: spawned on a ring around the icon, converging inward -
// the wait for Teams' echo drawn as the state being pulled together.
fx::SpawnParams swirlParams() {
  fx::SpawnParams p;
  p.spread = 6.0f;
  p.speed_min = 40.0f;
  p.speed_max = 80.0f;
  p.life_min = 0.6f;
  p.life_max = 1.1f;
  p.size_min = 2.0f;
  p.size_max = 3.2f;
  p.drag = 0.65f;
  p.colors[0] = gfx::rgb565(230, 230, 250);
  p.colors[1] = gfx::rgb565(150, 160, 210);
  p.color_count = 2;
  return p;
}

// The live-mic ambient field: slow embers that drift and rise off the mic
// half. Negative gravity does the rising; low speeds keep it a murmur.
fx::SpawnParams emberParams() {
  fx::SpawnParams p;
  p.spread = 4.0f;
  p.speed_min = 4.0f;
  p.speed_max = 18.0f;
  p.life_min = 1.4f;
  p.life_max = 2.8f;
  p.size_min = 1.8f;
  p.size_max = 3.4f;
  p.drag = 0.90f;
  p.gravity_y = -32.0f;
  p.colors[0] = gfx::rgb565(255, 110, 60);
  p.colors[1] = gfx::rgb565(255, 180, 80);
  p.colors[2] = gfx::rgb565(230, 70, 40);
  p.color_count = 3;
  return p;
}

}  // namespace

void TeamsScreen::begin() {
  muted_ = -1;
  camera_ = -1;
  mic_pending_ = false;
  cam_pending_ = false;
  call_s_ = -1;
  timer_[0] = '\0';
  mic_flip_ = 0;
  cam_flip_ = 0;
  ember_acc_ = 0.0f;
  swirl_acc_ = 0.0f;
  parts_.clear();
}

void TeamsScreen::prepare(int muted, int camera, bool mic_pending,
                          bool cam_pending, int call_s) {
  // Confirmed flips only: both old and new state must be known. A flip queues
  // until update() spends it, so a flip is never lost to frame ordering.
  if (muted_ >= 0 && muted >= 0 && muted != muted_) mic_flip_ = muted ? -1 : 1;
  if (camera_ >= 0 && camera >= 0 && camera != camera_)
    cam_flip_ = camera ? 1 : -1;
  muted_ = muted;
  camera_ = camera;
  mic_pending_ = mic_pending;
  cam_pending_ = cam_pending;
  call_s_ = call_s;
  if (call_s >= 0) {
    std::snprintf(timer_, sizeof(timer_), "%d:%02d", call_s / 60, call_s % 60);
  } else {
    timer_[0] = '\0';
  }
}

void TeamsScreen::update(float dt, core::Rng &rng) {
  if (mic_flip_) {
    parts_.configure(burstParams(mic_flip_));
    parts_.setOrigin(MIC_CX, ICON_CY);
    parts_.burst(mic_flip_ > 0 ? 120 : 60, mic_flip_ > 0 ? 1.2f : 0.7f, rng);
    mic_flip_ = 0;
  }
  if (cam_flip_) {
    parts_.configure(burstParams(cam_flip_));
    parts_.setOrigin(CAM_CX, ICON_CY);
    parts_.burst(cam_flip_ > 0 ? 100 : 50, cam_flip_ > 0 ? 1.1f : 0.65f, rng);
    cam_flip_ = 0;
  }
  if (muted_ == 0) {
    // Emission is metered in ember-per-second, accumulated against dt, so the
    // field's density does not depend on the frame rate.
    ember_acc_ += dt * 60.0f;
    while (ember_acc_ >= 1.0f) {
      ember_acc_ -= 1.0f;
      parts_.configure(emberParams());
      // Anywhere across the mic half's lower body, one ember at a time.
      parts_.setOrigin(rng.range(16.0f, static_cast<float>(gfx::CX) - 16.0f),
                       rng.range(220.0f, 300.0f));
      parts_.emit(1, rng);
    }
  } else {
    ember_acc_ = 0.0f;
  }
  if (mic_pending_ || cam_pending_) {
    swirl_acc_ += dt * 55.0f;
    while (swirl_acc_ >= 1.0f) {
      swirl_acc_ -= 1.0f;
      parts_.configure(swirlParams());
      if (mic_pending_) {
        parts_.setOrigin(MIC_CX, ICON_CY);
        parts_.implode(1, 64.0f, rng);
      }
      if (cam_pending_) {
        parts_.setOrigin(CAM_CX, ICON_CY);
        parts_.implode(1, 64.0f, rng);
      }
    }
  } else {
    swirl_acc_ = 0.0f;
  }
  parts_.update(dt);
}

void TeamsScreen::renderBand(gfx::Surface &s) {
  const HalfStyle mic = micStyle(muted_);
  const HalfStyle cam = camStyle(camera_);

  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);
    fillHalfRow(row, 0, SPLIT_X - 2, y, mic, mic_pending_);
    fillHalfRow(row, SPLIT_X + 2, gfx::W, y, cam, cam_pending_);
    // The seam: a true gap, so two buttons read as two buttons.
    row[SPLIT_X - 2] = 0;
    row[SPLIT_X - 1] = 0;
    row[SPLIT_X] = 0;
    row[SPLIT_X + 1] = 0;

    drawIconRow(row, y, gfx::CX / 2, MIC_ICON, mic);
    drawIconRow(row, y, gfx::CX + gfx::CX / 2, CAM_ICON, cam);
  }

  const uint16_t ink = gfx::rgb565(255, 255, 255);
  const uint16_t soft = gfx::rgb565(170, 170, 180);

  const char *mic_label =
      mic_pending_ ? "..." : (muted_ < 0 ? "MIC" : (muted_ ? "MUTED" : "LIVE"));
  const char *cam_label =
      cam_pending_ ? "..." : (camera_ < 0 ? "CAM" : (camera_ ? "ON" : "OFF"));
  gfx::drawTextCentered(s, gfx::fontTitle(), gfx::CX / 2, 236, mic_label,
                        muted_ == 0 ? ink : soft);
  gfx::drawTextCentered(s, gfx::fontTitle(), gfx::CX + gfx::CX / 2, 236,
                        cam_label, camera_ == 1 ? ink : soft);
  // The tap hint earns its pixels only while the state is unknown - once real
  // state flows, the label is the message.
  if (muted_ < 0) {
    gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX / 2, 258, "tap toggles",
                          soft);
  }
  if (timer_[0]) {
    gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX, 312, timer_, soft);
  }

  // The call-length arc: one lap of the rim is an hour. The colour names the
  // lap - white for the first hour, amber for the second, red beyond - and a
  // finished lap stays behind the running one as a full ring, so hour two
  // reads as "a whole white hour, plus this much amber".
  if (call_s_ > 0) {
    const int lap = call_s_ / 3600;
    const float frac = static_cast<float>(call_s_ % 3600) / 3600.0f;
    const uint16_t lap_col[3] = {gfx::rgb565(140, 140, 155),
                                 gfx::rgb565(225, 150, 40),
                                 gfx::rgb565(235, 70, 55)};
    const uint16_t col = lap_col[lap > 2 ? 2 : lap];
    if (lap > 0) {
      const uint16_t prev = lap_col[lap - 1 > 2 ? 2 : lap - 1];
      shell::drawArc(s, ARC_R, ARC_THICK, 0.0f, 1.0f, prev);
    }
    shell::drawArc(s, ARC_R, ARC_THICK, 0.0f, frac, col);
  }

  // Particles last, over everything: bursts, embers and the pending swirl are
  // light, and light sits on top.
  parts_.render(s);
}

}  // namespace views
