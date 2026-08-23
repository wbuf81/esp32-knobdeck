#include "Framebuffer.h"

namespace gfx {

void Framebuffer::fill(uint16_t c) {
  uint16_t *p = px_.data();
  const size_t n = count();
  for (size_t i = 0; i < n; ++i) p[i] = c;
}

}  // namespace gfx
