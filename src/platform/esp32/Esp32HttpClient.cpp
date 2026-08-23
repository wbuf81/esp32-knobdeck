// Device HTTP over TLS.
//
// Certificate validation is on. The alternative, setInsecure(), is one line
// shorter and would send the Spotify client secret and refresh token over a
// connection anyone on the network could impersonate. The ESP-IDF root CA
// bundle ships with the Arduino core, so validation costs nothing to maintain —
// no pinned certificates to rotate when Spotify's expire.
//
// The TLS client is reused across requests. A fresh handshake costs roughly a
// second and tens of KB of heap; at a 2s poll interval, reconnecting every time
// would leave the device permanently mid-handshake.

#if defined(DEVICE)

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstring>

#include "../../net/HttpClient.h"
#include "../../core/Deadline.h"
#include "../../net/NetLog.h"

// Linked in by the Arduino core when the certificate bundle is enabled.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

namespace {

WiFiClientSecure *makeClient() {
  auto *c = new WiFiClientSecure();
  c->setCACertBundle(rootca_crt_bundle_start);
  // Spotify answers well inside this; the ceiling exists so a black-holed
  // connection cannot wedge the net task indefinitely.
  c->setTimeout(15);
  return c;
}

// Exactly ONE TLS session exists on this device, and it belongs to the API.
//
// Artwork used to have its own. It cannot: an established mbedTLS session
// allocates a ~16.7KB input buffer and a ~16.7KB output buffer, each of which
// must be CONTIGUOUS, and this board simply does not keep a 34KB run of heap
// free once WiFi, the view sprites and the JPEG decoder have taken their share.
//
// The measurements, all taken mid-failure on hardware:
//
//   free 81712, largest block 19444  -> handshake fails, two sessions live
//   free 122348, largest block 32756 -> handshake STILL fails, one session live
//
// That second line is the important one. Dropping the API session first was not
// enough: 32756 fits the first 16.7KB buffer and leaves ~16KB, just short of the
// second. The requirement sits right on the boundary, so it failed whenever the
// active view happened to hold one sprite too many — which is why the cover came
// and went with no pattern anyone could see from the outside.
//
// So artwork is fetched over plain HTTP and needs no session at all. The bytes
// are public cover images; nothing here is secret, no token or client secret
// goes near the CDN, and the API keeps full certificate validation. The upside
// beyond reliability is speed: a download that cost 1.3-4.5s of handshake is now
// a few hundred milliseconds.
//
// The HTTPClient and its client are owned as a PAIR, and both are heap-allocated
// so a reset can destroy them in the right order.
//
// HTTPClient keeps a raw _client pointer and never clears it — not in end(),
// not in disconnect(). Deleting the WiFiClientSecure underneath a surviving
// HTTPClient therefore leaves that pointer dangling, and the next request
// crashes inside HTTPClient::connected() (LoadProhibited, EXCVADDR 0x18) called
// from setTimeout(), which runs BEFORE begin() gets a chance to rebind it. The
// crash lands a poll later than the mistake, in code that looks innocent.
//
// This was already latent on the transport-error path; making the artwork
// download reset the API session turned "rare" into "every album change".
HTTPClient *g_api_http = nullptr;
WiFiClientSecure *g_api = nullptr;

HTTPClient &apiHttp() {
  if (!g_api_http) g_api_http = new HTTPClient();
  return *g_api_http;
}

WiFiClientSecure &apiClient() {
  if (!g_api) g_api = makeClient();
  return *g_api;
}

// Throw the TLS session away so the next request builds a clean one.
//
// Needed because a poisoned session does not heal on its own. Spotify closes an
// idle keep-alive socket after about a minute; HTTPClient then writes the next
// request into the dead socket and waits out the read timeout. From that point
// every reconnect failed with start_ssl_client: -1, permanently — the device
// went offline at 69s and never came back, with plenty of free heap. Calling
// stop() releases the socket and the mbedTLS context; the client is rebuilt
// lazily on the next call.
// Order is load-bearing: the HTTPClient goes first, while the client it points
// at is still alive, because end() and ~HTTPClient() both dereference it.
void resetApiSession() {
  if (g_api_http) {
    g_api_http->end();
    delete g_api_http;
    g_api_http = nullptr;
  }
  if (g_api) {
    g_api->stop();
    delete g_api;
    g_api = nullptr;
  }
}

// Plain WiFiClient, not WiFiClientSecure: see the note above. A few hundred
// bytes instead of ~35KB, and no contiguous-allocation cliff to fall off.
HTTPClient *g_cdn_http = nullptr;
WiFiClient *g_cdn = nullptr;

HTTPClient &cdnHttp() {
  if (!g_cdn_http) g_cdn_http = new HTTPClient();
  return *g_cdn_http;
}

WiFiClient &cdnClient() {
  if (!g_cdn) g_cdn = new WiFiClient();
  return *g_cdn;
}

// Same disease, same cure as resetApiSession(). The CDN session is MORE
// exposed to it than the API one: album changes are typically minutes apart,
// far past the ~60s idle close, so nearly every download after the first would
// have found a dead socket.
void resetCdnSession() {
  if (g_cdn_http) {
    g_cdn_http->end();
    delete g_cdn_http;
    g_cdn_http = nullptr;
  }
  if (g_cdn) {
    g_cdn->stop();
    delete g_cdn;
    g_cdn = nullptr;
  }
}

// Artwork download bounds. DL_STALL_MS is reset by every byte received, so a
// slow-but-progressing download is never killed; only a silent one is.
constexpr uint32_t DL_STALL_MS = 8000;
constexpr uint32_t DL_TOTAL_MS = 30000;

}  // namespace

