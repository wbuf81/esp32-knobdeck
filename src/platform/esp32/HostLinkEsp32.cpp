#if defined(DEVICE)

#include <ESPmDNS.h>

#include <cstdlib>
#include <cstring>
#include <WiFi.h>

#include "core/Log.h"
#include "net/HostLink.h"

namespace net {
namespace {

// A raw WiFiServer and a hand-parsed request line, rather than the WebServer
// library. This needs one route and no body parsing, and internal SRAM is the
// binding constraint on this board - the library's buffers are not worth it.
WiFiServer g_server(HostLink::PORT);

}  // namespace

// Everything here is started lazily from poll(), once the network is actually
// up. An earlier version started the socket and mDNS in begin(), from setup(),
// before WiFi had associated - and that crash-looped the board. On this board
// the USB serial port is provided by the running firmware, so a crash loop also
// removes the log that would explain it and the port needed to flash a fix. It
// took a physical download-mode recovery to undo.
void HostLink::tryStart(uint32_t now_ms) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (now_ms < next_try_ms_) return;
  next_try_ms_ = now_ms + 3000;

  if (!socket_up_) {
    g_server.begin();
    g_server.setNoDelay(true);
    socket_up_ = true;
    LOGF("hostlink: listening on http://%s:%u/",
         WiFi.localIP().toString().c_str(), (unsigned)PORT);
  }
  if (!mdns_up_ && mdns_name_ && mdns_name_[0] && MDNS.begin(mdns_name_)) {
    MDNS.addService("http", "tcp", PORT);
    mdns_up_ = true;
    LOGF("hostlink: also http://%s.local:%u/", mdns_name_, (unsigned)PORT);
  }
}

