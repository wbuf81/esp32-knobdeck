#include "ThemePicker.h"

namespace fx {

const char *ThemePicker::rowName(int i) {
  if (i == 0) return "Shuffle";
  const int t = i - 1;
  if (t < 0 || t >= static_cast<int>(ThemeId::Count)) return "?";
  return themeName(static_cast<ThemeId>(t));
}

ThemeId ThemePicker::rowTheme(int i, ThemeId fallback) {
  const int t = i - 1;
  if (i <= 0 || t >= static_cast<int>(ThemeId::Count)) return fallback;
  return static_cast<ThemeId>(t);
}

void ThemePicker::chooseRow(int row, core::Rng &rng) {
  if (row <= 0) {
    shuffle_ = true;
    // Roll immediately, so picking Shuffle does something you can see rather
    // than nothing until the track happens to change.
    onTrackChange(rng);
    return;
  }
  const int t = row - 1;
  if (t < static_cast<int>(ThemeId::Count)) lock(static_cast<ThemeId>(t));
}

int ThemePicker::currentRow() const {
  if (shuffle_) return 0;
  return 1 + static_cast<int>(current_);
}

void ThemePicker::onTrackChange(core::Rng &rng) {
  if (!shuffle_) return;
  const int n = static_cast<int>(ThemeId::Count);
  if (n <= 1) return;
  // Pick among the OTHERS and map around the current one. This cannot repeat
  // and needs no retry loop, where "roll until different" would have an
  // unbounded worst case on a small set.
  const int cur = static_cast<int>(current_);
  const int pick = static_cast<int>(rng.next() % static_cast<uint32_t>(n - 1));
  current_ = static_cast<ThemeId>(pick >= cur ? pick + 1 : pick);
}

uint32_t ThemePicker::toStored() const {
  return shuffle_ ? 0u : 1u + static_cast<uint32_t>(current_);
}

void ThemePicker::fromStored(uint32_t v) {
  if (v == 0u) {
    shuffle_ = true;
    return;
  }
  const uint32_t t = v - 1u;
  // A stored value from a build with more themes than this one must not index
  // off the end of the enum.
  if (t >= static_cast<uint32_t>(ThemeId::Count)) {
    shuffle_ = true;
    return;
  }
  shuffle_ = false;
  current_ = static_cast<ThemeId>(t);
}

}  // namespace fx