namespace http {

namespace {
bool requestOnce(const char *method, const std::string &url,
                 const std::vector<std::string> &headers,
                 const std::string &body, HttpResponse *out, int attempt);
}  // namespace

bool request(const char *method, const std::string &url,
             const std::vector<std::string> &headers, const std::string &body,
             HttpResponse *out) {
  // Two attempts, because attempt 0 may be spent discovering that a reused
  // connection has already been closed by the far end. Attempt 1 always starts
  // from a fresh session, so a genuine failure still reports quickly.
  if (requestOnce(method, url, headers, body, out, 0)) return true;
  return requestOnce(method, url, headers, body, out, 1);
}

namespace {
bool requestOnce(const char *method, const std::string &url,
                 const std::vector<std::string> &headers,
                 const std::string &body, HttpResponse *out, int attempt) {
  out->body.clear();
  out->status = 0;
  out->retry_after_s = 0;

  // One persistent HTTPClient, not a fresh one per request.
  //
  // Keep-alive state lives on HTTPClient, not on the WiFiClientSecure it wraps,
  // so constructing one per call threw the connection away every time and paid
  // a full TLS handshake. Measured: /me/player cost ~930ms on every poll, two
  // seconds apart, which is a handshake rather than a request.
  HTTPClient &http = apiHttp();
  http.setReuse(true);
  // Bounded so one bad connection cannot wedge the net task for half a minute.
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(apiClient(), url.c_str())) {
    NETLOG("http.begin failed for %s", url.c_str());
    return false;
  }

  for (const auto &h : headers) {
    const size_t colon = h.find(':');
    if (colon == std::string::npos) continue;
    std::string name = h.substr(0, colon);
    std::string value = h.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    http.addHeader(name.c_str(), value.c_str());
  }

  // Spotify's control endpoints are PUT/POST with no body, and its frontend
  // rejects those with 411 Length Required unless Content-Length is present.
  // HTTPClient only adds the header when there IS a payload, so an empty-bodied
  // request goes out without it and every button press fails.
  //
  // libcurl sets this automatically, which is exactly why the emulator never
  // showed the problem and only real hardware did.
  if (body.empty() && std::strcmp(method, "GET") != 0) {
    http.addHeader("Content-Length", "0");
  }

  // Needed so a 429 can be honoured rather than guessed at.
  const char *collect[] = {"Retry-After"};
  http.collectHeaders(collect, 1);

  const uint32_t t_send = millis();
  int code;
  if (body.empty()) {
    code = http.sendRequest(method);
  } else {
    code = http.sendRequest(method, reinterpret_cast<uint8_t *>(
                                        const_cast<char *>(body.data())),
                            body.size());
  }

  if (code < 0) {
    // Reusing a connection races the server's idle close, so a transport error
    // is expected roughly once a minute and is not a real outage. Reset the
    // session and say so, rather than reporting offline for a poll interval
    // every time.
    NETLOG("%s %s failed: %s — resetting session", method, url.c_str(),
           HTTPClient::errorToString(code).c_str());
    resetApiSession();
    if (attempt == 0) return false;  // caller retries on a fresh connection
    return false;
  }

#if defined(TRACE_RENDER)
  NETLOG("  %s took %ums", method, (unsigned)(millis() - t_send));
#else
  (void)t_send;
#endif

