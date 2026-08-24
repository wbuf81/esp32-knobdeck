#pragma once

// Which theme is showing, and whether that is a choice or a roll.
//
// Split out from the view because it is pure decision logic with no pixels in
// it, which means it can be tested on the desktop - and "shuffle actually gives
// you a different theme" is exactly the kind of claim that is easy to get
// subtly wrong and impossible to see by looking.

#include <cstdint>

#include "core/Rng.h"
#include "fx/Themes.h"

namespace fx {

class ThemePicker {
 public:
  // Row 0 of the THEMES screen is Shuffle; the themes follow it.
  static constexpr int ROWS = 1 + static_cast<int>(ThemeId::Count);

  // What the list shows at row i.
  static const char *rowName(int i);

  void setShuffle() { shuffle_ = true; }
  void lock(ThemeId id) {
    shuffle_ = false;
    current_ = id;
  }
  // Row from the list, straight through: 0 is shuffle, the rest are themes.
  void chooseRow(int row, core::Rng &rng);
  // Which row should render as "you are here".
  int currentRow() const;

  bool shuffle() const { return shuffle_; }
  ThemeId current() const { return current_; }

  // Called when the track changes. Rolls a NEW theme when shuffling - never the
  // one already showing, because a shuffle that repeats reads as broken rather
  // than as random.
  void onTrackChange(core::Rng &rng);

  // Round-trips through one stored integer: 0 is shuffle, n is theme n-1.
  uint32_t toStored() const;
  void fromStored(uint32_t v);

 private:
  ThemeId current_ = ThemeId::CoverLight;
  bool shuffle_ = false;
};

}  // namespace fx
