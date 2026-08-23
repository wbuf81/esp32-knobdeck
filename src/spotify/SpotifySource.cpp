#include "SpotifySource.h"

// How large a cover to ask Spotify for.
//
// The ancestor read this from its UI theme header, which coupled the Spotify
// client to the layout. It is stated here instead: the cover is drawn as a 3D
// quad about 100 px tall, and 192 gives enough texture to stay sharp when the
// quad tilts toward the camera without paying for pixels no one can see.
static constexpr int ART_TARGET_PX = 192;

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>

#include "../net/HttpClient.h"
#include "../core/Clock.h"
#include "../net/NetLog.h"

namespace {

constexpr const char *API = "https://api.spotify.com/v1";

// Poll cadence from the design: 2s playing, 5s paused, 20s once asleep. The
// asleep case is driven by the UI thread lowering the rate, so this covers the
// first two.
constexpr uint32_t POLL_PLAYING_MS = 2000;
constexpr uint32_t POLL_PAUSED_MS = 5000;
constexpr uint32_t POLL_ASLEEP_MS = 20000;

// Builds the filter that keeps the /me/player response from exhausting heap.
// The raw payload carries available_markets arrays with hundreds of country
// codes on both the track and the album; parsing it unfiltered is the single
// most likely way to hard-reset the device.
void buildPlayerFilter(JsonDocument *filter) {
  (*filter)["is_playing"] = true;
  (*filter)["progress_ms"] = true;
  (*filter)["device"]["name"] = true;
  (*filter)["device"]["volume_percent"] = true;
  (*filter)["item"]["id"] = true;
  (*filter)["item"]["name"] = true;
  (*filter)["item"]["duration_ms"] = true;
  (*filter)["item"]["artists"][0]["name"] = true;
  (*filter)["item"]["album"]["id"] = true;
  (*filter)["item"]["album"]["images"][0]["url"] = true;
  (*filter)["item"]["album"]["images"][0]["width"] = true;
}

// Spotify serves 640 / 300 / 64. Take the smallest that still covers the
// artwork region, so we neither upscale nor download a 640 we will discard.
const char *pickImage(JsonArrayConst images, int want) {
  const char *best = nullptr;
  int best_w = 1 << 30;
  const char *largest = nullptr;
  int largest_w = -1;

  for (JsonObjectConst img : images) {
    const char *url = img["url"];
    if (!url) continue;
    const int w = img["width"] | 0;
    if (w > largest_w) {
      largest_w = w;
      largest = url;
    }
    if (w >= want && w < best_w) {
      best_w = w;
      best = url;
    }
  }
  return best ? best : largest;
}

}  // namespace

SpotifySource::SpotifySource(const char *client_id, const char *client_secret,
                             const char *refresh_token)
    : auth_(client_id, client_secret, refresh_token) {}

void SpotifySource::begin(const std::string &cache_dir) {
  art_.begin(cache_dir);
}

bool SpotifySource::authHeaders(std::vector<std::string> *headers,
                                uint32_t now_ms) {
  if (!auth_.ensureFresh(now_ms)) return false;
  headers->clear();
  headers->push_back("Authorization: Bearer " + auth_.token());
  return true;
}

bool SpotifySource::call(const char *method, const std::string &url,
                         const std::string &body, HttpResponse *resp,
                         AppState *out, uint32_t now_ms) {
  std::vector<std::string> headers;
  if (!authHeaders(&headers, now_ms)) {
    NETLOG("%s %s: no valid token (link=%d)", method, url.c_str(),
           static_cast<int>(auth_.status()));
    out->link = auth_.status();
    return false;
  }

  if (!http::request(method, url, headers, body, resp)) {
    NETLOG("%s %s: transport failure", method, url.c_str());
    out->link = LinkStatus::Offline;
    return false;
  }
  NETLOG("%s %s -> %d", method, url.c_str(), resp->status);
  if (resp->status >= 400) {
    // Spotify's error bodies name the missing scope or restriction outright.
    NETLOG("  body: %.300s", resp->body.c_str());
  }

  if (resp->status == 401) {
    // Token rejected despite our expiry maths. Refresh once and retry.
    auth_.invalidate();
    if (!authHeaders(&headers, now_ms)) {
      out->link = auth_.status();
      return false;
    }
    if (!http::request(method, url, headers, body, resp)) {
      out->link = LinkStatus::Offline;
      return false;
    }
  }

  if (resp->status == 429) {
    const long wait = resp->retry_after_s > 0 ? resp->retry_after_s : 5;
    rate_limited_.arm(now_ms, static_cast<uint32_t>(wait) * 1000);
    out->showToast("Rate limited", now_ms, 3000);
    return false;
  }

  out->link = LinkStatus::Online;
  return true;
}

