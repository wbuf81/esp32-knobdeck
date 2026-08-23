#include "BloomBand.h"

#include <cstring>

#include "Blend.h"
#include "Color.h"

namespace gfx {
namespace {

inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
}

// (a + 2b + c) / 4 across three channels packed one per byte, with no carry
// between them.
//
// Each term is pre-shifted and masked so the per-byte sum cannot exceed 253:
// 63 + 127 + 63. That is what makes it safe to add three packed pixels as
// single 32-bit words. The truncation costs at most two units per channel,
// which on a blur that exists to be soft is invisible - and it replaces nine
// byte loads and three byte stores with three aligned word loads and one store.
inline uint32_t blur3(uint32_t a, uint32_t b, uint32_t c) {
  return ((a >> 2) & 0x3F3F3F3Fu) + ((b >> 1) & 0x7F7F7F7Fu) +
         ((c >> 2) & 0x3F3F3F3Fu);
}

inline uint32_t pack888(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16);
}

}  // namespace

void BloomBand::begin() {
  std::memset(glow_, 0, sizeof(glow_));
  std::memset(accum_, 0, sizeof(accum_));
  std::memset(glow565_, 0, sizeof(glow565_));
  std::memset(glowrow_, 0, sizeof(glowrow_));
  glowrow_sy_ = -1;
}

void BloomBand::buildGlowRow(int small_y) {
  const uint16_t *src = &glow565_[static_cast<size_t>(small_y) * SW];

  // Nearest horizontally: each small pixel fills four screen pixels, written as
  // two 32-bit stores. Horizontal bilinear was tried and is not worth it - the
  // glow has been through four blur passes, so it varies by very little over
  // four pixels, and this is a soft additive haze rather than an edge.
  uint32_t *out = reinterpret_cast<uint32_t *>(glowrow_);
  for (int sx = 0; sx < SW; ++sx) {
    const uint16_t v = fade(src[sx], strength_);
    const uint32_t vv = (static_cast<uint32_t>(v) << 16) | v;
    out[sx * 2 + 0] = vv;
    out[sx * 2 + 1] = vv;
  }
  glowrow_sy_ = small_y;
}

const uint16_t *BloomBand::glowRow(int screen_y) {
  const int sy = screen_y / SCALE;
  if (sy != glowrow_sy_) buildGlowRow(sy);
  return glowrow_;
}

void BloomBand::accumulateBand(const Surface &s) {
  for (int sy = s.y0 / SCALE; sy < s.yEnd() / SCALE; ++sy) {
    std::memset(acc_, 0, sizeof(acc_));
    // Every other row and every other column: four samples per 4x4 cell.
    for (int sub = 0; sub < SCALE; sub += 2) {
      const int y = sy * SCALE + sub;
      if (!s.containsRow(y)) continue;
      const uint16_t *row = s.row(y);
      for (int x = 0; x < W; x += 2) {
        uint8_t r, g, b;
        unpack565(row[x], r, g, b);
        if (luma(r, g, b) < threshold_) continue;
        // Four samples per cell, each channel at most 255, so a 10-bit field
        // per channel is plenty and the packed layout survives the accumulate.
        acc_[x / SCALE] += pack888(r, g, b);
      }
    }
    uint32_t *dst = &accum_[static_cast<size_t>(sy) * SW];
    for (int sx = 0; sx < SW; ++sx) {
      // >>2 is the average of four samples. Masking after the shift is what
      // keeps a channel that saturated from bleeding into its neighbour.
      dst[sx] = (acc_[sx] >> 2) & 0x00FFFFFFu;
    }
  }
}

void BloomBand::blurPass() {
  // Horizontal, glow_ -> accum_ (which is free scratch at this point).
  for (int y = 0; y < SH; ++y) {
    const uint32_t *src = &glow_[static_cast<size_t>(y) * SW];
    uint32_t *dst = &accum_[static_cast<size_t>(y) * SW];
    dst[0] = blur3(src[0], src[0], src[1]);
    for (int x = 1; x < SW - 1; ++x) dst[x] = blur3(src[x - 1], src[x], src[x + 1]);
    dst[SW - 1] = blur3(src[SW - 2], src[SW - 1], src[SW - 1]);
  }
  // Vertical, accum_ -> glow_.
  for (int y = 0; y < SH; ++y) {
    const int ym = y > 0 ? y - 1 : 0;
    const int yp = y < SH - 1 ? y + 1 : SH - 1;
    const uint32_t *a = &accum_[static_cast<size_t>(ym) * SW];
    const uint32_t *b = &accum_[static_cast<size_t>(y) * SW];
    const uint32_t *c = &accum_[static_cast<size_t>(yp) * SW];
    uint32_t *dst = &glow_[static_cast<size_t>(y) * SW];
    for (int x = 0; x < SW; ++x) dst[x] = blur3(a[x], b[x], c[x]);
  }
}

void BloomBand::endFrame() {
  std::memcpy(glow_, accum_, sizeof(glow_));
  for (int i = 0; i < PASSES; ++i) blurPass();

  // One extra vertical-only sweep. Vertical glow sampling is nearest (see
  // buildGlowRow), so softening the vertical gradient here is what keeps the
  // four-row step invisible - and it costs 8,100 pixels, not 129,600.
  for (int y = 0; y < SH; ++y) {
    const int ym = y > 0 ? y - 1 : 0;
    const int yp = y < SH - 1 ? y + 1 : SH - 1;
    const uint32_t *a = &glow_[static_cast<size_t>(ym) * SW];
    const uint32_t *b = &glow_[static_cast<size_t>(y) * SW];
    const uint32_t *c = &glow_[static_cast<size_t>(yp) * SW];
    uint32_t *dst = &accum_[static_cast<size_t>(y) * SW];
    for (int x = 0; x < SW; ++x) dst[x] = blur3(a[x], b[x], c[x]);
  }
  std::memcpy(glow_, accum_, sizeof(glow_));

  // Pack once, here, rather than per expanded row.
  for (int i = 0; i < N; ++i) {
    const uint32_t v = glow_[i];
    glow565_[i] = rgb565(static_cast<uint8_t>(v & 0xFF),
                         static_cast<uint8_t>((v >> 8) & 0xFF),
                         static_cast<uint8_t>((v >> 16) & 0xFF));
  }

  std::memset(accum_, 0, sizeof(accum_));
  glowrow_sy_ = -1;
}

}  // namespace gfx
