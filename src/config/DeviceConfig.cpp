#include "DeviceConfig.h"

#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_COMPILED_SECRETS 1
#endif
#endif

#if defined(DEVICE)
#include <Preferences.h>

namespace {
constexpr const char *NVS_NS = "m5cfg";

std::string getOr(Preferences &p, const char *key, const char *fallback) {
  const String v = p.getString(key, "");
  return v.length() ? std::string(v.c_str()) : std::string(fallback);
}
}  // namespace

DeviceConfig DeviceConfig::load() {
  const char *ssid = "", *pass = "", *cid = "", *csec = "", *rtok = "";
  const char *mtok = "";
#if defined(HAVE_COMPILED_SECRETS)
  ssid = WIFI_SSID;
  pass = WIFI_PASSWORD;
  cid = SPOTIFY_CLIENT_ID;
  csec = SPOTIFY_CLIENT_SECRET;
  rtok = SPOTIFY_REFRESH_TOKEN;
  mtok = MAC_LINK_TOKEN;
#endif

  DeviceConfig c;
  Preferences p;
  if (p.begin(NVS_NS, /*readOnly=*/true)) {
    c.wifi_ssid = getOr(p, "ssid", ssid);
    c.wifi_password = getOr(p, "pass", pass);
    c.client_id = getOr(p, "cid", cid);
    c.client_secret = getOr(p, "csec", csec);
    c.refresh_token = getOr(p, "rtok", rtok);
    c.mac_token = getOr(p, "mtok", mtok);
    c.views_mask = p.getUInt("views", 0xFF);
    p.end();
  } else {
    // First boot: the namespace does not exist yet.
    c.wifi_ssid = ssid;
    c.wifi_password = pass;
    c.client_id = cid;
    c.client_secret = csec;
    c.refresh_token = rtok;
    c.mac_token = mtok;
  }
  return c;
}

bool DeviceConfig::save(const DeviceConfig &c) {
  Preferences p;
  if (!p.begin(NVS_NS, /*readOnly=*/false)) return false;
  bool ok = true;
  if (!c.wifi_ssid.empty()) ok &= p.putString("ssid", c.wifi_ssid.c_str()) > 0;
  // Password may legitimately be empty (open network) — written whenever the
  // SSID was, so the pair stays consistent.
  if (!c.wifi_ssid.empty()) p.putString("pass", c.wifi_password.c_str());
  if (!c.client_id.empty()) ok &= p.putString("cid", c.client_id.c_str()) > 0;
  if (!c.client_secret.empty())
    ok &= p.putString("csec", c.client_secret.c_str()) > 0;
  if (!c.refresh_token.empty())
    ok &= p.putString("rtok", c.refresh_token.c_str()) > 0;
  if (!c.mac_token.empty()) ok &= p.putString("mtok", c.mac_token.c_str()) > 0;
  p.putUInt("views", c.views_mask);
  p.end();
  return ok;
}

#else  // desktop: compiled secrets or nothing; the portal is hardware-only.

DeviceConfig DeviceConfig::load() {
  DeviceConfig c;
#if defined(HAVE_COMPILED_SECRETS)
  c.wifi_ssid = WIFI_SSID;
  c.wifi_password = WIFI_PASSWORD;
  c.client_id = SPOTIFY_CLIENT_ID;
  c.client_secret = SPOTIFY_CLIENT_SECRET;
  c.refresh_token = SPOTIFY_REFRESH_TOKEN;
  c.mac_token = MAC_LINK_TOKEN;
#endif
  return c;
}

bool DeviceConfig::save(const DeviceConfig &) { return false; }

#endif