void SpotifySource::runCommand(const Command &c, AppState *out,
                               uint32_t now_ms) {
  HttpResponse resp;
  std::string url;
  const char *method = "PUT";
  std::string body;

  switch (c.type) {
    case CommandType::PlayPause:
      // The optimistic local flip already happened, so send whichever verb
      // matches the state the user asked for.
      url = std::string(API) + (out->pb.is_playing ? "/me/player/play"
                                                   : "/me/player/pause");
      method = "PUT";
      break;
    case CommandType::Next:
      url = std::string(API) + "/me/player/next";
      method = "POST";
      break;
    case CommandType::Previous:
      url = std::string(API) + "/me/player/previous";
      method = "POST";
      break;
    case CommandType::SetVolume: {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%s/me/player/volume?volume_percent=%d",
                    API, c.arg);
      url = buf;
      method = "PUT";
      break;
    }
    case CommandType::ToggleLike: {
      if (out->pb.track_id[0] == '\0') return;
      // Spotify's February 2026 Dev Mode changes replaced the per-type save
      // endpoints with a generic /me/library taking Spotify URIs. The old
      // PUT /me/tracks is deprecated and answers 403 with no explanation.
      url = std::string(API) + "/me/library?uris=spotify:track:" +
            out->pb.track_id;
      method = out->pb.liked ? "PUT" : "DELETE";
      break;
    }
    case CommandType::None:
      return;
  }

  if (!call(method, url, body, &resp, out, now_ms)) return;

  if (resp.status == 404) {
    // By far the most common real failure: nothing has an active session.
    out->showToast("No active device", now_ms);
    out->pb.has_device = false;
  } else if (resp.status == 403) {
    out->showToast(c.type == CommandType::SetVolume ? "Volume not supported"
                                                    : "Not allowed",
                   now_ms);
  } else if (resp.status >= 400) {
    out->showToast("Command failed", now_ms);
  } else {
    out->pb.has_device = true;

    if (c.type == CommandType::Next || c.type == CommandType::Previous) {
      // Remember what was playing and chase the change. A flat delay was the
      // largest single contributor to skip latency: too short and the poll
      // returns the old track, too long and the screen just sits there.
      confirm_track_ = out->pb.track_id;
      confirm_polls_ = 0;
      // Spotify Connect can take a while to reflect a skip when playback lives
      // on another device, so allow longer than feels reasonable before giving
      // up and falling back to the normal cadence.
      confirm_.arm(now_ms, 8000);
      next_poll_.arm(now_ms, 120);
    } else {
      next_poll_.arm(now_ms, 250);
    }
  }
}

