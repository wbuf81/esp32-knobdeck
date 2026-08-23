#pragma once

// A centre-weighted list, for a round screen driven by a knob.
//
// The selected row sits in the middle at full size and brightness, and its
// neighbours fall away above and below - dimmer, smaller, and narrower, because
// the chord of the disc genuinely narrows. That is the point: the list looks
// like a wheel under the knob rather than a scrolling page, and the geometry
// doing the work is the same geometry the hardware has.
//
// The scroll position is a FLOAT. Snapping the selection from row to row on each
// detent reads as a jump; gliding to it reads as a wheel with mass, and it is
// the difference between a knob that feels connected and one that feels like a
// set of buttons.
//
// prepare()/render() split for the same reason as NowPlaying: measuring and
// fitting eighteen times a frame, once per band, cost half the frame rate there.

#include <cstdint>

#include "gfx/Surface.h"

namespace shell {

class ListView {
 public:
  // Rows drawn either side of the centre. Two is what fits on a 360 disc once
  // the heading and the progress ring have taken their room.
  static constexpr int WING = 2;
  static constexpr int ROWS = WING * 2 + 1;
  static constexpr int ROW_SPACING = 33;
  static constexpr int CENTRE_BASELINE = 196;
  static constexpr int HEADING_BASELINE = 92;
  static constexpr int MARGIN = 26;

  // Once per frame. `pos` is the fractional selected index.
  // `note` replaces the list body when there is nothing to show - "empty" is
  // not the same message as "Spotify will not let an app read this one", and
  // showing the first for the second blames the wrong party.
  void prepare(const char *const *items, int count, float pos,
               const char *heading, bool truncated, const char *note = nullptr);
  // Once per band.
  void render(gfx::Surface &s, uint16_t tint) const;

 private:
  struct Row {
    char text[72] = {};
    int x = 0;
    int baseline = 0;
    int level = 0;  // 0 = centre, 1 = neighbour, 2 = outer
  };

  Row rows_[ROWS + 2];
  int row_count_ = 0;
  char heading_[44] = {};
  int heading_x_ = 0;
  char note_[44] = {};
  int note_x_ = 0;
  bool empty_ = false;
};

}  // namespace shell
