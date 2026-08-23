#include "Library.h"

#include <cstring>

#include "core/BigAlloc.h"

namespace spotify {
namespace {

void copyField(char *dst, size_t cap, const char *src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  std::strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

}  // namespace

bool Library::begin() {
  if (playlists_ && tracks_) return true;
  playlists_ = static_cast<Entry *>(core::bigAlloc(sizeof(Entry) * MAX_PLAYLISTS));
  tracks_ = static_cast<Entry *>(core::bigAlloc(sizeof(Entry) * MAX_TRACKS));
  if (!playlists_ || !tracks_) {
    core::bigFree(playlists_);
    core::bigFree(tracks_);
    playlists_ = nullptr;
    tracks_ = nullptr;
    return false;
  }
  std::memset(playlists_, 0, sizeof(Entry) * MAX_PLAYLISTS);
  std::memset(tracks_, 0, sizeof(Entry) * MAX_TRACKS);
  return true;
}

const Entry *Library::playlist(int i) const {
  if (!playlists_ || i < 0 || i >= playlist_count_) return nullptr;
  return &playlists_[i];
}

const Entry *Library::track(int i) const {
  if (!tracks_ || i < 0 || i >= track_count_) return nullptr;
  return &tracks_[i];
}

void Library::clearPlaylists() {
  fill_ = 0;
  playlists_truncated_ = false;
}

bool Library::addPlaylist(const char *name, const char *uri, const char *id) {
  if (!playlists_) return false;
  if (fill_ >= MAX_PLAYLISTS) {
    playlists_truncated_ = true;
    return false;
  }
  Entry &e = playlists_[fill_++];
  copyField(e.name, sizeof(e.name), name);
  copyField(e.uri, sizeof(e.uri), uri);
  copyField(e.id, sizeof(e.id), id);
  return true;
}

void Library::publishPlaylists(bool truncated) {
  // Count last, then bump the generation. The renderer reads count only after
  // seeing a new generation, so it can never walk into entries still being
  // written.
  playlist_count_ = fill_;
  if (truncated) playlists_truncated_ = true;
  generation_.fetch_add(1);
}

void Library::clearTracks(const char *playlist_uri, const char *playlist_name) {
  fill_ = 0;
  tracks_truncated_ = false;
  tracks_error_ = 0;
  // The count is zeroed and published BEFORE the fetch, so the list a stale
  // frame draws is empty rather than the previous playlist's tracks under this
  // playlist's title.
  track_count_ = 0;
  copyField(tracks_of_uri_, sizeof(tracks_of_uri_), playlist_uri);
  copyField(tracks_of_name_, sizeof(tracks_of_name_), playlist_name);
  generation_.fetch_add(1);
}

bool Library::addTrack(const char *name, const char *uri, const char *id) {
  if (!tracks_) return false;
  if (fill_ >= MAX_TRACKS) {
    tracks_truncated_ = true;
    return false;
  }
  Entry &e = tracks_[fill_++];
  copyField(e.name, sizeof(e.name), name);
  copyField(e.uri, sizeof(e.uri), uri);
  copyField(e.id, sizeof(e.id), id);
  return true;
}

void Library::publishTracks(bool truncated) {
  track_count_ = fill_;
  if (truncated) tracks_truncated_ = true;
  generation_.fetch_add(1);
}

}  // namespace spotify
