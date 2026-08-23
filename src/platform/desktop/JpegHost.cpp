// JPEG decode on the desktop.
//
// Not implemented yet, and deliberately explicit about it rather than silently
// returning an empty image: the desktop build draws a synthetic cover, so this
// path is only reached if something asks it to decode real artwork. When the
// desktop is wired to live Spotify - worth doing, it is how the ancestor tuned
// its optimistic-UI timing without a board - this is where stb_image goes.

#if !defined(DEVICE)

#include <cstdio>

#include "art/Jpeg.h"

namespace art {

bool decodeJpeg(const uint8_t *data, size_t len, int max_dim, Image *out) {
  (void)data;
  (void)len;
  (void)max_dim;
  if (out) out->release();
  static bool warned = false;
  if (!warned) {
    warned = true;
    std::fprintf(stderr,
                 "decodeJpeg: not implemented on the desktop build; the "
                 "synthetic cover is used instead\n");
  }
  return false;
}

}  // namespace art

#endif
