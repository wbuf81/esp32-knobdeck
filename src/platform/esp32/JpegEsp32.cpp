// JPEG decode on the device, using the ESP32-S3's ROM decoder.
//
// In ROM, so it costs zero flash and needs no library. TJpgDec is baseline-only;
// Spotify's CDN serves baseline JPEG, and a progressive one fails cleanly here
// rather than producing garbage.

#if defined(DEVICE)

#include <esp_heap_caps.h>
#include <esp_rom_tjpgd.h>

#include <cstring>

#include "art/Jpeg.h"
#include "gfx/Color.h"

namespace art {
namespace {

// TJpgDec needs a little over 3 KB of scratch for a baseline image. Taken from
// internal SRAM: it is touched constantly during a decode, and the decode is
// already reading its input from PSRAM.
constexpr size_t WORK_BYTES = 4096;

struct Ctx {
  const uint8_t *data;
  size_t len;
  size_t pos;
  Image *out;
};

uint32_t inFunc(esp_rom_tjpgd_dec_t *dec, uint8_t *buf, uint32_t n) {
  Ctx *c = static_cast<Ctx *>(dec->device);
  const size_t left = c->len - c->pos;
  const size_t take = n > left ? left : n;
  // A null buffer means "skip", which is how the decoder steps over segments it
  // does not need.
  if (buf && take) std::memcpy(buf, c->data + c->pos, take);
  c->pos += take;
  return static_cast<uint32_t>(take);
}

uint32_t outFunc(esp_rom_tjpgd_dec_t *dec, void *bitmap,
                 esp_rom_tjpgd_rect_t *rect) {
  Ctx *c = static_cast<Ctx *>(dec->device);
  const uint8_t *src = static_cast<const uint8_t *>(bitmap);
  // The ROM decoder emits RGB888.
  for (int y = rect->top; y <= rect->bottom; ++y) {
    for (int x = rect->left; x <= rect->right; ++x) {
      c->out->set(x, y, gfx::rgb565(src[0], src[1], src[2]));
      src += 3;
    }
  }
  return 1;
}

}  // namespace

bool decodeJpeg(const uint8_t *data, size_t len, int max_dim, Image *out) {
  if (!data || !len || !out || max_dim <= 0) return false;
  out->release();

  void *work = heap_caps_malloc(WORK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!work) return false;

  Ctx ctx{data, len, 0, out};
  esp_rom_tjpgd_dec_t dec;
  if (esp_rom_tjpgd_prepare(&dec, inFunc, work, WORK_BYTES, &ctx) != JDR_OK) {
    heap_caps_free(work);
    return false;
  }

  // Whole powers of two only - that is all TJpgDec offers - so pick the
  // smallest reduction that fits. Decoding larger than needed costs PSRAM and
  // texture-fetch bandwidth for detail the quad cannot show.
  uint8_t scale = 0;
  const uint32_t longest = dec.width > dec.height ? dec.width : dec.height;
  while (scale < 3 && (longest >> scale) > static_cast<uint32_t>(max_dim)) ++scale;

  const int w = static_cast<int>(dec.width >> scale);
  const int h = static_cast<int>(dec.height >> scale);
  if (w <= 0 || h <= 0 || !out->allocate(w, h)) {
    heap_caps_free(work);
    return false;
  }

  const esp_rom_tjpgd_result_t r = esp_rom_tjpgd_decomp(&dec, outFunc, scale);
  heap_caps_free(work);

  if (r != JDR_OK) {
    // Released rather than left partly decoded: half a cover on screen reads as
    // a rendering fault, where no cover reads as a download that did not finish.
    out->release();
    return false;
  }
  return true;
}

}  // namespace art

#endif  // DEVICE
