#pragma once

// When the device should stop trusting its own full boot path.
//
// Pure arithmetic over a stored count and an uptime, with no NVS and no reset
// reason in sight, for the same reason Backlight and HostLink are shaped this
// way: the rules are the part that is easy to get wrong, and they are worth
// more under test than under a flashing cable.
//
// The failure this exists for is documented in CLAUDE.md and cost a physical
// unplug during development: a crash loop takes the USB-CDC serial down with
// it, so there is no log to read and esptool cannot sync in any reset mode.
// Once the board is in that state nothing software can do will help - which
// means the decision to stop doing the thing that crashes has to be made on
// the boot BEFORE the one that strands it.

#include <cstdint>

namespace core {

// Consecutive abnormal resets before the next boot skips the network and the
// effects. Three rather than two: a single panic plus its retry is a bad poll
// or a bad frame, and locking a working device into a diagnostic screen over
// one unlucky pair would be its own kind of failure.
constexpr int SAFE_MODE_STREAK = 3;

// How long a boot must survive before its predecessors are forgiven.
//
// This is the piece whose absence was the bug. The streak lived in NVS and was
// only ever cleared by a CLEAN reset reason, so unrelated crashes accumulated:
// a board that panicked once, ran for a month, and panicked again was two
// thirds of the way into safe mode for two reasons that had nothing to do with
// each other. A crash LOOP is defined by the loop, so the counter has to be
// able to forget.
//
// 30 s is well past the risky part of a boot - WiFi, the token refresh, the
// first poll and the first few hundred frames have all happened by then.
constexpr uint32_t HEALTHY_AFTER_MS = 30000;

// Counting past the threshold buys nothing, and every increment is a flash
// write on a board that is already not recovering.
constexpr int CRASH_STREAK_MAX = SAFE_MODE_STREAK + 1;

// The streak to store for this boot, given what was stored before it.
//
// `stored` is whatever NVS returned, which on a damaged partition can be
// anything at all. A negative value must not walk backwards away from safe
// mode on a board that is visibly crashing, so it is treated as zero.
inline int nextCrashStreak(int stored, bool abnormal_reset) {
  if (!abnormal_reset) return 0;
  const int base = stored > 0 ? stored : 0;
  const int next = base + 1;
  return next > CRASH_STREAK_MAX ? CRASH_STREAK_MAX : next;
}

inline bool safeModeWanted(int streak) { return streak >= SAFE_MODE_STREAK; }

inline bool streakForgiven(uint32_t uptime_ms) {
  return uptime_ms >= HEALTHY_AFTER_MS;
}

}  // namespace core
