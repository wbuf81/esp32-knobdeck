#include "Toast.h"

#include "NowPlaying.h"
#include "core/PlaybackState.h"  // setStr
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"

namespace shell {

void Toast::prepare(const char *msg, bool active) {
  visible_ = active && msg != nullptr && msg[0] != '\0';
  if (!visible_) {
    text_[0] = '\0';
    return;
  }
  setStr(text_, sizeof(text_), msg);
  // The usable width is the CHORD at this baseline, not the panel. Solved once
  // per frame here rather than once per band in render().
  max_w_ = gfx::halfChordAt(NowPlaying::TIME_BASELINE, NowPlaying::MARGIN) * 2;
}

void Toast::render(gfx::Surface &s, uint16_t tint) const {
  if (!visible_) return;
  // drawTextFit truncates with an ellipsis rather than overflowing, so a long
  // Spotify error stays inside the disc instead of running under the bezel.
  gfx::drawTextFit(s, gfx::fontArtist(), gfx::CX, NowPlaying::TIME_BASELINE,
                   text_, max_w_, tint);
}

}  // namespace shell
