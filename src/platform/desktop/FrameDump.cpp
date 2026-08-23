#include "FrameDump.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace desktop {
namespace {

void put16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
  v.push_back((x >> 16) & 0xff);
  v.push_back((x >> 24) & 0xff);
}

}  // namespace

bool dumpFrameBmp(const gfx::Framebuffer &fb, const char *path) {
  const int w = gfx::Framebuffer::width();
  const int h = gfx::Framebuffer::height();
  const int row_bytes = w * 3;
  const int pad = (4 - (row_bytes % 4)) % 4;
  const uint32_t image_bytes = static_cast<uint32_t>((row_bytes + pad) * h);

  std::vector<uint8_t> out;
  out.reserve(54 + image_bytes);

  out.push_back('B');
  out.push_back('M');
  put32(out, 54 + image_bytes);
  put32(out, 0);
  put32(out, 54);

  put32(out, 40);
  put32(out, static_cast<uint32_t>(w));
  put32(out, static_cast<uint32_t>(h));
  put16(out, 1);
  put16(out, 24);
  put32(out, 0);
  put32(out, image_bytes);
  put32(out, 2835);
  put32(out, 2835);
  put32(out, 0);
  put32(out, 0);

  // BMP rows run bottom-up and pixels are BGR. The framebuffer is
  // native-endian RGB565, so channels unpack directly with no swap - unlike the
  // ancestor project, whose equivalent function had to swap first.
  const uint16_t *px = fb.pixels();
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) {
      const uint16_t c = px[static_cast<size_t>(y) * w + x];
      const uint8_t r5 = (c >> 11) & 0x1f;
      const uint8_t g6 = (c >> 5) & 0x3f;
      const uint8_t b5 = c & 0x1f;
      out.push_back(static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
      out.push_back(static_cast<uint8_t>((g6 << 2) | (g6 >> 4)));
      out.push_back(static_cast<uint8_t>((r5 << 3) | (r5 >> 2)));
    }
    for (int p = 0; p < pad; ++p) out.push_back(0);
  }

  FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  const size_t n = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return n == out.size();
}

}  // namespace desktop