  out->status = code;
  if (http.hasHeader("Retry-After")) {
    out->retry_after_s = http.header("Retry-After").toInt();
  }

  // Read the body only when there is one.
  //
  // This guard is the difference between working and hanging forever. A 204 (or
  // 304) carries no body and no Content-Length, so getSize() returns -1, and
  // getString() on a -1 length reads until the peer closes the connection. With
  // keep-alive on, Spotify never closes it, so the call blocks indefinitely and
  // takes the whole net task with it.
  //
  // /me/player answers 204 whenever playback is idle, so this was not an edge
  // case: with nothing playing, the first poll after boot wedged the task
  // before the link could ever reach online, and the display sat on CONNECTING
  // until something else rebooted the device. It only ever worked because
  // testing happened with music playing.
  const int size = http.getSize();
  const bool has_body = code != 204 && code != 304 && size != 0;
  if (has_body) {
    // Responses here are small — /me/player measured 3.8KB — so buffering is
    // safe. Anything large enough to matter is artwork, which streams to SD.
    //
    // No reserve(): it was an optimisation that could throw bad_alloc under
    // heap pressure, and an uncaught throw on the ESP32 is abort(). Assigned
    // directly from the Arduino String rather than round-tripping through
    // c_str(), which allocated the body twice.
    String payload = http.getString();
    out->body.assign(payload.c_str(), payload.length());
  }

  http.end();
  return true;
}
}  // namespace

