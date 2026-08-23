#include "Image.h"

#include <cstring>

namespace art {

bool Image::allocate(int width, int height) {
  release();
  if (width <= 0 || height <= 0) return false;
  const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint16_t);
  px_ = static_cast<uint16_t *>(core::bigAlloc(bytes));
  if (!px_) return false;
  std::memset(px_, 0, bytes);
  w_ = width;
  h_ = height;
  return true;
}

void Image::release() {
  if (px_) core::bigFree(px_);
  px_ = nullptr;
  w_ = 0;
  h_ = 0;
}

}  // namespace art
