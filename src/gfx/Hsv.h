#pragma once

// HSV to RGB565, for building palettes from a single hue.

#include <cstdint>

#include "Color.h"

namespace gfx {

// h in 0..1 (wrapping), s and v in 0..1.
inline void hsv888(float h, float s, float v, uint8_t &orr, uint8_t &og,
                   uint8_t &ob);

inline uint16_t hsv565(float h, float s, float v) {
  uint8_t r, g, b;
  hsv888(h, s, v, r, g, b);
  return rgb565(r, g, b);
}

inline void hsv888(float h, float s, float v, uint8_t &orr, uint8_t &og,
                   uint8_t &ob) {
  h -= static_cast<float>(static_cast<int>(h));
  if (h < 0.0f) h += 1.0f;
  const float H = h * 6.0f;
  const int i = static_cast<int>(H);
  const float f = H - static_cast<float>(i);
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - s * f);
  const float t = v * (1.0f - s * (1.0f - f));
  float r, g, b;
  switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  const auto c8 = [](float x) {
    const int v8 = static_cast<int>(x * 255.0f + 0.5f);
    return static_cast<uint8_t>(v8 < 0 ? 0 : (v8 > 255 ? 255 : v8));
  };
  orr = c8(r);
  og = c8(g);
  ob = c8(b);
}

}  // namespace gfx
