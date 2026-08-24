#if defined(DEVICE)

#include <ESPmDNS.h>

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
  WiFiClient client = g_server.available();
  if (!client) return;

  // Bounded read of the request line only. An unbounded read here would let one
  // slow or hostile client stall the render loop, which is the trap the
  // ancestor's notes call out about serving anything from a device.
  char line[96];
  size_t n = 0;
  const uint32_t deadline = millis() + 40;
  while (client.connected() && n < sizeof(line) - 1 && millis() < deadline) {
    if (!client.available()) continue;
    const int ch = client.read();
    if (ch < 0 || ch == '\n') break;
    if (ch != '\r') line[n++] = static_cast<char>(ch);
  }
  line[n] = '\0';

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
    static const char kBody[] = "GET /awake or /locked, on a timer\n";
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
