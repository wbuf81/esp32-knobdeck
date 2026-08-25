#pragma once

// Whether the computer this sits beside is awake.
//
// The Mac pushes; this device listens. That direction is deliberate. The
// alternative - a small server on the Mac that the knob polls - means an
// unauthenticated listening port on a laptop, where here it is a port on a
// dedicated appliance that does one thing.
//
// The heartbeat is what makes it correct rather than merely convenient. A Mac
// that goes to sleep cannot send "I am asleep"; it simply stops sending. So the
// host reports its lock state on a timer, and SILENCE is the signal for sleep.
// Without that, sleeping would be indistinguishable from the daemon not running.
//
// It fails OPEN: an unreachable or never-heard-from host reads as awake, so a
// crashed daemon, a renamed Mac or a WiFi change cannot leave the screen dark
// forever. That is the same choice the Stream Deck daemon makes for the same
// reason - and getting it the other way round would mean a dead helper
// permanently bricks the display.

#include <cstdint>

#include "net/MacLink.h"

namespace net {

class HostLink {
 public:
  // How long silence lasts before the host counts as asleep. Comfortably more
  // than the sender's interval, so one dropped packet is not a blackout.
  static constexpr uint32_t TIMEOUT_MS = 25000;
  // Until the first heartbeat ever arrives, the host is assumed awake and this
  // whole mechanism is inert.
  static constexpr uint16_t PORT = 80;

  // Remembers the name and does NOTHING else. Safe to call from setup().
  void configure(const char *mdns_name) { mdns_name_ = mdns_name; }

  // Non-blocking. Call once per frame with whether the network is up.
  //
  // Both the listening socket and mDNS are started from HERE, not from a begin()
  // in setup(). Starting them before WiFi associates crash-looped the board, and
  // on a device whose serial port is provided by the firmware a crash loop also
  // takes away the only way to see why - and the only way to flash a fix.
  void poll(uint32_t now_ms, bool network_up);

  // True only once a heartbeat has been heard at least once AND either it said
  // locked or it has since gone quiet.
  bool hostAsleep(uint32_t now_ms) const;

  // Passed straight through to the MacLink. Empty disables the command channel.
  void setMacToken(const char *tok) { mac_.setToken(tok); }

  // The Mac's reported state, and the place to queue a command for it.
  MacLink &mac() { return mac_; }
  const MacLink &mac() const { return mac_; }
  bool macStale(uint32_t now_ms) const { return mac_.stale(now_ms); }

  bool everHeard() const { return ever_heard_; }
  uint32_t lastBeatMs() const { return last_beat_ms_; }

 private:
  bool ever_heard_ = false;
  uint32_t last_beat_ms_ = 0;
  const char *mdns_name_ = nullptr;
  bool socket_up_ = false;
  bool mdns_up_ = false;
  uint32_t next_try_ms_ = 0;

  MacLink mac_;
  // ONE held client at a time. poll() runs in the render loop, so this is a
  // deadline check across frames, never a wait inside one. A second client
  // arriving while this is held is answered immediately with no command.
  // Whether a good beat has been announced. Reset by macStale so a helper that
  // comes back after a gap says so again.
  bool mac_seen_ = false;
  bool holding_ = false;
  uint32_t hold_until_ms_ = 0;

  void tryStart(uint32_t now_ms);
};

}  // namespace net
