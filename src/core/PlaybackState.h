#pragma once

// What is playing, as plain data with no behaviour.
//
// Fixed-size char buffers rather than std::string: this struct is written by the
// net task and read by the UI task on a device with ~150KB of usable heap, and
// avoiding per-poll allocation churn matters more than convenience.

#include <cstdint>
#include <cstring>

constexpr size_t ID_LEN = 40;
constexpr size_t TEXT_LEN = 128;
constexpr size_t PATH_LEN = 128;

struct PlaybackState {
  bool has_track = false;
  bool is_playing = false;
  bool has_device = true;

  char track_id[ID_LEN] = {};
  char album_id[ID_LEN] = {};
  char title[TEXT_LEN] = {};
  char artist[TEXT_LEN] = {};

  // Local path to decodable artwork: a file on SD on hardware, a file on disk
  // in the emulator. Empty means "no artwork available", which must render as a
  // flat block rather than failing.
  char art_path[PATH_LEN] = {};

  // True while a cover download is in flight for the current album. Lets the
  // UI say "fetching" instead of "no artwork" — the old message claimed
  // failure during the one or two seconds every uncached album spends
  // downloading, which made a working device look broken.
  bool art_loading = false;

  uint32_t duration_ms = 0;
  uint32_t progress_ms = 0;

  // -1 means the active device did not report a volume. Never coerce to 0 —
  // that would render a plausible-looking lie.
  int volume_pct = -1;

  bool liked = false;

  // Whether `liked` means anything. Spotify's /me/tracks/contains is restricted
  // for some apps and returns 403 even with user-library-read granted, in which
  // case saved-state is genuinely unknowable and must not be drawn as "not
  // liked" — that would be a confident lie.
  bool liked_known = false;
};

inline void setStr(char *dst, size_t cap, const char *src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}
