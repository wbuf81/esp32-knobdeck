#pragma once

// One millisecond clock, defined explicitly per platform rather than relying on
// whichever millis() happens to be in scope.
//
// Note that the RENDERER does not use this: effects receive a dt and never read
// a clock, which is what makes a headless run bit-exact. This is for the network
// and Spotify layers, where real elapsed time is the point.

#include <cstdint>

#if defined(DEVICE)
#include <Arduino.h>
inline uint32_t nowMs() { return static_cast<uint32_t>(millis()); }
#else
#include <SDL.h>
inline uint32_t nowMs() { return static_cast<uint32_t>(SDL_GetTicks()); }
#endif
