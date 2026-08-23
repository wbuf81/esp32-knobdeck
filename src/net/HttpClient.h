#pragma once

// Blocking HTTP, called only from the net thread.
//
// One interface, two implementations: libcurl on the Mac, WiFiClientSecure on
// the ESP32. Everything above this line is shared.
//
// NOTE for the device port: this buffers the whole response body, which is fine
// on a host but not on a board with ~150KB of heap facing Spotify's very large
// /me/player payload. The ESP32 implementation must parse from a stream instead.

#include <string>
#include <vector>

struct HttpResponse {
  int status = 0;
  std::string body;
  long retry_after_s = 0;  // populated from the Retry-After header on 429
};

namespace http {

bool request(const char *method, const std::string &url,
             const std::vector<std::string> &headers, const std::string &body,
             HttpResponse *out);

// Streams straight to disk so a cover never sits in RAM in full. Kept for
// hosts with real storage; this board has no SD card wired and does not use it.
bool downloadToFile(const std::string &url, const std::string &path);

// Streams into a caller-provided buffer, capped.
//
// This is the path this board actually uses. The ancestor needed an SD card for
// artwork because it had no PSRAM to hold a cover; with 8 MB there is no reason
// for a filesystem to be involved at all - the JPEG lands in PSRAM, is decoded
// into PSRAM, and the card becomes optional hardware rather than required.
bool downloadToMemory(const std::string &url, std::vector<uint8_t> *out,
                      size_t max_bytes);

}  // namespace http
