#pragma once

// Per-pixel blend operations. The hot path of every effect in the project.
//
// All three operate on packed RGB565 without unpacking to separate channels,
// because unpack-blend-repack is roughly three times the instruction count and
// these run tens of millions of times per second. The masking is what keeps
// channels from bleeding into each other; getting that wrong produces rainbow
// noise rather than a subtle error, which is at least an honest failure.

#include <cstdint>

namespace gfx {

// Per-channel saturating add. This is what turns particles into light rather
// than dots, and it is the single most visually important function here.
inline uint16_t addSat(uint16_t a, uint16_t b) {
  uint32_t r = (a & 0xF800u) + (b & 0xF800u);
  uint32_t g = (a & 0x07E0u) + (b & 0x07E0u);
  uint32_t bl = (a & 0x001Fu) + (b & 0x001Fu);
  if (r > 0xF800u) r = 0xF800u;
  if (g > 0x07E0u) g = 0x07E0u;
  if (bl > 0x001Fu) bl = 0x001Fu;
  return static_cast<uint16_t>(r | g | bl);
}

// Multiply toward black by num/256. Used once per frame over the persistence
// buffer, which is what turns moving particles into comet trails.
inline uint16_t fade(uint16_t c, uint8_t num) {
  const uint32_t r = ((c & 0xF800u) * num) >> 8;
  const uint32_t g = ((c & 0x07E0u) * num) >> 8;
  const uint32_t b = ((c & 0x001Fu) * num) >> 8;
  return static_cast<uint16_t>((r & 0xF800u) | (g & 0x07E0u) | (b & 0x001Fu));
}

// Linear interpolation, t in 0..256 so both endpoints are exact. A 0..255
// range cannot represent "all of b" and quietly darkens every full-strength
// blend by one step.
inline uint16_t lerp565(uint16_t a, uint16_t b, uint16_t t) {
  if (t > 256) t = 256;
  const uint32_t it = 256u - t;
  const uint32_t ar = (a >> 11) & 0x1Fu, ag = (a >> 5) & 0x3Fu, ab = a & 0x1Fu;
  const uint32_t br = (b >> 11) & 0x1Fu, bg = (b >> 5) & 0x3Fu, bb = b & 0x1Fu;
  const uint32_t r = (ar * it + br * t) >> 8;
  const uint32_t g = (ag * it + bg * t) >> 8;
  const uint32_t bl = (ab * it + bb * t) >> 8;
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

}  // namespace gfx
