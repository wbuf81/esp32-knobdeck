#include "net/MacLink.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace net {
namespace {

// Copies at most cap-1 bytes and always terminates. No length from the wire is
// ever trusted.
void copyField(char *dst, size_t cap, const char *src, size_t len) {
  if (len > cap - 1) len = cap - 1;
  std::memcpy(dst, src, len);
  dst[len] = '\0';
}

// Finds `key` at the START of a line and returns its value span. Anchoring to
// the line start is what makes an unknown key harmless rather than a way to
// smuggle a value into a field it does not belong to.
bool field(const char *body, const char *key, const char **val, size_t *len) {
  const size_t klen = std::strlen(key);
  const char *p = body;
  while (*p) {
    const char *eol = std::strchr(p, '\n');
    const size_t line_len = eol ? static_cast<size_t>(eol - p) : std::strlen(p);
    if (line_len > klen && std::strncmp(p, key, klen) == 0 && p[klen] == '=') {
      *val = p + klen + 1;
      *len = line_len - klen - 1;
      return true;
    }
    if (!eol) break;
    p = eol + 1;
  }
  return false;
}

int fieldInt(const char *body, const char *key, int fallback) {
  const char *v = nullptr;
  size_t n = 0;
  if (!field(body, key, &v, &n)) return fallback;
  char buf[16];
  if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
  std::memcpy(buf, v, n);
  buf[n] = '\0';
  return std::atoi(buf);
}

void copyIfPresent(const char *body, const char *key, char *dst, size_t cap) {
  const char *v = nullptr;
  size_t n = 0;
  if (field(body, key, &v, &n)) copyField(dst, cap, v, n);
}

// Compares in CONSTANT TIME with respect to how much of the token matches.
//
// strncmp returns at the first differing byte, so how long it takes leaks how
// many leading bytes were right. On a LAN, against a 32-byte token, that is a
// thin channel - but it is a thin channel guarding something that ends in
// osascript on someone's laptop, and the fix is four lines.
bool tokenEqual(const char *a, const char *b, size_t n) {
  unsigned char diff = 0;
  for (size_t i = 0; i < n; ++i) {
    diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
  }
  return diff == 0;
}

}  // namespace

void MacLink::setToken(const char *tok) {
  if (tok == nullptr) {
    token_[0] = '\0';
    return;
  }
  copyField(token_, sizeof(token_), tok, std::strlen(tok));
}

bool MacLink::applyBeat(const char *body, uint32_t now_ms) {
  if (body == nullptr) return false;
  if (std::strlen(body) > MAX_BODY) return false;

  if (fieldInt(body, "v", -1) != PROTOCOL_V) return false;

  // The token gate, checked before ANYTHING is written to state_. A rejected
  // beat that still moved a field would be the whole vulnerability.
  const char *tv = nullptr;
  size_t tn = 0;
  if (token_[0] == '\0') return false;
  if (!field(body, "tok", &tv, &tn)) return false;
  if (tn != std::strlen(token_)) return false;
  if (!tokenEqual(tv, token_, tn)) return false;

  // Past the gate: now it is safe to apply.
  state_.locked = fieldInt(body, "locked", 0) != 0;

  state_.out_vol = fieldInt(body, "out_vol", -1);
  if (state_.out_vol > 100) state_.out_vol = 100;
  if (state_.out_vol < -1) state_.out_vol = -1;

  state_.out_muted = fieldInt(body, "out_muted", -1);
  if (state_.out_muted > 1) state_.out_muted = 1;
  if (state_.out_muted < -1) state_.out_muted = -1;

  copyIfPresent(body, "host", state_.host, sizeof(state_.host));
  copyIfPresent(body, "sp_device", state_.sp_device, sizeof(state_.sp_device));

  state_.sp_playing = fieldInt(body, "sp_playing", 0) != 0;
  copyIfPresent(body, "sp_track", state_.sp_track, sizeof(state_.sp_track));
  copyIfPresent(body, "sp_artist", state_.sp_artist, sizeof(state_.sp_artist));
  copyIfPresent(body, "sp_uri", state_.sp_uri, sizeof(state_.sp_uri));
  state_.sp_pos_ms = fieldInt(body, "sp_pos_ms", -1);
  state_.sp_dur_ms = fieldInt(body, "sp_dur_ms", -1);

  auto tri = [&](const char *key) {
    int v = fieldInt(body, key, -1);
    return v > 1 ? 1 : (v < -1 ? -1 : v);
  };
  state_.teams_in_call = tri("teams_in_call");
  state_.teams_muted = tri("teams_muted");
  state_.teams_camera = tri("teams_camera");

  state_.valid = true;
  last_beat_ms_ = now_ms;
  return true;
}

void MacLink::requestTeamsToggleMute() {
  if (!commandChannelEnabled()) return;
  pending_mute_ = true;
}

void MacLink::requestTeamsToggleCamera() {
  if (!commandChannelEnabled()) return;
  pending_cam_ = true;
}

void MacLink::requestOutputVolumeDelta(int detents) {
  if (!commandChannelEnabled()) return;
  pending_delta_ += detents;  // accumulated: clicks add up, they do not replace
  if (pending_delta_ > MAX_DELTA) pending_delta_ = MAX_DELTA;
  if (pending_delta_ < -MAX_DELTA) pending_delta_ = -MAX_DELTA;
}

int MacLink::buildResponse(char *out, size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  int n = std::snprintf(out, cap, "v=%d\ntok=%s\n", PROTOCOL_V, token_);
  if (n < 0 || static_cast<size_t>(n) >= cap) return 0;
  auto append = [&](const char *fmt, int v) {
    const int m = std::snprintf(out + n, cap - static_cast<size_t>(n), fmt, v);
    if (m > 0 && static_cast<size_t>(n + m) < cap) n += m;
  };
  if (pending_delta_ != 0) append("adjust_output_volume=%d\n", pending_delta_);
  if (pending_mute_) append("teams_toggle_mute=%d\n", 1);
  if (pending_cam_) append("teams_toggle_camera=%d\n", 1);
  // Delivered exactly once, all of them - a toggle repeated on the next beat
  // would un-toggle it.
  pending_delta_ = 0;
  pending_mute_ = false;
  pending_cam_ = false;
  return n;
}

bool MacLink::stale(uint32_t now_ms) const {
  if (!state_.valid) return false;  // fails open
  return (now_ms - last_beat_ms_) > TIMEOUT_MS;
}

}  // namespace net
