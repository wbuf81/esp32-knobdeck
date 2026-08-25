#include "NetWorker.h"

#include <cstdint>

#include "../core/Clock.h"
#include "../core/MergePolicy.h"
#include "NetLog.h"

#if !defined(DEVICE)
#include <chrono>
#endif

namespace {

// How often the task wakes. The source decides when it actually polls; this is
// only the granularity at which a queued command gets picked up, so it is pure
// added latency on every button press. At 100ms it was a visible part of the
// ~1.2s lag on next/previous. The loop is cheap — a mutex, a struct copy, and
// a time comparison — so a faster tick costs almost nothing.
constexpr uint32_t TICK_MS = 25;

// Far beyond any real request — connect plus read timeouts total well under
// this — so tripping it means the task is genuinely wedged.
constexpr uint32_t NET_STALL_LIMIT_MS = 90000;

#if defined(DEVICE)
// mbedTLS handshakes are stack-hungry. The pthread default is nowhere near
// enough and the failure mode is a stack-overflow panic partway through TLS.
constexpr uint32_t NET_TASK_STACK = 16384;
// Core 0: the Arduino loop, and therefore all rendering, runs on core 1.
constexpr BaseType_t NET_TASK_CORE = 0;
constexpr UBaseType_t NET_TASK_PRIO = 1;
#endif

void napMs(uint32_t ms) {
#if !defined(DEVICE)
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
#else
  vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

}  // namespace


NetWorker::NetWorker(const char *client_id, const char *client_secret,
                     const char *refresh_token)
    : source_(client_id, client_secret, refresh_token) {
  state_.link = LinkStatus::Connecting;
}

NetWorker::~NetWorker() { stop(); }

void NetWorker::start(const std::string &cache_dir, const char *wifi_ssid,
                      const char *wifi_password) {
  cache_dir_ = cache_dir;
  ssid_ = wifi_ssid;
  password_ = wifi_password;
  source_.begin(cache_dir_);
  running_ = true;

#if !defined(DEVICE)
  thread_ = std::thread(&NetWorker::run, this);
#else
  xTaskCreatePinnedToCore(&NetWorker::taskEntry, "net", NET_TASK_STACK, this,
                          NET_TASK_PRIO, &task_, NET_TASK_CORE);
#endif
}

void NetWorker::stop() {
  running_ = false;
#if !defined(DEVICE)
  if (thread_.joinable()) thread_.join();
#endif
}

#if defined(DEVICE)
void NetWorker::taskEntry(void *self) {
  static_cast<NetWorker *>(self)->run();
  vTaskDelete(nullptr);
}
#endif

bool NetWorker::submit(const Command &in) {
  Command c = in;
  c.submitted_ms = nowMs();
  std::lock_guard<std::mutex> lk(mtx_);
  if (c.type == CommandType::SetVolume) {
    // Only the final volume matters; replace any pending one.
    return cmds_.pushCoalesced(c);
  }
  return cmds_.push(c);
}

bool NetWorker::stalled(uint32_t now_ms) const {
  const uint32_t hb = heartbeat_ms_.load();
  if (hb == 0) return false;  // not started yet

  // now_ms is sampled at the top of the UI frame, and the net task keeps
  // running while that frame renders, so the heartbeat can legitimately be
  // NEWER than the timestamp we are asked about. Subtracting unsigned then
  // gave ~4 billion and tripped the limit instantly. The slower the view, the
  // wider that window: the pixel mode's full-screen redraw made it reboot the
  // device mid-song. Treat any age in the top half of the range as "ahead of
  // us", which is the same wrap-safe convention Deadline uses.
  const uint32_t age = now_ms - hb;
  if (age > (UINT32_MAX / 2)) return false;
  return age > NET_STALL_LIMIT_MS;
}

AppState NetWorker::snapshot() {
  std::lock_guard<std::mutex> lk(mtx_);
  return state_;
}

namespace {
// Only on a change: a per-iteration log at 40Hz would bury everything else.
void logLinkChange(LinkStatus s) {
  static LinkStatus last = LinkStatus::Booting;
  static bool first = true;
  if (!first && s == last) return;
  first = false;
  last = s;
  static const char *NAMES[] = {"booting", "connecting", "online",
                                "offline", "auth-error", "reauth-needed"};
  NETLOG("link -> %s", NAMES[static_cast<int>(s)]);
}
}  // namespace

void NetWorker::run() {
  NETLOG("net task started");
  wifi_.begin(ssid_, password_);

  while (running_) {
    const uint32_t now = nowMs();
    heartbeat_ms_.store(now);
    {
      // Proof of life, independent of whether any request succeeds. Silence
      // here means the loop itself is wedged, which is a different problem to
      // requests failing.
      static uint32_t last = 0;
      if (now - last > 60000) {
        last = now;
        NETLOG("net alive (uptime %us)", (unsigned)(now / 1000));
      }
    }

    if (!wifi_.ensureConnected(now)) {
      // No link yet. Publish the connection state so the status screen can be
      // specific about why, and attempt no HTTP at all.
      {
        std::lock_guard<std::mutex> lk(mtx_);
        state_.link = wifi_.status();
      }
      logLinkChange(wifi_.status());
      napMs(TICK_MS);
      continue;
    }

    // WiFi IS associated, so publish that before any HTTP is attempted.
    //
    // Without this, LinkStatus only ever became Online inside call() - after a
    // successful Spotify request - which conflated "the network is up" with
    // "Spotify answered". HostLink is gated on network_up and has nothing to do
    // with Spotify, so a device that booted straight into a rate limit never
    // started its HTTP server at all: it pinged fine and port 80 was closed.
    //
    // Latent until the rate limit was made to survive reboots, because every
    // boot used to poll successfully before anyone looked.
    //
    // step() still downgrades this - a 401 to AuthError, a failed request to
    // Offline - so nothing is being papered over. It is only the floor that
    // moves: associated means the link is up.
    {
      std::lock_guard<std::mutex> lk(mtx_);
      if (state_.link != LinkStatus::Online) {
        state_.link = LinkStatus::Online;
        logLinkChange(LinkStatus::Online);
      }
    }

    AppState scratch;
    CommandQueue<> local;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      scratch = state_;
      Command c;
      while (cmds_.pop(&c)) local.push(c);
    }

    source_.setIdlePoll(screen_asleep_.load());
    if (wake_nudge_.exchange(false)) source_.nudge();
    {
      // Copied under the lock the setter uses; step() then runs without it.
      std::lock_guard<std::mutex> lk(mtx_);
      source_.setPreferredDevice(preferred_device_);
    }

    // All network I/O happens here, holding no lock.
    source_.step(&scratch, &local, now);

    {
      std::lock_guard<std::mutex> lk(mtx_);
      mergePlayback(&state_, scratch, source_.polledThisStep(), nowMs());
    }
    logLinkChange(scratch.link);

#if defined(TRACE_RENDER)
    {
      static uint32_t win = 0;
      static uint32_t iters = 0;
      ++iters;
      if (win == 0) win = now;
      if (now - win >= 5000) {
        NETLOG("net: %.1f iterations/s", iters * 1000.0f / (now - win));
        iters = 0;
        win = now;
      }
    }
#endif

    napMs(TICK_MS);
  }
}
