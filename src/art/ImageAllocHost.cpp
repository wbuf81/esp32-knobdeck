// Plain-heap image allocation, for the desktop build and the host tests.
//
// The device provides its own, returning PSRAM. Kept in separate translation
// units rather than behind an #if so neither build can accidentally compile the
// other's allocator.

#if !defined(DEVICE)

#include <cstdlib>

#include "Image.h"

namespace art {

void *imageAlloc(size_t bytes) { return std::malloc(bytes); }
void imageFree(void *p) { std::free(p); }

}  // namespace art

#endif
