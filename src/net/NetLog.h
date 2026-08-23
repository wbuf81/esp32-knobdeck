#pragma once

// Opt-in network tracing, enabled with SPOTIFY_DEBUG=1.
//
// Logs status codes, URLs and Spotify's own error strings. Never logs the
// client secret, the refresh token, or an access token — a debug flag must not
// become the thing that leaks the credentials into a terminal scrollback.

#include <cstdio>
#include <cstdlib>

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

#define NETLOG(...)                        \
  do {                                     \
    if (netDebug()) {                      \
      std::fprintf(stderr, "[net] ");      \
      std::fprintf(stderr, __VA_ARGS__);   \
      std::fprintf(stderr, "\n");          \
    }                                      \
  } while (0)
