#include "SpotifySource.h"

#include "DevicePick.h"

#include <cstring>

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
  (*filter)["context"]["uri"] = true;
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
  // cache_dir is unused: covers live in PSRAM, not on a card. Kept in the
  // signature because the desktop build may yet want a real directory.
  (void)cache_dir;
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
    case CommandType::FetchPlaylists:
      fetchPlaylists(out, now_ms);
      return;
    case CommandType::FetchTracks:
      // c.uri is the playlist URI, c.text its name; the bare id is the tail of
      // the URI.
      // The bare id is derived from the URI inside fetchTracks; there is no
      // separate id on the command.
      fetchTracks(nullptr, c.uri, c.text, out, now_ms);
      return;
    case CommandType::FetchQueue:
      fetchQueue(out, now_ms);
      return;
    case CommandType::PlayQueueItem:
      playQueueItem(c, out, now_ms);
      return;
    case CommandType::WakeDevice:
      wakeDevice(out, now_ms);
      return;
    case CommandType::PlayFromContext: {
      url = std::string(API) + "/me/player/play";
      method = "PUT";
      // Playing WITHIN a context rather than as a bare track URI, so what
      // follows is the rest of the playlist. A bare uris:[...] play would end
      // the queue after this one song, which is not what picking a track from a
      // playlist means.
      char buf[192];
      std::snprintf(buf, sizeof(buf),
                    "{\"context_uri\":\"%s\",\"offset\":{\"position\":%d}}",
                    c.uri, c.arg);
      body = buf;
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
    // And nothing is listening. This is the one place that learns it WITHOUT
    // first firing a command and reading the 404 off the failure - which is why
    // the transport furniture used to reappear after every reboot and cost one
    // doomed request to clear again.
    out->pb.has_device = false;
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

  // Remembered for the queue jump. A null context is normal - a bare track or
  // autoplay radio has none - and is exactly the case the jump has to fall back
  // for, so it is recorded as empty rather than left stale from the last
  // playlist.
  context_uri_ = doc["context"]["uri"] | "";

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

namespace {

// The bare id is the last colon-separated field of a Spotify URI.
const char *idFromUri(const char *uri) {
  if (!uri) return "";
  const char *last = std::strrchr(uri, ':');
  return last ? last + 1 : uri;
}

}  // namespace

void SpotifySource::fetchPlaylists(AppState *out, uint32_t now_ms) {
  if (!library_) return;

  // fields= trims the response server-side. Spotify's full playlist objects
  // carry images, owners and follower counts; asking for three fields turns a
  // response measured in tens of kilobytes into one measured in hundreds of
  // bytes per item, which is the difference between parsing it on this board and
  // not.
  char url[160];
  std::snprintf(url, sizeof(url),
                "%s/me/playlists?limit=%d&fields=items(name,uri,id),total", API,
                spotify::Library::MAX_PLAYLISTS);

  HttpResponse resp;
  if (!call("GET", url, "", &resp, out, now_ms)) return;
  if (resp.status != 200) {
    NETLOG("playlists -> %d", resp.status);
    return;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    NETLOG("playlists: could not parse %u bytes", (unsigned)resp.body.size());
    return;
  }

  library_->clearPlaylists();
  JsonArrayConst items = doc["items"];
  int n = 0;
  for (JsonObjectConst it : items) {
    const char *name = it["name"] | "";
    const char *uri = it["uri"] | "";
    if (!name[0] || !uri[0]) continue;  // a null entry is a deleted playlist
    if (!library_->addPlaylist(name, uri, it["id"] | "")) break;
    ++n;
  }
  const int total = doc["total"] | n;
  library_->publishPlaylists(total > n);
  NETLOG("playlists: %d of %d", n, total);
}

void SpotifySource::fetchTracks(const char *playlist_id,
                                const char *playlist_uri,
                                const char *playlist_name, AppState *out,
                                uint32_t now_ms) {
  if (!library_ || !playlist_uri || !playlist_uri[0]) return;
  const char *id = playlist_id && playlist_id[0] ? playlist_id
                                                 : idFromUri(playlist_uri);

  // Cleared and published FIRST, so a frame drawn mid-fetch shows an empty list
  // under the new heading rather than the previous playlist's tracks under it.
  library_->clearTracks(playlist_uri, playlist_name);

  char url[200];
  std::snprintf(
      url, sizeof(url),
      "%s/playlists/%s/tracks?limit=%d&fields=items(track(name,uri,id)),total",
      API, id, spotify::Library::MAX_TRACKS);

  HttpResponse resp;
  if (!call("GET", url, "", &resp, out, now_ms)) return;

  if (resp.status == 403) {
    // Retry once WITHOUT fields=, to separate two causes that both answer a
    // bare "Forbidden": the endpoint being unavailable to this app at all,
    // versus the field filter being rejected. Worth one extra request, because
    // only one of those is fixable here.
    char plain[160];
    std::snprintf(plain, sizeof(plain), "%s/playlists/%s/tracks?limit=%d", API,
                  id, spotify::Library::MAX_TRACKS);
    HttpResponse alt;
    if (call("GET", plain, "", &alt, out, now_ms)) {
      NETLOG("tracks retry without fields -> %d", alt.status);
      if (alt.status == 200) {
        NETLOG("  fields= was the problem, not the endpoint");
        resp = alt;
      }
    }
  }

  if (resp.status != 200) {
    // The whole URL and the start of the body. A bare status cannot distinguish
    // a refused playlist from a malformed request, and Spotify puts its reason
    // in the body.
    NETLOG("tracks -> %d url=%s", resp.status, url);
    NETLOG("tracks body: %.180s", resp.body.c_str());
    NETLOG("tracks playlist='%s' uri=%s id=%s",
           playlist_name ? playlist_name : "?", playlist_uri, id);
    library_->setTracksError(resp.status);
    library_->publishTracks(false);
    return;
  }
  library_->setTracksError(0);

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    NETLOG("tracks: could not parse %u bytes", (unsigned)resp.body.size());
    library_->publishTracks(false);
    return;
  }

  JsonArrayConst items = doc["items"];
  int n = 0;
  for (JsonObjectConst it : items) {
    JsonObjectConst t = it["track"];
    if (t.isNull()) continue;  // a local file or an unavailable track
    const char *name = t["name"] | "";
    const char *uri = t["uri"] | "";
    if (!name[0] || !uri[0]) continue;
    if (!library_->addTrack(name, uri, t["id"] | "")) break;
    ++n;
  }
  const int total = doc["total"] | n;
  library_->publishTracks(total > n);
  NETLOG("tracks: %d of %d in %s", n, total, playlist_name ? playlist_name : "");
}

