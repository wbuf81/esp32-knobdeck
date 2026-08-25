#include "net/HostLink.h"

namespace net {


bool HostLink::hostAsleep(uint32_t now_ms) const {
  // Never heard from: this mechanism is not in use, so it must not darken
  // anything. Failing open is the whole point.
  if (!ever_heard_) return false;
  if (reported_locked_) return true;
  // A /beat carrying locked=1 is the same statement in the newer protocol.
  if (mac_.state().valid && mac_.state().locked) return true;
  // Unsigned subtraction, correct across the millis wrap.
  return (now_ms - last_beat_ms_) > TIMEOUT_MS;
}

}  // namespace net