void SpotifySource::pollPlayer(AppState *out, uint32_t now_ms) {
  HttpResponse resp;
  if (!call("GET", std::string(API) + "/me/player", "", &resp, out, now_ms)) {
    return;
  }

  if (resp.status == 204 || resp.body.empty()) {
    // 204 means Spotify has no active playback session at all — not merely
    // paused. It happens when the app that was playing has gone idle or been
    // closed, and it is the difference between "nothing playing" and "we lost
    // the connection", which look identical on screen if not reported.
    NETLOG("state: /me/player 204 — no active session");
    out->pb.has_track = false;
    out->pb.is_playing = false;
    polled_ = true;
    next_poll_.arm(now_ms, idle_poll_ ? POLL_ASLEEP_MS : POLL_PAUSED_MS);
    return;
  }
  if (resp.status != 200) {
    next_poll_.arm(now_ms, POLL_PAUSED_MS);
    return;
  }

  JsonDocument filter;
  buildPlayerFilter(&filter);

  JsonDocument doc;
  const DeserializationError err = deserializeJson(
      doc, resp.body, DeserializationOption::Filter(filter));
  if (err) {
    out->showToast("Bad response", now_ms);
    next_poll_.arm(now_ms, POLL_PAUSED_MS);
    return;
  }

  JsonObjectConst item = doc["item"];
  if (item.isNull()) {
    // A valid response can omit item while the player transitions.
    out->pb.has_track = false;
    next_poll_.arm(now_ms, POLL_PAUSED_MS);
    return;
  }

  out->pb.has_track = true;
  out->pb.has_device = true;
  out->pb.is_playing = doc["is_playing"] | false;
  out->pb.progress_ms = doc["progress_ms"] | 0u;
  out->pb.duration_ms = item["duration_ms"] | 0u;

  setStr(out->pb.track_id, ID_LEN, item["id"] | "");
  setStr(out->pb.title, TEXT_LEN, item["name"] | "");

  JsonArrayConst artists = item["artists"];
  setStr(out->pb.artist, TEXT_LEN,
         artists.size() > 0 ? (artists[0]["name"] | "") : "");

  JsonObjectConst album = item["album"];
  setStr(out->pb.album_id, ID_LEN, album["id"] | "");

  // Absent volume must stay unknown, never be coerced to zero.
  JsonVariantConst vol = doc["device"]["volume_percent"];
  out->pb.volume_pct = vol.isNull() ? -1 : (vol.as<int>());

  // Artwork only touches the network on an album change, and only on a cache
  // miss after that.
  // Artwork: use the cache if it is already there, otherwise queue the
  // download and let this poll finish. Fetching inline meant the new title sat
  // in a buffer for the length of a 46KB transfer before reaching the screen.
  const char *img = pickImage(album["images"], ART_TARGET_PX);
  if (img && out->pb.album_id[0]) {
    const std::string cached = art_.cachedPath(out->pb.album_id);
    if (!cached.empty()) {
      setStr(out->pb.art_path, PATH_LEN, cached.c_str());
      out->pb.art_loading = false;
    } else if (art_.failed(out->pb.album_id)) {
      // Already tried and refused for this album. Queueing it again would burn
      // the between-polls slot every two seconds — starving the liked-state
      // refresh that shares it — and flicker the artwork region while doing it.
      out->pb.art_path[0] = '\0';
      out->pb.art_loading = false;
    } else {
      out->pb.art_path[0] = '\0';
      out->pb.art_loading = true;
      pending_art_album_ = out->pb.album_id;
      pending_art_url_ = img;
    }
  } else {
    out->pb.art_path[0] = '\0';
    out->pb.art_loading = false;
  }

  polled_ = true;

  // Still chasing a skip: keep polling fast until the track really changes.
  if (confirm_.armed()) {
    const bool changed = confirm_track_ != out->pb.track_id;
    ++confirm_polls_;
    if (changed || confirm_.elapsed(now_ms)) {
      NETLOG("skip confirmed after %u polls, %ums%s", confirm_polls_,
             (unsigned)confirm_.elapsedMs(now_ms), changed ? "" : " (GAVE UP)");
      confirm_.disarm();
      confirm_polls_ = 0;
      confirm_track_.clear();
    } else {
      next_poll_.arm(now_ms, 120);
      return;
    }
  }

  {
    static uint32_t last_state_log = 0;
    if (now_ms - last_state_log > 10000) {
      last_state_log = now_ms;
      NETLOG("state: playing=%d device=%d '%s' vol=%d", (int)out->pb.is_playing,
             (int)out->pb.has_device, out->pb.title, out->pb.volume_pct);
    }
  }

  next_poll_.arm(now_ms, out->pb.is_playing
                             ? POLL_PLAYING_MS
                             : (idle_poll_ ? POLL_ASLEEP_MS : POLL_PAUSED_MS));
}

void SpotifySource::refreshLiked(AppState *out, uint32_t now_ms) {
  if (!liked_supported_) {
    out->pb.liked_known = false;
    return;
  }
  if (out->pb.track_id[0] == '\0') return;
  if (last_liked_track_ == out->pb.track_id) return;

  HttpResponse resp;
  const std::string url = std::string(API) +
                          "/me/library/contains?uris=spotify:track:" +
                          out->pb.track_id;
  if (!call("GET", url, "", &resp, out, now_ms)) return;

  if (resp.status == 403 || resp.status == 404) {
    // Give up permanently rather than retrying at the poll rate. Saved-state
    // then stays unknown, which the UI renders as no heart at all.
    NETLOG("saved-state unavailable: /me/library/contains -> %d", resp.status);
    liked_supported_ = false;
    out->pb.liked_known = false;
    return;
  }
  if (resp.status != 200) return;
  NETLOG("library/contains body: %.120s", resp.body.c_str());

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) return;
  if (doc.is<JsonArray>() && doc.size() > 0) {
    out->pb.liked = doc[0] | false;
    out->pb.liked_known = true;
    last_liked_track_ = out->pb.track_id;
  }
}

// SPOTIFY_DIAG=1: probe a handful of endpoints once and log only their status
// codes, to tell "scope not actually usable" apart from "this one endpoint is
// restricted". Never logs tokens.
void SpotifySource::diagnose(AppState *out, uint32_t now_ms) {
  const char *probes[] = {
      // Response SIZE is the number that decides whether this fits on a board
      // with no PSRAM. Spotify's `market` parameter is documented to replace
      // the huge available_markets arrays with a single is_playable flag.
      "/me/player",
      "/me/player?market=from_token",
      "/me",
      "/me/player/devices",
      "/me/tracks?limit=1",
      "/me/albums?limit=1",
      "/me/library/contains?uris=spotify:track:4cOdK2wGLETKBW3PvgPWqT",
      // Would a real, beat-synced visualiser be possible? These are the only
      // sources of tempo/energy the Web API offers.
      "/audio-features/4cOdK2wGLETKBW3PvgPWqT",
      "/audio-analysis/4cOdK2wGLETKBW3PvgPWqT",
  };
  for (const char *p : probes) {
    HttpResponse r;
    if (call("GET", std::string(API) + p, "", &r, out, now_ms)) {
      size_t am = 0, pos = 0;
      while ((pos = r.body.find("available_markets", pos)) != std::string::npos) {
        ++am;
        pos += 17;
      }
      NETLOG("DIAG %-34s -> %d  %6zu bytes  available_markets x%zu", p,
             r.status, r.body.size(), am);
    } else {
      NETLOG("DIAG %-24s -> transport/auth failure", p);
    }
  }
}

