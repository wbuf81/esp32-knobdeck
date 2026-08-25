#pragma once

// When free heap has fallen far enough to matter.
//
// Pure, like CrashPolicy, and for the same reason: the interesting part is the
// hysteresis, not the reading. The deleted Diag.h declared a heapTick() that
// was never written, and the number it would have watched is the one that
// decides whether TLS keeps working - mbedTLS handshakes are the largest
// transient allocation on this device, and the symptom when they start failing
// is requests quietly not happening rather than anything that looks like a
// memory problem.
//
// Measured on this board: free INTERNAL heap sits at 48-51 KB during playback
// with artwork decoded, and around 104 KB before the first cover lands.

#include <cstddef>

namespace core {

// Below this, a handshake is at risk.
constexpr size_t HEAP_FLOOR_BYTES = 24000;

// And it must climb back to here before the alarm rearms. Rearming at the floor
// would make a value hovering on the boundary fire on alternate frames, which is
// a serial flood - and a flood is how the line you needed gets pushed off the
// top of the buffer. This project has twice found that a diagnostic can be the
// bug; a warning that drowns the log is that same mistake.
constexpr size_t HEAP_CLEAR_BYTES = 34000;

class HeapWatch {
 public:
  // Call every frame with the current free heap. True EXACTLY ONCE per
  // crossing, so the caller can log or toast without rate-limiting it itself.
  bool observe(size_t free_bytes) {
    if (armed_ && free_bytes < HEAP_FLOOR_BYTES) {
      armed_ = false;
      return true;
    }
    if (!armed_ && free_bytes >= HEAP_CLEAR_BYTES) armed_ = true;
    return false;
  }

 private:
  bool armed_ = true;
};

}  // namespace core
