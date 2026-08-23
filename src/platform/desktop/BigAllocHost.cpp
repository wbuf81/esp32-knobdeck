#if !defined(DEVICE)

#include <cstdlib>

#include "core/BigAlloc.h"

namespace core {

void *bigAlloc(size_t bytes) { return std::malloc(bytes); }
void bigFree(void *p) { std::free(p); }

}  // namespace core

#endif
