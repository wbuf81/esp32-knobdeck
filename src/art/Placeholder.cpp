#include <cmath>

#include "Image.h"
#include "gfx/Color.h"
#include "gfx/Hsv.h"

namespace art {

// A synthetic cover, so the 3D path can be developed and tested before any
// artwork exists. Deliberately asymmetric in both axes and in colour: a
// symmetric test image cannot catch a flipped or transposed UV mapping, which is
// the failure this most needs to detect.
void makePlaceholderCover(uint32_t seed, int size, Image *out) {
  if (!out || size <= 0) return;
  if (!out->allocate(size, size)) return;

  const float hue = static_cast<float>(seed % 997u) / 997.0f;
  const float inv = 1.0f / static_cast<float>(size - 1);

  for (int y = 0; y < size; ++y) {
    const float fy = static_cast<float>(y) * inv;
    for (int x = 0; x < size; ++x) {
      const float fx = static_cast<float>(x) * inv;

      // Diagonal bands plus a corner marker, so orientation is unambiguous.
      const float band = std::sin((fx * 3.0f + fy * 5.0f) * 6.2831853f);
      float v = 0.34f + 0.30f * (0.5f + 0.5f * band);
      float s = 0.80f;
      float h = hue + 0.10f * fy;

      // A soft bright corner in the top-left and a soft dark one bottom-right.
      // Asymmetric in both axes so a flipped or transposed UV mapping cannot
      // pass by symmetry - which is what this image exists to catch - but
      // gradual rather than a hard wedge, because a hard-edged marker reads as a
      // rendering fault when you are looking at the screen rather than at a
      // test.
      const float corner = 1.0f - (fx + fy) * 0.5f;  // 1 at TL, 0 at BR
      v = v * (0.55f + 0.60f * corner);
      s = s * (0.55f + 0.45f * (1.0f - corner));

      out->set(x, y, gfx::hsv565(h, s, v));
    }
  }
}

}  // namespace art
