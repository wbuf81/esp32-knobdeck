#if defined(DEVICE)

#include <WiFi.h>

#include "../../net/NetLog.h"
#include "../../net/WifiLink.h"

void WifiLink::begin(const char *ssid, const char *password) {
  ssid_ = ssid;
  password_ = password;
  WiFi.mode(WIFI_STA);
  // The device is mains powered and must stay responsive; power saving adds
  // latency to every poll for no benefit here.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  status_ = LinkStatus::Connecting;
}

bool WifiLink::ensureConnected(uint32_t now_ms) {
  if (WiFi.status() == WL_CONNECTED) {
    if (status_ != LinkStatus::Online) {
      NETLOG("wifi connected, ip=%s rssi=%d", WiFi.localIP().toString().c_str(),
             (int)WiFi.RSSI());
    }
    attempting_ = false;
    backoff_ms_ = 0;
    status_ = LinkStatus::Online;
    return true;
  }

  if (attempting_) {
    if (attempt_.pending(now_ms)) {
      status_ = LinkStatus::Connecting;
      return false;
    }
    // Timed out. Back off before trying again, doubling up to the cap.
    WiFi.disconnect();
    attempting_ = false;
    backoff_ms_ = backoff_ms_ == 0
                      ? WIFI_BACKOFF_START_MS
                      : (backoff_ms_ * 2 > WIFI_BACKOFF_MAX_MS
                             ? WIFI_BACKOFF_MAX_MS
                             : backoff_ms_ * 2);
    retry_.arm(now_ms, backoff_ms_);
    status_ = LinkStatus::Offline;
    NETLOG("wifi attempt timed out, retrying in %ums", backoff_ms_);
    return false;
  }

  if (retry_.pending(now_ms)) {
    status_ = LinkStatus::Offline;
    return false;
  }

  NETLOG("wifi connecting to %s", ssid_ ? ssid_ : "(unset)");
  WiFi.begin(ssid_, password_);
  attempting_ = true;
  attempt_.arm(now_ms, WIFI_ATTEMPT_TIMEOUT_MS);
  status_ = LinkStatus::Connecting;
  return false;
}

#endif  // DEVICE
