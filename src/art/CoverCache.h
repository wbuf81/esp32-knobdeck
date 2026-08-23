#pragma once

// Album artwork, decoded and held in PSRAM.
//
// The drop-in replacement for the ancestor's SD-card ArtCache. Its interface is
// kept deliberately - cachedPath / failed / ensure - because the logic in
// SpotifySource around those three is well-tuned and worth not disturbing: it
// defers the download until after the new title is on screen, and it remembers
// which album already refused so it does not re-queue the same fetch every two
// seconds and strobe the artwork region.
//
// What changed is where a cover lives. The ancestor needed a FAT32 card because
// it had no PSRAM to hold one; here the JPEG streams into PSRAM, is decoded into
// PSRAM, and the SD slot becomes optional hardware.
//
// THREADING: ensure() runs on the net task, image() on the render task. Slots are
// written round-robin and only the published index is ever read, so the slot
// being decoded into is never the slot being drawn. A slot is not reused until
// two further downloads have happened - album changes are minutes apart and a
// frame is tens of milliseconds, so nothing can be mid-read.

#include <atomic>
#include <string>

#include "Image.h"

namespace art {

class CoverCache {
 public:
  static constexpr int SLOTS = 3;
  // Spotify's 300px covers run 20-40 KB. The cap is generous enough for the
  // 640px variant and small enough that a wrong URL cannot eat PSRAM.
  static constexpr size_t MAX_JPEG_BYTES = 400 * 1024;
  // Decoded size ceiling. The cover draws about 100 px tall, so 320 leaves
  // plenty of detail for a tilt toward the camera without paying for pixels
  // nothing can show.
  static constexpr int MAX_DIM = 320;

  // Net task. Returns the album id on success and an empty string on failure -
  // the caller treats that as "no artwork", which must render deliberately.
  std::string ensure(const std::string &album_id, const std::string &url);

  // Net task. Non-empty if this album's cover is already decoded and published.
  // Never touches the network.
  std::string cachedPath(const std::string &album_id) const;

  // Net task. True once this album has failed, so the poll path stops
  // re-queueing it every two seconds.
  bool failed(const std::string &album_id) const;

  // Render task. The published cover if it is this album's, else null.
  const Image *image(const char *album_id) const;

 private:
  struct Slot {
    Image img;
    char album[48] = {};
  };

  Slot slots_[SLOTS];
  std::atomic<int> published_{-1};
  int write_idx_ = 0;
  std::string failed_album_;
};

}  // namespace art
