#pragma once

// Where the device's credentials actually come from.
//
// Resolution order: NVS (written by the setup portal) wins field by field,
// then the compiled-in secrets.h if the build has one. This means a developer
// device keeps working with no setup step, while a gifted device — flashed
// from a build with NO secrets.h at all — configures itself entirely through
// the portal and never needs PlatformIO again.
//
// Values are never logged. Saving writes only the fields the form filled in,
// so re-running the portal to change WiFi does not wipe the Spotify token.

#include <cstdint>
#include <string>

struct DeviceConfig {
  std::string wifi_ssid;
  std::string wifi_password;
  std::string client_id;
  std::string client_secret;
  std::string refresh_token;

  // Which views rotate: bit i enables full-screen mode i, bit 7 enables the
  // classic view. Set from the portal's checkboxes. All-on by default; an
  // empty mask falls back to classic-only rather than a blank device.
  uint32_t views_mask = 0xFF;

  bool complete() const {
    return !wifi_ssid.empty() && !client_id.empty() &&
           !client_secret.empty() && !refresh_token.empty();
  }

  // NVS first, compiled secrets as the fallback for any missing field.
  static DeviceConfig load();

  // Persists non-empty fields to NVS. Empty fields keep their stored value.
  static bool save(const DeviceConfig &c);
};
