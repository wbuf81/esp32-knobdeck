#pragma once

// JPEG decode, from memory.
//
// From memory rather than from a file because this board does not need a
// filesystem for artwork. The ancestor listed a FAT32 SD card as required
// hardware purely because it had no PSRAM to hold a cover; with 8 MB the JPEG
// lands in PSRAM, is decoded into PSRAM, and the card becomes optional.
//
// On the device this is the ESP32-S3's ROM decoder, so it costs no flash at all.

#include <cstddef>
#include <cstdint>

#include "Image.h"

namespace art {

// Decodes into `out`, downscaling by whole powers of two so the longer side is
// at most `max_dim`. Returns false and leaves `out` invalid on any failure -
// never half-populated, because a partly decoded cover on screen looks like a
// rendering fault rather than a download problem.
bool decodeJpeg(const uint8_t *data, size_t len, int max_dim, Image *out);

}  // namespace art
