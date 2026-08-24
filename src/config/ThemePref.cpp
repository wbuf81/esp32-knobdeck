#include "ThemePref.h"

#include <Preferences.h>

namespace config {
namespace {
constexpr const char *NS = "knob-ui";
constexpr const char *KEY = "theme";
}  // namespace

uint32_t loadTheme(uint32_t fallback) {
  Preferences p;
  // Read-only open fails when the namespace has never been written, which is
  // the normal first-boot path rather than an error.
  if (!p.begin(NS, /*readOnly=*/true)) return fallback;
  const uint32_t v = p.getUInt(KEY, fallback);
  p.end();
  return v;
}

void saveTheme(uint32_t v) {
  Preferences p;
  if (!p.begin(NS, /*readOnly=*/false)) return;
  p.putUInt(KEY, v);
  p.end();
}

}  // namespace config
