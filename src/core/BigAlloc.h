#pragma once

// Allocation for buffers too large for internal SRAM.
//
// On the device this is PSRAM, explicitly, and that is not a preference: after
// the renderer's band and bloom buffers there is about 38 KB of internal SRAM
// free, and mbedTLS needs 34 KB of it contiguous for one session. Anything
// measured in tens of kilobytes - a decoded cover, a playlist listing - has to
// go to PSRAM or it takes the network down with it.
//
// Deliberately does NOT fall back to internal memory on failure. A fallback
// succeeds for the small cases and then starves TLS in a way that surfaces
// somewhere else entirely; failing here is far easier to diagnose.

#include <cstddef>

namespace core {

void *bigAlloc(size_t bytes);
void bigFree(void *p);

}  // namespace core
