#pragma once

// Bloom: the cheapest way to make drawn things look lit.
//
// bright-pass -> 4x downscale to 90x90 -> N separable blur passes -> bilinear
// upscale, added back saturating. Running the blur at 1/16 the area is what
// makes it affordable; at this radius nobody can tell it was low resolution.
//
// The scratch buffers hold interleaved 8-bit RGB, NOT RGB565. That is not an
// arbitrary choice: an earlier version blurred in RGB565 and quantised twice
// per pass, which crushed the soft falloff to black within three passes. The
// falloff *is* the bloom - a glow with a hard edge reads as a mistake - so the
// pipeline keeps 8-bit precision until the final add.
//
// Scratch buffers are allocated once here and never per frame. On the device
// this file's allocation pattern is the difference between working and a
// fragmentation reboot loop ten minutes in.

#include <cstdint>
#include <vector>

#include "Framebuffer.h"

namespace gfx {

class Bloom {
 public:
  static constexpr int SMALL_W = 90;
  static constexpr int SMALL_H = 90;
  static constexpr int SCALE = W / SMALL_W;  // 4
  // Each pass widens the glow by one small pixel, which is SCALE screen pixels.
  static constexpr int PASSES = 3;

  Bloom();

  // threshold: 8-bit luma floor for contributing to the glow.
  // strength:  0..255 scale on the added glow.
  void apply(Framebuffer &fb, uint8_t threshold, uint8_t strength);

 private:
  void brightPassDownscale(const Framebuffer &fb, uint8_t threshold);
  void blurPass();
  void upscaleAdd(Framebuffer &fb, uint8_t strength);

  // Interleaved RGB888, SMALL_W * SMALL_H * 3.
  std::vector<uint8_t> small_;
  std::vector<uint8_t> tmp_;
};

}  // namespace gfx
