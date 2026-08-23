#include "ArtCache.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>

#include "../net/HttpClient.h"
#include "../net/NetLog.h"
#include "../platform/esp32/Esp32Storage.h"

namespace {

bool fileExists(const std::string &path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

// Album IDs are base62 from Spotify, but this is going into a filesystem path,
// so refuse anything that is not plainly safe rather than trusting the input.
bool safeId(const std::string &id) {
  if (id.empty() || id.size() > 40) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// Covers accumulate one per album forever, so a long-lived device slowly
// fills its card with art nobody will look at again. Capped by deleting the
// oldest files (FAT mtime) once the count crosses the limit. Runs at boot
// only: a scan of a few hundred dirents costs milliseconds, and bounding it
// per-download would mean paying that scan on every album change instead.
constexpr size_t CACHE_MAX_FILES = 300;

void evictOldest(const std::string &dir) {
  DIR *d = ::opendir(dir.c_str());
  if (!d) return;

  struct Entry {
    time_t mtime;
    char name[64];
  };
  // Two passes would need the file list in memory anyway; one pass with a
  // fixed-size table keeps the worst case bounded.
  static Entry entries[CACHE_MAX_FILES + 64];
  size_t n = 0;

  struct dirent *e;
  while ((e = ::readdir(d)) != nullptr && n < CACHE_MAX_FILES + 64) {
    if (e->d_name[0] == '.') continue;
    if (!std::strstr(e->d_name, ".jpg")) continue;
    struct stat st;
    const std::string path = dir + "/" + e->d_name;
    if (::stat(path.c_str(), &st) != 0) continue;
    entries[n].mtime = st.st_mtime;
    std::strncpy(entries[n].name, e->d_name, sizeof(entries[n].name) - 1);
    entries[n].name[sizeof(entries[n].name) - 1] = '\0';
    ++n;
  }
  ::closedir(d);

  if (n <= CACHE_MAX_FILES) return;

  // Selection sort on the oldest few: n is small and this runs once per boot.
  const size_t excess = n - CACHE_MAX_FILES;
  for (size_t k = 0; k < excess; ++k) {
    size_t oldest = k;
    for (size_t i = k + 1; i < n; ++i) {
      if (entries[i].mtime < entries[oldest].mtime) oldest = i;
    }
    const Entry tmp = entries[k];
    entries[k] = entries[oldest];
    entries[oldest] = tmp;
    const std::string victim = dir + "/" + entries[k].name;
    std::remove(victim.c_str());
  }
  NETLOG("art cache: evicted %zu of %zu covers", excess, n);
}

}  // namespace

void ArtCache::begin(const std::string &dir) {
  dir_ = dir;
  // Create each path component. A single mkdir of ".cache/art" fails with
  // ENOENT because ".cache" does not exist yet, and every later download would
  // then fail with no obvious cause.
  std::string partial;
  for (size_t i = 0; i <= dir_.size(); ++i) {
    if (i == dir_.size() || dir_[i] == '/') {
      if (!partial.empty()) ::mkdir(partial.c_str(), 0755);
    }
    if (i < dir_.size()) partial += dir_[i];
  }
  evictOldest(dir_);
}

std::string ArtCache::cachedPath(const std::string &album_id) const {
  if (!safeId(album_id) || dir_.empty()) return "";
  const std::string path = dir_ + "/" + album_id + ".jpg";
  return fileExists(path) ? path : "";
}

bool ArtCache::failed(const std::string &album_id) const {
  return !album_id.empty() && failed_album_ == album_id;
}

std::string ArtCache::ensure(const std::string &album_id,
                             const std::string &url) {
  if (!safeId(album_id) || url.empty()) return "";

  const std::string path = dir_ + "/" + album_id + ".jpg";
  if (fileExists(path)) return path;

  // No usable storage: never even open the connection. There is nowhere to put
  // the result, and attempting anyway is a request per poll for as long as the
  // device runs.
  if (!storageAvailable()) return "";

  // Already failed for this album — do not retry until the track changes.
  if (failed_album_ == album_id) return "";

  if (!http::downloadToFile(url, path) || !fileExists(path)) {
    NETLOG("artwork unavailable for %s; not retrying until the album changes",
           album_id.c_str());
    failed_album_ = album_id;
    return "";
  }
  failed_album_.clear();
  return path;
}