void SpotifySource::fetchQueue(AppState *out, uint32_t now_ms) {
  if (!library_) return;

  // What is playing and what is coming up.
  //
  // This exists because /playlists/{id}/tracks is refused to this app, and it is
  // a better answer to "show me the current playlist" anyway: the queue is what
  // will actually be played next, including anything queued by hand, where a
  // playlist's track list is only where the queue happens to be drawn from.
  //
  // It also needs no scope beyond user-read-playback-state, which every build
  // already has.
  library_->clearTracks("", "UP NEXT");

  HttpResponse resp;
  const std::string url = std::string(API) + "/me/player/queue";
  // Timed on every fetch. Measured: the body is ~71KB and the round trip ~750ms
  // with no available_markets to strip - market= and additional_types= were both
  // tried and neither shrinks it - so this line exists to catch the case where
  // it is instead multiple seconds, which has been seen once and not explained.
  const uint32_t t_q0 = nowMs();
  if (!call("GET", url, "", &resp, out, now_ms)) {
    library_->publishTracks(false);
    return;
  }
  NETLOG("QSIZE plain           %6u bytes in %5ums", (unsigned)resp.body.size(),
         (unsigned)(nowMs() - t_q0));
  if (resp.status != 200) {
    NETLOG("queue -> %d", resp.status);
    library_->setTracksError(resp.status);
    library_->publishTracks(false);
    return;
  }
  library_->setTracksError(0);

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    NETLOG("queue: could not parse %u bytes", (unsigned)resp.body.size());
    library_->publishTracks(false);
    return;
  }

  int n = 0;
  // The current track first, so the list reads as a position in something
  // rather than as a detached list of what comes after.
  JsonObjectConst cur = doc["currently_playing"];
  if (!cur.isNull()) {
    const char *name = cur["name"] | "";
    if (name[0] && library_->addTrack(name, cur["uri"] | "", cur["id"] | ""))
      ++n;
  }
  for (JsonObjectConst t : doc["queue"].as<JsonArrayConst>()) {
    const char *name = t["name"] | "";
    if (!name[0]) continue;
    if (!library_->addTrack(name, t["uri"] | "", t["id"] | "")) break;
    ++n;
  }
  library_->publishTracks(false);
  NETLOG("queue: %d entries", n);
}