void HostLink::poll(uint32_t now_ms, bool network_up) {
  if (!network_up) return;
  tryStart(now_ms);
  if (!socket_up_) return;

  // If the helper has gone quiet, forget that we announced it, so its return is
  // reported rather than silently resumed.
  if (mac_seen_ && mac_.stale(now_ms)) {
    mac_seen_ = false;
    LOGF("maclink: helper went quiet");
  }

  // A held client is checked FIRST and answered the moment a command appears or
  // the hold expires. This is what makes a knob turn feel instant while costing
  // the Mac one sleeping socket read - see MacLink::HOLD_MS for why holding is
  // lighter on the laptop than a fast heartbeat, not merely faster.
  static WiFiClient held;
  if (holding_) {
    // Signed compare, so this is correct across the millis wrap. An unsigned
    // (now - until) would read as enormous just after the wrap and expire the
    // hold instantly, forever.
    const bool expired = static_cast<int32_t>(now_ms - hold_until_ms_) >= 0;
    if (!held.connected()) {
      // stop() even though the peer has gone: the fd is only released when this
      // static client is reassigned or stopped, so clearing the flag alone
      // leaked a descriptor per dropped helper connection.
      held.stop();
      holding_ = false;
    } else if (mac_.hasPending() || expired) {
      char body[192];
      const int n = mac_.buildResponse(body, sizeof(body));
      held.printf(
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
          "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
          (unsigned)n, body);
      held.flush();
      held.stop();
      holding_ = false;
    }
  }

  // A second client while one is held is ANSWERED, not ignored. Returning here
  // left it in the listen backlog until the hold expired, which added up to a
  // second of latency to anything else talking to the device - a browser poke,
  // a second helper - and contradicted the design. The held client keeps its
  // place; the newcomer just gets no command.
  if (holding_) {
    WiFiClient other = g_server.available();
    if (other) {
      static const char kBusy[] = "held\n";
      other.printf(
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
          "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
          (unsigned)(sizeof(kBusy) - 1), kBusy);
      other.flush();
      other.stop();
    }
    return;
  }

  WiFiClient client = g_server.available();
  if (!client) return;

  // Bounded read of the request line only. An unbounded read here would let one
  // slow or hostile client stall the render loop, which is the trap the
  // ancestor's notes call out about serving anything from a device.
  char line[96];
  size_t n = 0;
  uint32_t deadline = millis() + 40;
  while (client.connected() && n < sizeof(line) - 1 && millis() < deadline) {
    if (!client.available()) continue;
    const int ch = client.read();
    if (ch < 0 || ch == '\n') break;
    if (ch != '\r') line[n++] = static_cast<char>(ch);
  }
  line[n] = '\0';

  if (std::strstr(line, "/beat")) {
    // Headers then body, both bounded. Content-Length is READ but not trusted:
    // the read stops at MAX_BODY whatever it claims.
    int want = 0;
    char hdr[128];
    deadline = millis() + 60;
    while (client.connected() && millis() < deadline) {
      size_t h = 0;
      while (client.connected() && h < sizeof(hdr) - 1 && millis() < deadline) {
        if (!client.available()) continue;
        const int ch = client.read();
        if (ch < 0 || ch == '\n') break;
        if (ch != '\r') hdr[h++] = static_cast<char>(ch);
      }
      hdr[h] = '\0';
      if (h == 0) break;  // blank line: headers done
      if (strncasecmp(hdr, "Content-Length:", 15) == 0) {
        want = std::atoi(hdr + 15);
      }
    }
    if (want < 0) want = 0;
    if (want > static_cast<int>(MacLink::MAX_BODY)) {
      want = static_cast<int>(MacLink::MAX_BODY);
    }

    // Static, not automatic. 513 bytes on the stack of the task that also runs
    // the whole render loop is a lot to spend for one route, and a stack
    // overflow here is a panic rather than something the watchdog can catch.
    // Single-threaded by construction: poll() only ever runs on the UI task.
    static char body[MacLink::MAX_BODY + 1];
    int got = 0;
    deadline = millis() + 60;
    while (client.connected() && got < want && millis() < deadline) {
      if (!client.available()) continue;
      const int ch = client.read();
      if (ch < 0) break;
      body[got++] = static_cast<char>(ch);
    }
    body[got] = '\0';

    if (!mac_.applyBeat(body, now_ms)) {
      // Logged sparsely. A rejected beat repeats at the helper's rate, and a
      // per-beat log would bury everything else - the same reason the rate
      // limit note is throttled.
      static uint32_t last_gripe = 0;
      if (now_ms - last_gripe > 60000) {
        last_gripe = now_ms;
        LOGF("hostlink: /beat REJECTED (token or version)");
      }
      static const char kDeny[] = "unauthorized\n";
      client.printf(
          "HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\n"
          "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
          (unsigned)(sizeof(kDeny) - 1), kDeny);
      client.flush();
      client.stop();
      return;
    }

    // Announced ONCE, and again if it ever comes back after going quiet.
    // Without this there is no way to tell from the device whether the channel
    // is working - which is the same blind spot that made a rate limit look
    // like a hang this afternoon.
    if (!mac_seen_) {
      mac_seen_ = true;
      const MacState &s = mac_.state();
      LOGF("maclink: beat from '%s' (spotify device '%s') vol=%d locked=%d",
           s.host, s.sp_device, s.out_vol, s.locked ? 1 : 0);
      if (s.sp_track[0]) {
        LOGF("maclink: mac says %s - %s (%d/%d ms)%s", s.sp_artist, s.sp_track,
             s.sp_pos_ms, s.sp_dur_ms, s.sp_playing ? "" : " [paused]");
      }
    }
    ever_heard_ = true;
    last_beat_ms_ = now_ms;

    if (mac_.hasPending()) {
      char out[192];
      const int len = mac_.buildResponse(out, sizeof(out));
      client.printf(
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
          "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
          (unsigned)len, out);
      client.flush();
      client.stop();
      return;
    }
    // Nothing to say yet. HOLD, so the next knob turn is answered within a
    // frame rather than a beat later.
    held = client;
    holding_ = true;
    hold_until_ms_ = now_ms + MacLink::HOLD_MS;
    return;
  }

  // GET /awake and GET /locked are GONE.
  //
  // They were unauthenticated writes to the one piece of state that can black
  // out the screen, and hostAsleep() checked reported_locked_ FIRST - ahead of
  // the token-checked MacLink state - so anything on the LAN could darken the
  // display and outrank the real helper. Verified by curling /locked with no
  // credentials and watching every render timer go to zero.
  //
  // They were kept for a partially-updated setup that could still report sleep
  // state. The helper now speaks /beat only, so that rationale is gone and what
  // remained was an unauthenticated write path with no users.
  //
  // Answered plainly rather than ignored, so a person poking at it with a
  // browser learns what it wants. An explicit Content-Length and a flush before
  // the close: a bare 204 followed straight by stop() was accepted by curl and
  // REJECTED by Node's fetch, which reported "fetch failed" for a request the
  // device was answering.
  static const char kBody[] =
      "POST /beat with key=value lines and a valid token\n";
  client.printf(
      "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
      "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
      (unsigned)(sizeof(kBody) - 1), kBody);
  client.flush();
  client.stop();
}

}  // namespace net

#endif
