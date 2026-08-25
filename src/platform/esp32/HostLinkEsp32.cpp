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
    if (holding_) return;  // still waiting; do not take a second client
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

    char body[MacLink::MAX_BODY + 1];
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

  bool known = false;
  if (std::strstr(line, "/locked")) {
    reported_locked_ = true;
    known = true;
  } else if (std::strstr(line, "/awake")) {
    reported_locked_ = false;
    known = true;
  }

  // 200 with an explicit Content-Length, and flushed before the close.
  //
  // This was a bare 204 followed straight by stop(), which curl accepted and
  // Node's fetch did not - it reported "fetch failed" for a request the device
  // was answering. Closing the socket immediately after print() can drop the
  // buffered bytes, and a 204 with no length header gives a strict client
  // nothing to frame the response with. The heartbeat silently never arrived.
  if (known) {
    ever_heard_ = true;
    last_beat_ms_ = now_ms;
    client.print(
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n");
  } else {
    // Anything else is answered plainly rather than ignored, so a person poking
    // at it with a browser learns what it wants.
    static const char kBody[] =
        "GET /awake or /locked, or POST /beat with key=value lines\n";
    client.printf(
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n%s",
        (unsigned)(sizeof(kBody) - 1), kBody);
  }
  client.flush();
  client.stop();
}

}  // namespace net

#endif
