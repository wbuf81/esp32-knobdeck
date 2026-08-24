#pragma once

// The one UI preference that has to outlive a reboot.
//
// Deliberately NOT a field on DeviceConfig. That struct holds credentials, and
// its save() writes the whole set - so routing a theme change through it would
// mean the UI rewriting the Spotify refresh token every time you picked a row.
// Its own namespace also means a corrupt or absent UI preference can never
// affect whether the device can log in.

#include <cstdint>

namespace config {

// Returns `fallback` when nothing has been stored yet.
uint32_t loadTheme(uint32_t fallback);
void saveTheme(uint32_t v);

}  // namespace config