// The streamed CDN fetch, with the sink left open.
//
// downloadToFile and downloadToMemory share this rather than each carrying a
// copy. The two deadlines below are load-bearing - their absence wedged the net
// task solid in the ancestor project - and two copies of a guard like that is
// how one of them quietly drifts.
namespace {

// Returns false to abort the download.
using Sink = bool (*)(void *ctx, const uint8_t *data, size_t n);

struct MemSink {
  std::vector<uint8_t> *out;
  size_t cap;
};

bool fileSink(void *ctx, const uint8_t *data, size_t n) {
  FILE *f = static_cast<FILE *>(ctx);
  return std::fwrite(data, 1, n, f) == n;
}

bool memSink(void *ctx, const uint8_t *data, size_t n) {
  MemSink *m = static_cast<MemSink *>(ctx);
  if (m->out->size() + n > m->cap) {
    NETLOG("artwork exceeds %u byte cap - aborting", (unsigned)m->cap);
    return false;
  }
  m->out->insert(m->out->end(), data, data + n);
  return true;
}

// `written` and `remaining` are reported back so each caller can say why it
// failed rather than returning a bare false.
bool streamCdn(const std::string &url, Sink sink, void *ctx, int *written_out,
               int *declared_out, int *remaining_out) {
  // Spotify hands out https:// image URLs; fetch them over http:// instead.
  // The CDN serves the same bytes either way - verified: 200 with a correct
  // Content-Length and no redirect - and it means artwork never needs a second
  // TLS session, which is the constraint the whole net design is built around.
  // Do not quietly "fix" this back to https.
  std::string plain = url;
  if (plain.compare(0, 8, "https://") == 0) plain.erase(4, 1);

  NETLOG("artwork GET %s", plain.c_str());
  NETLOG("  heap: free %lu, largest block %lu",
         (unsigned long)ESP.getFreeHeap(),
         (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  // Re-fetched through cdnHttp() after every reset, never held across one: a
  // reset destroys the HTTPClient along with its client, so a reference taken
  // beforehand would dangle.
  auto configure = [](HTTPClient &c) {
    c.setReuse(true);
    c.setConnectTimeout(5000);
    c.setTimeout(10000);
  };

  configure(cdnHttp());
  if (!cdnHttp().begin(cdnClient(), plain.c_str())) {
    NETLOG("artwork begin() refused the url");
    resetCdnSession();
    return false;
  }

  int code = cdnHttp().GET();
  if (code < 0) {
    // A reused connection lost the race with the server's idle close. Rebuild
    // and try once more, or this album is marked failed and never shows a cover.
    NETLOG("artwork GET failed (%s) - retrying on a fresh session",
           HTTPClient::errorToString(code).c_str());
    resetCdnSession();
    configure(cdnHttp());
    if (!cdnHttp().begin(cdnClient(), plain.c_str())) {
      resetCdnSession();
      return false;
    }
    code = cdnHttp().GET();
  }
  if (code != HTTP_CODE_OK) {
    NETLOG("artwork GET -> %d", code);
    resetCdnSession();
    return false;
  }

  // Both bounds below are load-bearing, and their absence wedged the net task
  // solid. http.setTimeout() does NOT apply here: it governs HTTPClient's own
  // reads, and this loop pumps the stream itself. Without a deadline, a CDN that
  // accepts the connection and then goes quiet spins this loop forever at
  // delay(1) - the task stays alive, so nothing looks crashed, it simply never
  // completes another iteration and playback freezes.
  //
  // The connected() test alone is not enough either. On a chunked response
  // getSize() is -1, and keep-alive holds the socket open after the final chunk,
  // so connected() stays true with nothing left to read.
  HTTPClient &http = cdnHttp();
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buf[1024];
  const int declared = http.getSize();
  int remaining = declared;
  int written = 0;
  bool ok = true;

  Deadline no_progress, overall;
  no_progress.arm(millis(), DL_STALL_MS);
  overall.arm(millis(), DL_TOTAL_MS);

  while (remaining != 0) {
    const uint32_t now = millis();
    if (overall.elapsed(now)) {
      NETLOG("artwork download exceeded %ums - aborting", (unsigned)DL_TOTAL_MS);
      ok = false;
      break;
    }
    const size_t avail = stream->available();
    if (avail == 0) {
      // Nothing buffered and the peer is done sending: complete for a chunked
      // response, truncated for a sized one.
      if (!http.connected()) break;
      if (no_progress.elapsed(now)) {
        NETLOG("artwork download stalled %ums with %d left - aborting",
               (unsigned)DL_STALL_MS, remaining);
        ok = false;
        break;
      }
      delay(1);
      continue;
    }
    const int n =
        stream->readBytes(buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n <= 0) {
      NETLOG("artwork read returned %d with %d left - stopping", n, remaining);
      break;
    }
    if (!sink(ctx, buf, static_cast<size_t>(n))) {
      NETLOG("artwork sink refused after %d bytes", written);
      ok = false;
      break;
    }
    written += n;
    if (remaining > 0) remaining -= n;
    no_progress.arm(millis(), DL_STALL_MS);  // progress resets the stall clock
  }

  // Closed rather than held until the next album. Album changes are minutes
  // apart, far past the CDN's ~60s idle close, so a held socket would be dead on
  // arrival and the next download would spend its first attempt finding out.
  // Costs nothing to rebuild now that there is no handshake.
  resetCdnSession();

  if (written_out) *written_out = written;
  if (declared_out) *declared_out = declared;
  if (remaining_out) *remaining_out = remaining;
  return ok && remaining <= 0;
}

}  // namespace

bool downloadToFile(const std::string &url, const std::string &path) {
  // Open the destination BEFORE fetching. Doing the GET first meant that with
  // unusable storage it downloaded ~30KB every poll and threw it away - a
  // permanent request storm against Spotify's CDN.
  const std::string tmp = path + ".part";
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    NETLOG("cannot open %s for write - skipping download", tmp.c_str());
    return false;
  }

  int written = 0, declared = 0, remaining = 0;
  const bool ok = streamCdn(url, fileSink, f, &written, &declared, &remaining);
  std::fclose(f);

  // Promoted only on success, so a truncated download never becomes a cache
  // entry that fails to decode later.
  if (ok) {
    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) == 0) return true;
    NETLOG("artwork rename to %s failed after %d bytes", path.c_str(), written);
    std::remove(tmp.c_str());
    return false;
  }

  NETLOG("artwork incomplete: %d of %d bytes, %d left", written, declared,
         remaining);
  std::remove(tmp.c_str());
  return false;
}

bool downloadToMemory(const std::string &url, std::vector<uint8_t> *out,
                      size_t max_bytes) {
  if (!out) return false;
  out->clear();
  MemSink sink{out, max_bytes};

  int written = 0, declared = 0, remaining = 0;
  const bool ok = streamCdn(url, memSink, &sink, &written, &declared, &remaining);
  if (ok) return true;

  NETLOG("artwork incomplete: %d of %d bytes, %d left", written, declared,
         remaining);
  out->clear();
  return false;
}

}  // namespace http

#endif  // DEVICE
