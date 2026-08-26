#include "TeamsScreen.h"

#include <cstdio>

#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"

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
  if (camera < 0) return {70, 70, 84, false, false};
  if (camera == 1) return {30, 44, 88, false, true};  // on-air blue
  return {30, 32, 40, true, false};                   // off: near-dark
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

void TeamsScreen::begin() {
  muted_ = -1;
  camera_ = -1;
  mic_pending_ = false;
  cam_pending_ = false;
  timer_[0] = '\0';
}

void TeamsScreen::prepare(int muted, int camera, bool mic_pending,
                          bool cam_pending, int call_s) {
  muted_ = muted;
  camera_ = camera;
  mic_pending_ = mic_pending;
  cam_pending_ = cam_pending;
  if (call_s >= 0) {
    std::snprintf(timer_, sizeof(timer_), "%d:%02d", call_s / 60, call_s % 60);
  } else {
    timer_[0] = '\0';
  }
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
}

}  // namespace views
