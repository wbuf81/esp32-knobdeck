#include "CoverCache.h"

#include <cstring>
#include <vector>

#include "Jpeg.h"
#include "net/HttpClient.h"
#include "net/NetLog.h"

namespace art {

std::string CoverCache::cachedPath(const std::string &album_id) const {
  if (album_id.empty()) return std::string();
  const int p = published_.load();
  if (p < 0) return std::string();
  if (std::strncmp(slots_[p].album, album_id.c_str(), sizeof(slots_[p].album) - 1) != 0)
    return std::string();
  return slots_[p].img.valid() ? album_id : std::string();
}

bool CoverCache::failed(const std::string &album_id) const {
  return !album_id.empty() && failed_album_ == album_id;
}

const Image *CoverCache::image(const char *album_id) const {
  if (!album_id || !album_id[0]) return nullptr;
  const int p = published_.load();
  if (p < 0) return nullptr;
  const Slot &s = slots_[p];
  if (!s.img.valid()) return nullptr;
  if (std::strncmp(s.album, album_id, sizeof(s.album) - 1) != 0) return nullptr;
  return &s.img;
}

std::string CoverCache::ensure(const std::string &album_id,
                               const std::string &url) {
  if (album_id.empty() || url.empty()) return std::string();

  const std::string have = cachedPath(album_id);
  if (!have.empty()) return have;

  std::vector<uint8_t> jpeg;
  if (!http::downloadToMemory(url, &jpeg, MAX_JPEG_BYTES) || jpeg.empty()) {
    failed_album_ = album_id;
    return std::string();
  }

  // Decoded into the slot AFTER the published one, so the slot being written is
  // never the slot being drawn.
  const int slot = write_idx_;
  Slot &s = slots_[slot];
  if (!decodeJpeg(jpeg.data(), jpeg.size(), MAX_DIM, &s.img)) {
    NETLOG("artwork decode failed (%u bytes)", (unsigned)jpeg.size());
    failed_album_ = album_id;
    return std::string();
  }

  std::strncpy(s.album, album_id.c_str(), sizeof(s.album) - 1);
  s.album[sizeof(s.album) - 1] = '\0';
  // Publish last: until this store, the render task cannot see the new slot at
  // all, so there is no window where it could read a half-decoded cover.
  published_.store(slot);
  write_idx_ = (write_idx_ + 1) % SLOTS;

  NETLOG("artwork decoded %dx%d into slot %d", s.img.width(), s.img.height(),
         slot);
  if (failed_album_ == album_id) failed_album_.clear();
  return album_id;
}

}  // namespace art
