#include "SafeScreen.h"

#include <cstdio>
#include <cstring>

#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"
#include "shell/RadialShell.h"

namespace views {
namespace {

// Dark amber. Not red: red is what this project uses for nothing at all, and a
// full red screen reads as "dead" when the actual message is "alive, running,
// and deliberately doing less".
constexpr uint8_t BG_R = 26, BG_G = 14, BG_B = 4;

// Baselines, top to bottom. Spread down the middle of the disc rather than
// centred as a block, because the chord is narrowest at the top and bottom and
// the longest lines need to sit near the middle.
constexpr int Y_TITLE = 128;
constexpr int Y_REASON = 168;
constexpr int Y_STREAK = 194;
constexpr int Y_NOTE1 = 232;
constexpr int Y_NOTE2 = 254;

}  // namespace

void SafeScreen::begin(const char *reset_reason, int streak) {
  // Copied, not pointed at. The caller's string is a static in the ESP-IDF
  // reset-reason table today, but a view that outlives its argument is a bug
  // waiting for the day that changes.
  const char *r = (reset_reason && reset_reason[0]) ? reset_reason : "unknown";
  std::snprintf(reason_, sizeof(reason_), "%s", r);
  std::snprintf(streak_, sizeof(streak_), "%d crashes in a row", streak);
}

void SafeScreen::renderBand(gfx::Surface &s) {
  const uint16_t bg = gfx::rgb565(BG_R, BG_G, BG_B);
  const uint16_t amber = gfx::rgb565(255, 176, 32);
  const uint16_t white = gfx::rgb565(255, 255, 255);
  const uint16_t dim = gfx::rgb565(150, 120, 80);

  // One write per pixel, no read. Same rule as the dog: the backdrop replaces
  // whatever was there rather than being drawn over.
  for (int y = s.y0; y < s.yEnd(); ++y) {
    uint16_t *row = s.row(y);
    for (int x = 0; x < gfx::W; ++x) row[x] = bg;
  }

  // A ring just inside the bezel, so the state is readable across the room
  // before any text is. shell::drawArc rather than a trig loop of this file's
  // own: it already solves the angular range that crosses this band, which
  // turns eighteen full-circle walks per frame into about two, and it already
  // gets the clipping right at band boundaries.
  shell::drawArc(s, shell::RadialShell::PROGRESS_R, 4, 0.0f, 1.0f, amber);

  gfx::drawTextCentered(s, gfx::fontTitle(), gfx::CX, Y_TITLE, "SAFE MODE",
                        amber);
  gfx::drawTextCentered(s, gfx::fontArtist(), gfx::CX, Y_REASON, reason_,
                        white);
  gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX, Y_STREAK, streak_, dim);
  gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX, Y_NOTE1,
                        "network + effects off", dim);
  gfx::drawTextCentered(s, gfx::fontSmall(), gfx::CX, Y_NOTE2,
                        "clears itself after 30s", dim);
}

}  // namespace views
