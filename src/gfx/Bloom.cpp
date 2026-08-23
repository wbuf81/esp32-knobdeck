#include "Bloom.h"

#include "Blend.h"
#include "Color.h"

namespace gfx {
namespace {

constexpr int SW = Bloom::SMALL_W;
constexpr int SH = Bloom::SMALL_H;
constexpr int SC = Bloom::SCALE;

inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b) {
  // Integer approximation of Rec.601: (77r + 150g + 29b) / 256.
  return static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
}

inline size_t idx(int x, int y) {
  return (static_cast<size_t>(y) * SW + x) * 3;
}

}  // namespace

Bloom::Bloom()
    : small_(static_cast<size_t>(SW) * SH * 3, 0),
      tmp_(static_cast<size_t>(SW) * SH * 3, 0) {}

void Bloom::brightPassDownscale(const Framebuffer &fb, uint8_t threshold) {
  const uint16_t *px = fb.pixels();
  for (int sy = 0; sy < SH; ++sy) {
    for (int sx = 0; sx < SW; ++sx) {
      uint32_t ar = 0, ag = 0, ab = 0;
      for (int y = 0; y < SC; ++y) {
        const uint16_t *row = px + static_cast<size_t>(sy * SC + y) * W +
                              static_cast<size_t>(sx) * SC;
        for (int x = 0; x < SC; ++x) {
          uint8_t r, g, b;
          unpack565(row[x], r, g, b);
          if (luma(r, g, b) < threshold) continue;
          ar += r;
          ag += g;
          ab += b;
        }
      }
      const uint32_t n = SC * SC;
      const size_t i = idx(sx, sy);
      small_[i + 0] = static_cast<uint8_t>(ar / n);
      small_[i + 1] = static_cast<uint8_t>(ag / n);
      small_[i + 2] = static_cast<uint8_t>(ab / n);
    }
  }
}

// One separable 3-tap box blur: a horizontal sweep into tmp_, then a vertical
// sweep back into small_. All arithmetic stays 8-bit per channel.
void Bloom::blurPass() {
  for (int y = 0; y < SH; ++y) {
    for (int x = 0; x < SW; ++x) {
      const int xm = x > 0 ? x - 1 : 0;
      const int xp = x < SW - 1 ? x + 1 : SW - 1;
      const size_t im = idx(xm, y), ic = idx(x, y), ip = idx(xp, y);
      const size_t o = ic;
      for (int c = 0; c < 3; ++c) {
        tmp_[o + c] = static_cast<uint8_t>(
            (small_[im + c] + 2 * small_[ic + c] + small_[ip + c]) >> 2);
      }
    }
  }
  for (int y = 0; y < SH; ++y) {
    const int ym = y > 0 ? y - 1 : 0;
    const int yp = y < SH - 1 ? y + 1 : SH - 1;
    for (int x = 0; x < SW; ++x) {
      const size_t im = idx(x, ym), ic = idx(x, y), ip = idx(x, yp);
      for (int c = 0; c < 3; ++c) {
        small_[ic + c] = static_cast<uint8_t>(
            (tmp_[im + c] + 2 * tmp_[ic + c] + tmp_[ip + c]) >> 2);
      }
    }
  }
}

void Bloom::upscaleAdd(Framebuffer &fb, uint8_t strength) {
  uint16_t *px = fb.pixels();
  for (int y = 0; y < H; ++y) {
    // Bilinear source coordinate in 8.8 fixed point.
    const int fy = (y * 256) / SC;
    const int sy0 = fy >> 8;
    const int sy1 = sy0 < SH - 1 ? sy0 + 1 : SH - 1;
    const uint32_t wy = static_cast<uint32_t>(fy & 0xFF);
    const uint32_t iwy = 256u - wy;
    uint16_t *row = px + static_cast<size_t>(y) * W;
    for (int x = 0; x < W; ++x) {
      const int fx = (x * 256) / SC;
      const int sx0 = fx >> 8;
      const int sx1 = sx0 < SW - 1 ? sx0 + 1 : SW - 1;
      const uint32_t wx = static_cast<uint32_t>(fx & 0xFF);
      const uint32_t iwx = 256u - wx;

      const size_t a = idx(sx0, sy0), b = idx(sx1, sy0);
      const size_t c = idx(sx0, sy1), d = idx(sx1, sy1);

      uint8_t ch[3];
      for (int k = 0; k < 3; ++k) {
        // Bilinear and the strength scale both happen in 8-bit space, so the
        // faint tail of the glow survives to the pack instead of quantising to
        // black on the way in.
        const uint32_t top = (small_[a + k] * iwx + small_[b + k] * wx) >> 8;
        const uint32_t bot = (small_[c + k] * iwx + small_[d + k] * wx) >> 8;
        const uint32_t v = (top * iwy + bot * wy) >> 8;
        ch[k] = static_cast<uint8_t>((v * strength) / 255u);
      }
      row[x] = addSat(row[x], rgb565(ch[0], ch[1], ch[2]));
    }
  }
}

void Bloom::apply(Framebuffer &fb, uint8_t threshold, uint8_t strength) {
  if (strength == 0) return;
  brightPassDownscale(fb, threshold);
  for (int i = 0; i < PASSES; ++i) blurPass();
  upscaleAdd(fb, strength);
}

}  // namespace gfx
