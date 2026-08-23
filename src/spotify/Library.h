#pragma once

// Playlists and their tracks, as flat lists in PSRAM.
//
// Written by the net task, read by the render task. Publication is the same
// pattern as the cover cache: fill a buffer that nothing is reading, then bump a
// generation counter under an atomic. The renderer reads the generation once per
// frame and only re-lays-out its list when it changes.
//
// Bounded on purpose. A listing that grows with the account would eventually be
// the thing that exhausts PSRAM on a device meant to run for months, and nobody
// scrolls past forty items on a 1.8-inch dial anyway. When the cap is hit it
// says so rather than silently showing a prefix.

#include <atomic>
#include <cstdint>

namespace spotify {

struct Entry {
  char name[52] = {};
  char uri[52] = {};  // spotify:playlist:... or spotify:track:...
  char id[24] = {};   // bare id, for building further URLs
};

class Library {
 public:
  static constexpr int MAX_PLAYLISTS = 32;
  static constexpr int MAX_TRACKS = 60;

  // Allocates the lists in PSRAM. False means the device is out of large
  // memory, and the browser should stay unavailable rather than half-work.
  bool begin();

  // --- net task ---
  void clearPlaylists();
  bool addPlaylist(const char *name, const char *uri, const char *id);
  void publishPlaylists(bool truncated);

  void clearTracks(const char *playlist_uri, const char *playlist_name);
  bool addTrack(const char *name, const char *uri, const char *id);
  void publishTracks(bool truncated);

  // --- render task ---
  uint32_t generation() const { return generation_.load(); }
  int playlistCount() const { return playlist_count_; }
  int trackCount() const { return track_count_; }
  const Entry *playlist(int i) const;
  const Entry *track(int i) const;
  const char *tracksOf() const { return tracks_of_name_; }
  const char *tracksUri() const { return tracks_of_uri_; }
  bool playlistsTruncated() const { return playlists_truncated_; }
  bool tracksTruncated() const { return tracks_truncated_; }

 private:
  Entry *playlists_ = nullptr;
  Entry *tracks_ = nullptr;
  int playlist_count_ = 0;
  int track_count_ = 0;
  int fill_ = 0;  // how many entries the net task has written so far
  bool playlists_truncated_ = false;
  bool tracks_truncated_ = false;
  char tracks_of_uri_[52] = {};
  char tracks_of_name_[52] = {};
  std::atomic<uint32_t> generation_{0};
};

}  // namespace spotify
