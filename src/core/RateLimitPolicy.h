#pragma once

// How long to wait before the first poll after a boot that follows a 429.
//
// This exists because of a spiral I caused. `rate_limited_` is a Deadline in
// RAM, so every flash cleared it and the device polled Spotify the instant it
// came up. Twenty reflashes in one afternoon meant twenty immediate requests
// into an already-exhausted quota, each earning a fresh 429 and very plausibly
// extending the penalty - and none of it was visible, because the 429 was not
// logged and its only signal was a toast that rendered nowhere.
//
// The stored value is a SIGNAL TO PROBE SLOWLY, not a duration to serve. There
// is no wall clock on this board - no NTP anywhere in the project - so the
// device cannot know how long it was powered off. Honouring a remembered 39
// minutes could leave it idle long after Spotify had forgiven it, which is its
// own kind of broken.
//
// So: above the cap, wait one cap and then ask. One probe a minute instead of
// one poll every two seconds is a 30x reduction in strikes while the limit
// stands, and Spotify's own answer stays the authority on whether it still
// does. Below the cap, the remembered wait is both trustworthy and cheap, so
// it is served in full.

#include <cstdint>

namespace core {

// One probe a minute while a remembered limit might still stand.
constexpr uint32_t BOOT_PROBE_CAP_MS = 60000;

// `stored_wait_s` is the last Retry-After the device was told, as read back
// from NVS. Zero means it was not limited. Negative means the read is garbage -
// a damaged partition can return anything, and a negative must never become an
// enormous unsigned wait.
inline uint32_t bootRateLimitWaitMs(int32_t stored_wait_s) {
  if (stored_wait_s <= 0) return 0;
  const uint64_t ms = static_cast<uint64_t>(stored_wait_s) * 1000u;
  return ms > BOOT_PROBE_CAP_MS ? BOOT_PROBE_CAP_MS
                                : static_cast<uint32_t>(ms);
}

}  // namespace core