// SPOTIFY_DIAG_WRITE=1: verify PUT/DELETE /me/library actually work.
//
// Only runs when the current track is NOT already saved, and reverts what it
// does, so the user's library ends exactly as it started. If the track is
// already saved it refuses, because then a revert would mean deleting
// something the user actually wanted.
void SpotifySource::probeLibraryWrite(AppState *out, uint32_t now_ms) {
  if (out->pb.track_id[0] == '\0') return;
  const std::string uri = std::string("spotify:track:") + out->pb.track_id;
  const std::string contains = std::string(API) + "/me/library/contains?uris=" + uri;
  const std::string lib = std::string(API) + "/me/library?uris=" + uri;

  HttpResponse r;
  if (!call("GET", contains, "", &r, out, now_ms) || r.status != 200) {
    NETLOG("WRITETEST: cannot read saved-state, aborting");
    return;
  }
  if (r.body.find("true") != std::string::npos) {
    NETLOG("WRITETEST: track already saved — refusing, revert would delete it");
    return;
  }

  if (!call("PUT", lib, "", &r, out, now_ms)) return;
  NETLOG("WRITETEST: PUT  /me/library -> %d %.80s", r.status,
         r.status >= 400 ? r.body.c_str() : "");
  const bool put_ok = r.status >= 200 && r.status < 300;

  if (put_ok) {
    call("GET", contains, "", &r, out, now_ms);
    NETLOG("WRITETEST: after PUT, contains = %.20s", r.body.c_str());

    // Revert, so the library ends as it began.
    if (call("DELETE", lib, "", &r, out, now_ms)) {
      NETLOG("WRITETEST: DELETE /me/library -> %d (reverting)", r.status);
      call("GET", contains, "", &r, out, now_ms);
      NETLOG("WRITETEST: after revert, contains = %.20s", r.body.c_str());
    }
  }
}

void SpotifySource::step(AppState *out, CommandQueue<> *cmds, uint32_t now_ms) {
  polled_ = false;

  if (std::getenv("SPOTIFY_DIAG")) {
    static bool done = false;
    if (!done) {
      done = true;
      diagnose(out, now_ms);
    }
  }

  if (rate_limited_.armed()) {
    if (rate_limited_.pending(now_ms)) return;
    rate_limited_.disarm();
  }

  Command c;
  while (cmds->pop(&c)) {
    runCommand(c, out, now_ms);
  }

  if (next_poll_.pending(now_ms)) {
    // Between polls is exactly when there is time for the saved-state check.
    // Doing it immediately after the poll delayed the new track reaching the
    // screen by a whole extra round trip, because the caller only merges once
    // step() returns.
    // Artwork first: a missing cover is more visible than a missing heart.
    if (!pending_art_album_.empty()) {
      const std::string album = pending_art_album_;
      const std::string url = pending_art_url_;
      pending_art_album_.clear();
      pending_art_url_.clear();
      const uint32_t t0 = now_ms;
      const std::string path = art_.ensure(album, url);
      NETLOG("artwork fetch %ums%s", (unsigned)(nowMs() - t0),
             path.empty() ? " (failed)" : "");
      // Publish the outcome either way: leaving art_loading set on failure
      // would freeze the placeholder on screen for the rest of the track.
      if (album == out->pb.album_id) {
        if (!path.empty()) setStr(out->pb.art_path, PATH_LEN, path.c_str());
        out->pb.art_loading = false;
        polled_ = true;  // the merge only copies when a poll happened
      }
      return;
    }
    if (liked_pending_) {
      liked_pending_ = false;
      refreshLiked(out, now_ms);
    }
    return;
  }

  const uint32_t t0 = now_ms;
  pollPlayer(out, now_ms);
  if (polled_) {
    liked_pending_ = true;
#if defined(TRACE_RENDER)
    NETLOG("poll round trip %ums", (unsigned)(nowMs() - t0));
#else
    (void)t0;
#endif
  }

  if (std::getenv("SPOTIFY_DIAG_WRITE")) {
    static bool done = false;
    if (!done && out->pb.track_id[0] != '\0') {
      done = true;
      probeLibraryWrite(out, now_ms);
    }
  }
}
