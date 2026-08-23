// Image allocation on the device: PSRAM, explicitly.
//
// A cover is up to 259 KB and internal SRAM's largest free block after the
// renderer's band and bloom buffers is about 61 KB, so this is not a preference.
// Falling back to internal on failure would succeed for a small cover and then
// starve mbedTLS, so it does not fall back - a cover that will not fit should
// fail visibly as "no artwork", which the view already renders deliberately.

#if defined(DEVICE)

#include <esp_heap_caps.h>

#include "art/Image.h"

namespace art {

void *imageAlloc(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void imageFree(void *p) {
  if (p) heap_caps_free(p);
}

}  // namespace art

#endif
