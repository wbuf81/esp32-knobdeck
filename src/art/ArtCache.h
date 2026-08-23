#pragma once

// Album artwork on local storage, keyed by album ID.
//
// A cache hit means a track change repaints with no network access at all. On
// the device this directory lives on the SD card; in the emulator it is a local
// folder. Capped at a few hundred covers, oldest evicted at boot — not because
// the card would fill (half a million covers would), but so the directory scan
// and the cache stay bounded on a device that runs for years.

#include <string>

class ArtCache {
 public:
  void begin(const std::string &dir);

  // Returns a local path to decodable artwork, downloading it if absent.
  // Empty string means unavailable — the caller must render a flat block rather
  // than failing.
  std::string ensure(const std::string &album_id, const std::string &url);

  // Path if already cached, empty otherwise. Never touches the network, so the
  // poll path can use it without paying for a download.
  std::string cachedPath(const std::string &album_id) const;

  // True once this album's cover has failed and will not be retried until the
  // album changes. The poll path has to ask, because otherwise it re-queues the
  // same download every two seconds: ensure() refuses it instantly, art_loading
  // goes true then false, and the screen strobes between the "fetching cover"
  // placeholder and "no artwork" instead of settling on the honest one.
  bool failed(const std::string &album_id) const;

 private:
  std::string dir_;

  // Without this, a failing album is retried on every poll — once every two
  // seconds, forever. Cleared on album change, so a transient failure still
  // gets another go on the next track.
  std::string failed_album_;
};