// Jump to an entry in UP NEXT.
//
// Spotify has no "play queue index N" call, so this is built out of the two
// things the API does offer, in the order that gives the better result:
//
//  1. Play the track WITHIN the context that is already playing. One request,
//     it lands exactly on the track, and the rest of the playlist keeps going
//     after it. Only possible when there is a context and the track belongs to
//     it - so it is attempted, not assumed.
//  2. Otherwise skip forward once per row. Slower and visible, but it cannot be
//     wrong: the queue IS what /next walks through, including anything the user
//     queued by hand, which is precisely the case tier 1 cannot serve.
//
// Playing the bare track uri was the third option and is deliberately not here.
// It works every time and destroys the queue every time - everything below the
// track you picked is gone and Spotify drops into autoplay radio when it ends.
// A jump that silently discards the rest of your queue is not a jump.
void SpotifySource::wakeDevice(AppState *out, uint32_t now_ms) {
  HttpResponse resp;
  if (!call("GET", std::string(API) + "/me/player/devices", "", &resp, out,
            now_ms)) {
    return;
  }
  if (resp.status != 200) {
    NETLOG("wake: /me/player/devices -> %d", resp.status);
    out->showToast("Cannot list devices", now_ms);
    return;
  }

  // Only the fields the choice needs. The full payload carries volume and
  // supports_volume per device, and this board has no reason to buffer them.
  // [0] applies the filter to EVERY element of the array - the same form
  // buildPlayerFilter uses for artists and images, rather than a second idiom
  // for the same job.
  JsonDocument filter;
  filter["devices"][0]["id"] = true;
  filter["devices"][0]["name"] = true;
  filter["devices"][0]["type"] = true;
  filter["devices"][0]["is_active"] = true;
  filter["devices"][0]["is_restricted"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, resp.body,
                      DeserializationOption::Filter(filter)) !=
      DeserializationError::Ok) {
    out->showToast("Bad response", now_ms);
    return;
  }

  spotify::DeviceInfo found[8];
  int n = 0;
  for (JsonObjectConst d : doc["devices"].as<JsonArrayConst>()) {
    if (n >= 8) break;
    setStr(found[n].id, sizeof(found[n].id), d["id"] | "");
    setStr(found[n].name, sizeof(found[n].name), d["name"] | "");
    setStr(found[n].type, sizeof(found[n].type), d["type"] | "");
    found[n].is_active = d["is_active"] | false;
    found[n].is_restricted = d["is_restricted"] | false;
    if (found[n].id[0] == '\0') continue;  // unusable without an id
    ++n;
  }
  NETLOG("wake: %d device(s) known", n);
  for (int i = 0; i < n; ++i) {
    NETLOG("wake:   %s (%s)%s%s", found[i].name, found[i].type,
           found[i].is_active ? " active" : "",
           found[i].is_restricted ? " RESTRICTED" : "");
  }

  const int pick = spotify::pickDevice(found, n);
  if (pick < 0) {
    // The honest answer. Spotify fully quit does not appear in this list at
    // all, and there is nothing the device can do about that.
    out->showToast("No devices found", now_ms);
    return;
  }
  NETLOG("wake: transferring to %s", found[pick].name);

  // play:true makes this the transfer AND the resume, which is why waking is
  // two requests rather than three.
  char body[128];
  std::snprintf(body, sizeof(body),
                "{\"device_ids\":[\"%s\"],\"play\":true}", found[pick].id);
  HttpResponse tr;
  if (!call("PUT", std::string(API) + "/me/player", body, &tr, out, now_ms)) {
    return;
  }
  if (tr.status >= 200 && tr.status < 300) {
    out->pb.has_device = true;
    // Poll again almost immediately: the whole point is that the player view
    // replaces the dog promptly rather than at the idle cadence.
    next_poll_.arm(now_ms, 250);
    NETLOG("wake: transfer accepted");
    return;
  }
  NETLOG("wake: transfer -> %d", tr.status);
  if (tr.status == 404) out->showToast("Device gone", now_ms);
  else if (tr.status == 403) out->showToast("Not allowed", now_ms);
  else out->showToast("Wake failed", now_ms);
}

