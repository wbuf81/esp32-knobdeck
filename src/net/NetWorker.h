#pragma once

// Runs WiFi association and SpotifySource off the UI thread.
//
// This is the design's core-0 net task. A Spotify call takes 200-500ms and a
// TLS handshake longer; sharing a thread with rendering would freeze the
// buttons every time one happened.
//
// On the device it is a real FreeRTOS task rather than std::thread, for two
// reasons std::thread cannot provide: it must be pinned to core 0 so it never
// competes with rendering on core 1, and it needs a much larger stack than the
// pthread default because an mbedTLS handshake is stack-hungry.
//
// Ownership rule on both platforms: the net task never holds the lock while
// doing I/O. It snapshots under lock, works on its own copy, then merges.

#include <atomic>
#include <mutex>
#include <string>

#include "../core/AppState.h"
#include "../core/CommandQueue.h"
#include "../spotify/SpotifySource.h"
#include "WifiLink.h"

#if !defined(DEVICE)
#include <atomic>
#include <thread>
#else
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

class NetWorker {
 public:
  NetWorker(const char *client_id, const char *client_secret,
            const char *refresh_token);
  ~NetWorker();

  void start(const std::string &cache_dir, const char *wifi_ssid,
             const char *wifi_password);
  void stop();

  // UI side: enqueue a command for the net task to execute.
  void submit(const Command &in);

  // UI side: copy the shared state out for rendering.
  AppState snapshot();

  // Borrowed. The browser's listings live here; the net task fills them, the
  // render task reads them through the library's generation counter.
  void setLibrary(spotify::Library *lib) { source_.setLibrary(lib); }

  // UI side: the decoded cover for an album, or null. Borrowed, not owned, and
  // valid until two further album changes - see art/CoverCache.h.
  const art::Image *cover(const char *album_id) const {
    return source_.cover(album_id);
  }

  // UI side: the screen went to sleep or woke. Asleep stretches the poll
  // interval; waking forces an immediate poll so the screen never shows stale
  // state after a button press.
  void setScreenAsleep(bool asleep) {
    const bool was = screen_asleep_.exchange(asleep);
    if (was && !asleep) wake_nudge_.store(true);
  }

  // True when the net task has not completed an iteration for far longer than
  // any legitimate network operation. It is deliberately not on the hardware
  // watchdog — it is allowed to block for seconds — so this is how a genuine
  // wedge gets noticed.
  bool stalled(uint32_t now_ms) const;

  // UI side: apply a locally-optimistic edit under the same lock the net task
  // merges with, so the two cannot interleave mid-update.
  template <typename Fn>
  void mutate(Fn fn) {
    std::lock_guard<std::mutex> lk(mtx_);
    fn(state_);
  }

 private:
  void run();

  SpotifySource source_;
  WifiLink wifi_;
  std::string cache_dir_;
  const char *ssid_ = nullptr;
  const char *password_ = nullptr;
  bool source_started_ = false;

  std::atomic<uint32_t> heartbeat_ms_{0};
  std::atomic<bool> screen_asleep_{false};
  std::atomic<bool> wake_nudge_{false};
  std::mutex mtx_;
  AppState state_;
  CommandQueue<> cmds_;

#if !defined(DEVICE)
  std::thread thread_;
  std::atomic<bool> running_{false};
#else
  static void taskEntry(void *self);
  TaskHandle_t task_ = nullptr;
  volatile bool running_ = false;
#endif
};
