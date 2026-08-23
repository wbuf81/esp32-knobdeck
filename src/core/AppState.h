#pragma once

// Single source of truth. Sources write it, screens read it.

#include <cstdint>

#include "Deadline.h"
#include "PlaybackState.h"

enum class LinkStatus : uint8_t {
  Booting,
  Connecting,
  Online,
  Offline,
  AuthError,
  ReauthNeeded,
};

struct AppState {
  PlaybackState pb;
  LinkStatus link = LinkStatus::Booting;

  // Battery, sampled on the UI thread rather than published by a source: it is
  // a property of the board, not of Spotify. -1 means no reading.
  //
  // There is deliberately no `charging` flag. The IP5306's charging bit just
  // mirrors its charger-enable bit and reads true forever, so any behaviour
  // built on it is built on noise — the field existing at all is how the
  // battery badge once ended up permanently hidden.
  int8_t battery_pct = -1;

  // Bumped every time a source publishes fresh data. The UI extrapolates
  // progress between publishes and resyncs only when this changes; without it
  // there is no way to distinguish a new poll from re-reading the same state.
  uint32_t publish_seq = 0;

  // Transient message shown in the bottom strip in place of the time row.
  char toast[64] = {};
  Deadline toast_for;

  // Fields recently changed optimistically must not be overwritten by a poll
  // response already in flight. See the settle window in the design spec.
  Deadline settle_playing;
  Deadline settle_volume;
  Deadline settle_liked;

  void showToast(const char *msg, uint32_t now_ms, uint32_t duration_ms = 2000) {
    setStr(toast, sizeof(toast), msg);
    toast_for.arm(now_ms, duration_ms);
  }

  bool toastActive(uint32_t now_ms) const {
    return toast[0] != '\0' && toast_for.pending(now_ms);
  }
};