void SpotifySource::playQueueItem(const Command &c, AppState *out,
                                  uint32_t now_ms) {
  if (c.uri[0] == '\0') return;

  bool done = false;
  if (!context_uri_.empty()) {
    char body[256];
    std::snprintf(body, sizeof(body),
                  "{\"context_uri\":\"%s\",\"offset\":{\"uri\":\"%s\"}}",
                  context_uri_.c_str(), c.uri);
    HttpResponse resp;
    if (call("PUT", std::string(API) + "/me/player/play", body, &resp, out,
             now_ms)) {
      done = resp.status >= 200 && resp.status < 300;
      // Not an error worth a toast: a hand-queued track is simply not in the
      // context, and the fallback below is the right answer rather than a
      // consolation prize.
      if (!done) NETLOG("jump: context offset -> %d, skipping instead", resp.status);
    }
  }

  if (!done) {
    // One request per row. Bounded by the list itself, which is capped at
    // MAX_TRACKS, so this cannot become an unbounded burst against the rate
    // limit however the UI is driven.
    int skips = c.arg;
    if (skips < 0) skips = 0;
    if (skips > spotify::Library::MAX_TRACKS) skips = spotify::Library::MAX_TRACKS;
    NETLOG("jump: skipping forward %d", skips);
    for (int i = 0; i < skips; ++i) {
      HttpResponse resp;
      if (!call("POST", std::string(API) + "/me/player/next", "", &resp, out,
                now_ms))
        break;
      if (resp.status < 200 || resp.status >= 300) {
        NETLOG("jump: next -> %d, stopping", resp.status);
        break;
      }
    }
  }

  // The queue is deliberately NOT re-read here.
  //
  // Measured: Spotify answers /me/player/queue with nothing at all for a moment
  // after a context change - the read immediately after a successful jump came
  // back "queue: 0 entries" every time. So the re-fetch was both too early to be
  // true and a ~750ms request nobody was waiting on, since the confirmation
  // screen already returns to the player. The next swipe-down fetches a queue
  // that has settled.
  //
  // The player poll IS nudged, because the track on screen is now wrong and that
  // is the thing the user is looking at.
  nudge();
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
    // Only when it is bad enough to be felt. A command normally waits one 25ms
    // tick, and logging that every button press would bury the case this exists
    // to catch.
    if (c.submitted_ms != 0 && now_ms - c.submitted_ms > 250) {
      NETLOG("command %d waited %ums to start", (int)c.type,
             (unsigned)(now_ms - c.submitted_ms));
    }
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
