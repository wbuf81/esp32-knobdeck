#pragma once

// RGB565 packing, native-endian.

#include <cstdint>

namespace gfx {

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Expands back to 8-bit with the top bits replicated into the low ones, so
// white round-trips to 255 rather than 248.
inline void unpack565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  const uint8_t r5 = (c >> 11) & 0x1F;
  const uint8_t g6 = (c >> 5) & 0x3F;
  const uint8_t b5 = c & 0x1F;
  r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
  g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
  b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

}  // namespace gfx
