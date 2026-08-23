#include "SpotifyAuth.h"

#include <ArduinoJson.h>

#include "../net/HttpClient.h"
#include "../net/NetLog.h"

namespace {

constexpr const char *TOKEN_URL = "https://accounts.spotify.com/api/token";

// Refresh this long before the token actually expires.
constexpr uint32_t REFRESH_MARGIN_MS = 60000;

// Backoff after a network failure, and the much longer interval used once the
// grant itself is rejected — there is no point hammering a revoked token.
constexpr uint32_t RETRY_BACKOFF_MS = 5000;
constexpr uint32_t REAUTH_RETRY_MS = 300000;

const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string &in) {
  std::string out;
  out.reserve(((in.size() + 2) / 3) * 4);
  size_t i = 0;
  while (i + 2 < in.size()) {
    const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                       (static_cast<uint8_t>(in[i + 1]) << 8) |
                       static_cast<uint8_t>(in[i + 2]);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += kB64[n & 63];
    i += 3;
  }
  if (i + 1 == in.size()) {
    const uint32_t n = static_cast<uint8_t>(in[i]) << 16;
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += "==";
  } else if (i + 2 == in.size()) {
    const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                       (static_cast<uint8_t>(in[i + 1]) << 8);
    out += kB64[(n >> 18) & 63];
    out += kB64[(n >> 12) & 63];
    out += kB64[(n >> 6) & 63];
    out += '=';
  }
  return out;
}

// Percent-encodes the few characters that actually matter in a token.
std::string formEncode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

}  // namespace

SpotifyAuth::SpotifyAuth(const char *client_id, const char *client_secret,
                         const char *refresh_token)
    : client_id_(client_id ? client_id : ""),
      client_secret_(client_secret ? client_secret : ""),
      refresh_token_(refresh_token ? refresh_token : "") {}

bool SpotifyAuth::ensureFresh(uint32_t now_ms) {
  // Refresh proactively, a margin before expiry. Expressed as elapsed time so
  // it cannot invert at the millis() wrap — the absolute form would have kept
  // reporting the token as valid forever after 49.7 days.
  if (!access_token_.empty() && valid_for_.pending(now_ms)) return true;
  if (retry_after_.pending(now_ms)) return false;

  const std::string body = "grant_type=refresh_token&refresh_token=" +
                           formEncode(refresh_token_);
  const std::string basic = base64(client_id_ + ":" + client_secret_);

  std::vector<std::string> headers = {
      "Authorization: Basic " + basic,
      "Content-Type: application/x-www-form-urlencoded",
  };

  HttpResponse resp;
  if (!http::request("POST", TOKEN_URL, headers, body, &resp)) {
    NETLOG("token refresh: transport failure (no HTTP response)");
    ++consecutive_failures_;
    status_ = consecutive_failures_ >= 3 ? LinkStatus::AuthError
                                         : LinkStatus::Connecting;
    retry_after_.arm(now_ms, RETRY_BACKOFF_MS);
    return false;
  }

  NETLOG("token refresh: HTTP %d", resp.status);
  if (resp.status != 200) {
    // Spotify returns {"error":"...","error_description":"..."} — safe to log.
    NETLOG("token refresh body: %.200s", resp.body.c_str());
  }

  if (resp.status == 400 || resp.status == 401) {
    // invalid_grant: the refresh token was revoked. This is terminal — retry
    // rarely and tell the user to re-authorise rather than spinning.
    status_ = LinkStatus::ReauthNeeded;
    retry_after_.arm(now_ms, REAUTH_RETRY_MS);
    return false;
  }

  if (resp.status != 200) {
    ++consecutive_failures_;
    status_ = consecutive_failures_ >= 3 ? LinkStatus::AuthError
                                         : LinkStatus::Connecting;
    retry_after_.arm(now_ms, RETRY_BACKOFF_MS);
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, resp.body) != DeserializationError::Ok) {
    ++consecutive_failures_;
    retry_after_.arm(now_ms, RETRY_BACKOFF_MS);
    return false;
  }

  const char *tok = doc["access_token"];
  if (!tok) {
    ++consecutive_failures_;
    retry_after_.arm(now_ms, RETRY_BACKOFF_MS);
    return false;
  }

  access_token_ = tok;
  const uint32_t expires_in = doc["expires_in"] | 3600;
  const uint32_t lifetime_ms = expires_in * 1000;
  valid_for_.arm(now_ms, lifetime_ms > REFRESH_MARGIN_MS
                             ? lifetime_ms - REFRESH_MARGIN_MS
                             : lifetime_ms / 2);
  consecutive_failures_ = 0;
  retry_after_.disarm();
  status_ = LinkStatus::Online;

  // Scope list is not sensitive, and a missing scope is the usual cause of an
  // otherwise unexplained 403 from the API.
  if (const char *sc = doc["scope"]) NETLOG("granted scopes: %s", sc);

  // Spotify may hand back a rotated refresh token; adopt it if so.
  if (const char *rt = doc["refresh_token"]) refresh_token_ = rt;

  return true;
}
