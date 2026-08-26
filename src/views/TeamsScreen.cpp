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

// Half backgrounds. The LIVE mic is the loud one - dark red ground so the
// glance reads before any glyph does. Muted is calm. Unknown is grey.
inline uint16_t liveBg() { return gfx::rgb565(96, 18, 18); }
inline uint16_t calmBg() { return gfx::rgb565(16, 30, 20); }
inline uint16_t offBg() { return gfx::rgb565(22, 22, 26); }
inline uint16_t unknownBg() { return gfx::rgb565(34, 34, 34); }

uint16_t micBg(int muted) {
  if (muted < 0) return unknownBg();
  return muted ? calmBg() : liveBg();
}
uint16_t camBg(int camera) {
  if (camera < 0) return unknownBg();
  return camera ? liveBg() : offBg();
}

// Pending dims the half: same hue, half brightness, so "waiting for Teams"
// cannot be confused with either settled state.
uint16_t dim(uint16_t c) {
  const uint32_t rb = c & 0xF81F;
  const uint32_t g = c & 0x07E0;
  return static_cast<uint16_t>(((rb >> 1) & 0xF81F) | ((g >> 1) & 0x07E0));
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
  uint16_t mic = micBg(muted_);
  uint16_t cam = camBg(camera_);
  if (mic_pending_) mic = dim(mic);
  if (cam_pending_) cam = dim(cam);

  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);
    for (int x = 0; x < SPLIT_X; ++x) row[x] = mic;
    for (int x = SPLIT_X; x < gfx::W; ++x) row[x] = cam;
    // The seam, so the two buttons read as two buttons.
    row[SPLIT_X - 1] = gfx::rgb565(0, 0, 0);
    row[SPLIT_X] = gfx::rgb565(0, 0, 0);
  }

  const uint16_t ink = gfx::rgb565(255, 255, 255);
  const uint16_t soft = gfx::rgb565(200, 200, 200);

  // Labels carry the meaning; the glyphs are reinforcement. Text is what a
  // glance trusts.
  const char *mic_label = muted_ < 0 ? "?" : (muted_ ? "MUTED" : "LIVE");
  const char *cam_label = camera_ < 0 ? "?" : (camera_ ? "CAM ON" : "cam off");
  gfx::drawTextCentered(s, gfx::fontTitle(), gfx::CX / 2, 200,
                        mic_pending_ ? "..." : mic_label, ink);
  gfx::drawTextCentered(s, gfx::fontTitle(), gfx::CX + gfx::CX / 2, 200,
                        cam_pending_ ? "..." : cam_label,
                        camera_ == 1 ? ink : soft);
  gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX / 2, 226, "mic", soft);
  gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX + gfx::CX / 2, 226,
                        "camera", soft);

  // A simple glyph per half: mic = capsule on a stand, camera = body + lens.
  // Procedural, like Matrix's glyph table - a font walk for two icons would be
  // the expensive route to a worse drawing.
  const int mcx = gfx::CX / 2, mcy = 130;
  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);
    const int dy = y - mcy;
    if (dy >= -28 && dy <= 4) {  // capsule
      for (int dx = -12; dx <= 12; ++dx) {
        if (dx * dx + (dy < -16 ? (dy + 16) * (dy + 16) : (dy > -8 ? (dy + 8) * (dy + 8) : 0)) <= 144) {
          row[mcx + dx] = ink;
        }
      }
    }
    if (dy > 4 && dy <= 18 && ((y - mcy) % 3)) {  // stand
      row[mcx - 1] = ink;
      row[mcx] = ink;
      row[mcx + 1] = ink;
    }
    const int ccx = gfx::CX + gfx::CX / 2, ccy = 130;
    const int cdy = y - ccy;
    if (cdy >= -16 && cdy <= 16) {  // camera body
      for (int dx = -22; dx <= 14; ++dx) row[ccx + dx] = ink;
      if (cdy >= -8 && cdy <= 8) {  // lens wedge
        for (int dx = 16; dx <= 24; ++dx) {
          if (dx - 16 <= 8 - (cdy < 0 ? -cdy : cdy)) row[ccx + dx] = ink;
        }
      }
    }
  }

  if (timer_[0]) {
    gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX, 312, timer_, soft);
  }
}

}  // namespace views
