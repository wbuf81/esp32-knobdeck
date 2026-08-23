#pragma once

// WiFi association, with reconnect and backoff.
//
// Non-blocking: called from the net task each iteration, never waits for the
// radio. The UI must keep rendering and the buttons must keep responding while
// the link is down, which is the whole reason the status screen exists.

#include <cstdint>

#include "../core/AppState.h"
#include "../core/Deadline.h"

class WifiLink {
 public:
  void begin(const char *ssid, const char *password);

  // Drives the connection state machine. Returns true when the link is usable.
  bool ensureConnected(uint32_t now_ms);

  LinkStatus status() const { return status_; }

 private:
  const char *ssid_ = nullptr;
  const char *password_ = nullptr;

  bool attempting_ = false;
  Deadline attempt_;
  Deadline retry_;
  uint32_t backoff_ms_ = 0;
  LinkStatus status_ = LinkStatus::Booting;
};

// Attempt timeout, and the backoff schedule between attempts. Capped so a
// device left running through a router reboot reconnects within half a minute
// rather than backing off to hours.
constexpr uint32_t WIFI_ATTEMPT_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_BACKOFF_START_MS = 1000;
constexpr uint32_t WIFI_BACKOFF_MAX_MS = 30000;
