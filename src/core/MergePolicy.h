#pragma once

// Rules for folding a source's freshly polled state into the shared UI state.
//
// Pulled out of NetWorker so it can be unit tested on the host: the settle
// windows and the liked-known handling are subtle, easy to break, and produce
// symptoms (a pause icon snapping back, a heart flickering) that are miserable
// to diagnose by eye.
//
// Display-free and thread-free by design. The caller holds whatever lock is
// needed.

#include <cstring>

#include "AppState.h"

inline void mergePlayback(AppState *dst, const AppState &src, bool polled,
                          uint32_t now_ms) {
  dst->link = src.link;

  // A toast from the source wins only if it outlives whatever is already
  // showing, so a stale message cannot clobber a fresher one. Compared by time
  // remaining rather than by absolute expiry, which would invert at the wrap.
  if (src.toast_for.remainingMs(now_ms) > dst->toast_for.remainingMs(now_ms)) {
    setStr(dst->toast, sizeof(dst->toast), src.toast);
    dst->toast_for = src.toast_for;
  }

  // Without a fresh poll the source's copy is a stale snapshot; merging it
  // would undo the UI's own extrapolation.
  if (!polled) return;

  const bool track_changed = std::strcmp(dst->pb.track_id, src.pb.track_id) != 0;

  setStr(dst->pb.track_id, ID_LEN, src.pb.track_id);
  setStr(dst->pb.album_id, ID_LEN, src.pb.album_id);
  setStr(dst->pb.title, TEXT_LEN, src.pb.title);
  setStr(dst->pb.artist, TEXT_LEN, src.pb.artist);
  setStr(dst->pb.art_path, PATH_LEN, src.pb.art_path);
  dst->pb.art_loading = src.pb.art_loading;
  dst->pb.has_track = src.pb.has_track;
  dst->pb.has_device = src.pb.has_device;
  dst->pb.duration_ms = src.pb.duration_ms;
  dst->pb.progress_ms = src.pb.progress_ms;

  // Settle windows: a field the user just changed optimistically is left alone
  // until the window expires, so a response already in flight cannot snap it
  // back and make the device look broken.
  if (dst->settle_playing.elapsed(now_ms)) {
    dst->pb.is_playing = src.pb.is_playing;
  }
  if (dst->settle_volume.elapsed(now_ms)) {
    dst->pb.volume_pct = src.pb.volume_pct;
  }
  if (dst->settle_liked.elapsed(now_ms)) {
    dst->pb.liked = src.pb.liked;
  }

  // Never downgrade known -> unknown for the same track. When the API cannot
  // report saved-state, the user's own like this session is the only truth we
  // have and it must not evaporate when the settle window closes.
  if (track_changed || src.pb.liked_known) {
    dst->pb.liked_known = src.pb.liked_known;
  }

  ++dst->publish_seq;
}
