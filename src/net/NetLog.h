#pragma once

// Opt-in network tracing, enabled with SPOTIFY_DEBUG=1.
//
// Logs status codes, URLs and Spotify's own error strings. Never logs the
// client secret, the refresh token, or an access token — a debug flag must not
// become the thing that leaks the credentials into a terminal scrollback.

#include <cstdio>
#include <cstdlib>

#include "core/Log.h"

namespace netlog {
// Prefixes and forwards, so a net line is formatted and written exactly once.
// Three separate printf calls per message is what let the render task's output
// interleave into the middle of one.
void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
}  // namespace netlog

inline bool netDebug() {
#if !defined(DEVICE)
  static const bool on = std::getenv("SPOTIFY_DEBUG") != nullptr;
  return on;
#else
  // On by default on the device. An ESP32 has no environment, so getenv always
  // returns null there — which meant every one of these lines was silently
  // suppressed on exactly the platform they were written to diagnose. The
  // serial log is the only instrument the hardware has; a few UART writes on a
  // 2s poll cost nothing worth saving.
  return true;
#endif
}

#define NETLOG(...)                 \
  do {                              \
    if (netDebug()) {               \
      ::netlog::line(__VA_ARGS__);  \
    }                               \
  } while (0)
