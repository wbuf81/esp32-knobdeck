#include "net/HostLink.h"

namespace net {


bool HostLink::hostAsleep(uint32_t now_ms) const {
  // Never heard from: this mechanism is not in use, so it must not darken
  // anything. Failing open is the whole point.
  if (!ever_heard_) return false;
  // The Mac's own report, over the authenticated channel. This used to be
  // preceded by a reported_locked_ flag set by an UNAUTHENTICATED GET /locked,
  // which therefore outranked it - so anything on the LAN could black out the
  // screen and beat the real helper to it. That route and that flag are gone.
  if (mac_.state().valid && mac_.state().locked) return true;
  // Unsigned subtraction, correct across the millis wrap.
  return (now_ms - last_beat_ms_) > TIMEOUT_MS;
}

}  // namespace net
