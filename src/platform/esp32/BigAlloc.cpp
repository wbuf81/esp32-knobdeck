#if defined(DEVICE)

#include <esp_heap_caps.h>

#include "core/BigAlloc.h"

namespace core {

void *bigAlloc(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void bigFree(void *p) {
  if (p) heap_caps_free(p);
}

}  // namespace core

#endif
