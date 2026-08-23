#pragma once

// The real Spotify Web API source.
//
// Same role as FakeSource: it owns remote truth and publishes into AppState.
// Screens do not know which one is running.
//
// Runs on the net thread. Every method here blocks on network I/O, which is
// exactly why it must not share a thread with rendering.

#include <cstdint>
#include <string>
#include <vector>

#include "../art/ArtCache.h"
#include "../core/AppState.h"
#include "../core/Deadline.h"
#include "../core/CommandQueue.h"
#include "../net/HttpClient.h"
#include "SpotifyAuth.h"

class SpotifySource {
 public:
  SpotifySource(const char *client_id, const char *client_secret,
                const char *refresh_token);

  void begin(const std::string &cache_dir);

  // One iteration: run queued commands, then poll if due. `out` is a scratch
  // state the caller merges under lock — this never touches shared memory.
  void step(AppState *out, CommandQueue<> *cmds, uint32_t now_ms);

  // True only when this step actually got a fresh player response. The caller
  // must not merge playback fields otherwise — the scratch state would be a
  // stale snapshot and would undo the UI's progress extrapolation.
  bool polledThisStep() const { return polled_; }

  // Screen-asleep hint from the UI. While set, the paused/idle poll stretches
  // to POLL_ASLEEP_MS — nobody is looking, so 5s polling all night is pure
  // Spotify traffic for nothing. Net thread only.
  void setIdlePoll(bool idle) { idle_poll_ = idle; }

  // Poll now rather than waiting out the current interval. Called on wake, so
  // the first frame someone sees is never up to 20s stale.
  void nudge() { next_poll_.disarm(); }

 private:
  bool authHeaders(std::vector<std::string> *headers, uint32_t now_ms);
  bool call(const char *method, const std::string &url, const std::string &body,
            HttpResponse *resp, AppState *out, uint32_t now_ms);
  void runCommand(const Command &c, AppState *out, uint32_t now_ms);
  void pollPlayer(AppState *out, uint32_t now_ms);
  void refreshLiked(AppState *out, uint32_t now_ms);
  void diagnose(AppState *out, uint32_t now_ms);
  void probeLibraryWrite(AppState *out, uint32_t now_ms);

  SpotifyAuth auth_;
  ArtCache art_;

  std::string last_liked_track_;
  Deadline next_poll_;
  Deadline rate_limited_;
  bool polled_ = false;

  // After a skip, poll rapidly until the track actually changes rather than
  // waiting a fixed delay and hoping. Spotify needs a moment to settle, and
  // that moment varies.
  std::string confirm_track_;
  Deadline confirm_;
  unsigned confirm_polls_ = 0;

  // The saved-state check is a whole extra round trip. Running it in the same
  // step as the poll kept the new track off screen until it finished.
  bool liked_pending_ = false;

  // Artwork is fetched after the track is already on screen. Downloading it
  // inside the poll kept metadata we already had waiting on a 46KB transfer.
  std::string pending_art_album_;
  std::string pending_art_url_;

  // Cleared permanently once /me/tracks/contains answers 403: the restriction
  // is per-app, so retrying every poll would just burn rate limit forever.
  bool liked_supported_ = true;

  bool idle_poll_ = false;
};
