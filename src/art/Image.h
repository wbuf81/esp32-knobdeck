#pragma once

// A decoded cover, native-endian RGB565.
//
// Allocated explicitly rather than through std::vector, because on the device it
// MUST land in PSRAM: a 192x192 cover is 73 KB and internal SRAM's largest free
// block after the renderer's buffers is about 61 KB. Quad3D reads it once per
// rasterised pixel and never writes it, and PSRAM reads measured 56 MB/s - fine
// for a texture fetch, and the one large buffer this design can afford to put
// there.

#include <cstddef>
#include <cstdint>

namespace art {

// Platform-provided. Device returns PSRAM; desktop returns plain heap.
void *imageAlloc(size_t bytes);
void imageFree(void *p);

class Image {
 public:
  Image() = default;
  ~Image() { release(); }
  Image(const Image &) = delete;
  Image &operator=(const Image &) = delete;

  bool allocate(int width, int height);
  void release();

  bool valid() const { return px_ != nullptr && w_ > 0 && h_ > 0; }
  int width() const { return w_; }
  int height() const { return h_; }
  uint16_t *pixels() { return px_; }
  const uint16_t *pixels() const { return px_; }

  // Edge-clamped read. Quad3D's bilinear fetch relies on the clamp rather than
  // bounds-checking itself.
  uint16_t at(int x, int y) const {
    if (!px_) return 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w_) x = w_ - 1;
    if (y >= h_) y = h_ - 1;
    return px_[static_cast<size_t>(y) * w_ + x];
  }

  void set(int x, int y, uint16_t c) {
    if (!px_ || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    px_[static_cast<size_t>(y) * w_ + x] = c;
  }

 private:
  uint16_t *px_ = nullptr;
  int w_ = 0;
  int h_ = 0;
};

// Platform-provided decoder. Desktop uses stb_image; the device will stream
// through TJpgDec off SD. Same signature, so nothing above this layer cares.
bool loadJpeg(const char *path, int max_dim, Image *out);

// A synthetic cover, for development and for the tests. Deterministic on `seed`.
void makePlaceholderCover(uint32_t seed, int size, Image *out);

}  // namespace art
