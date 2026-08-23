#include "Bench.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstring>

#include "gfx/Blend.h"
#include "gfx/Geometry.h"

namespace esp32 {
namespace {

constexpr size_t FRAME_PX = static_cast<size_t>(gfx::W) * gfx::H;
constexpr size_t FRAME_BYTES = FRAME_PX * 2;
constexpr int ITERS = 30;

volatile uint32_t g_sink = 0;

// Fade lookup tables for a fixed factor.
//
// fade() costs about fourteen operations per pixel: three masks, three
// multiplies, three shifts, three masks and two ors. Two 256-entry tables
// replace all of it with two loads and an or.
//
// RGB565's green channel straddles the byte boundary, and splitting a multiply
// across that boundary is exact - (a+b)*n == a*n + b*n - but the two truncating
// shifts are not, so green can land one step low. One step of a 6-bit channel
// is invisible; fourteen operations per pixel at 129,600 pixels is not.
uint16_t g_hi[256];
uint16_t g_lo[256];

void buildFadeLut(uint8_t num) {
  for (int i = 0; i < 256; ++i) {
    // High byte: RRRRRGGG
    const uint32_t r = ((i & 0xF8u) * num) >> 8;
    const uint32_t gh = ((i & 0x07u) * num) >> 8;
    g_hi[i] = static_cast<uint16_t>(((r & 0xF8u) << 8) | ((gh & 0x07u) << 8));
    // Low byte: GGGBBBBB
    const uint32_t gl = ((i & 0xE0u) * num) >> 8;
    const uint32_t b = ((i & 0x1Fu) * num) >> 8;
    g_lo[i] = static_cast<uint16_t>((gl & 0xE0u) | (b & 0x1Fu));
  }
}

inline uint16_t fadeLut(uint16_t c) {
  return static_cast<uint16_t>(g_hi[c >> 8] | g_lo[c & 0xFF]);
}

double mbps(size_t bytes_total, int64_t us) {
  if (us <= 0) return 0.0;
  return (static_cast<double>(bytes_total) / 1048576.0) /
         (static_cast<double>(us) / 1000000.0);
}

struct Row {
  const char *name;
  double mb;
  double ms;
};

void report(const char *label, const char *op, size_t bytes, int64_t us) {
  Serial.printf("  %-10s %-16s %7.1f MB/s   %6.2f ms/frame\n", label, op,
                mbps(bytes * ITERS, us),
                (static_cast<double>(us) / 1000.0) / ITERS);
}

void benchBuffer(const char *label, uint16_t *buf) {
  if (!buf) {
    Serial.printf("  %-10s ALLOCATION FAILED\n", label);
    return;
  }
  int64_t t0, t1;

  // --- 16-bit sequential write ---
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it) {
    const uint16_t c = static_cast<uint16_t>(it * 977);
    for (size_t i = 0; i < FRAME_PX; ++i) buf[i] = c;
  }
  t1 = esp_timer_get_time();
  report(label, "write u16", FRAME_BYTES, t1 - t0);

  // --- 32-bit sequential write: two pixels per store ---
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it) {
    const uint16_t c = static_cast<uint16_t>(it * 977);
    const uint32_t cc = (static_cast<uint32_t>(c) << 16) | c;
    uint32_t *p32 = reinterpret_cast<uint32_t *>(buf);
    for (size_t i = 0; i < FRAME_PX / 2; ++i) p32[i] = cc;
  }
  t1 = esp_timer_get_time();
  report(label, "write u32", FRAME_BYTES, t1 - t0);

  // --- memset: whatever the toolchain's optimised routine manages ---
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it) std::memset(buf, it & 0xFF, FRAME_BYTES);
  t1 = esp_timer_get_time();
  report(label, "memset", FRAME_BYTES, t1 - t0);

  // --- 32-bit sequential read ---
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it) {
    uint32_t acc = 0;
    const uint32_t *p32 = reinterpret_cast<const uint32_t *>(buf);
    for (size_t i = 0; i < FRAME_PX / 2; ++i) acc += p32[i];
    g_sink = acc;
  }
  t1 = esp_timer_get_time();
  report(label, "read u32", FRAME_BYTES, t1 - t0);

  // --- fade, arithmetic version (the current implementation) ---
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it)
    for (size_t i = 0; i < FRAME_PX; ++i) buf[i] = gfx::fade(buf[i], 230);
  t1 = esp_timer_get_time();
  report(label, "fade arith", FRAME_BYTES * 2, t1 - t0);

  // --- fade, table version ---
  buildFadeLut(230);
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it)
    for (size_t i = 0; i < FRAME_PX; ++i) buf[i] = fadeLut(buf[i]);
  t1 = esp_timer_get_time();
  report(label, "fade LUT", FRAME_BYTES * 2, t1 - t0);

  // --- fade, table version skipping black ---
  // A particle scene is mostly black, and skipping a pixel that is already
  // zero costs one compare against a load, a table pair and a store.
  std::memset(buf, 0, FRAME_BYTES);
  for (size_t i = 0; i < FRAME_PX; i += 8) buf[i] = 0xFFFF;  // ~12% lit
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it)
    for (size_t i = 0; i < FRAME_PX; ++i) {
      const uint16_t c = buf[i];
      if (c) buf[i] = fadeLut(c);
    }
  t1 = esp_timer_get_time();
  report(label, "fade skip-black", FRAME_BYTES * 2, t1 - t0);

  // --- addSat over the whole frame: the particle blend cost ceiling ---
  t0 = esp_timer_get_time();
  for (int it = 0; it < ITERS; ++it)
    for (size_t i = 0; i < FRAME_PX; ++i)
      buf[i] = gfx::addSat(buf[i], 0x0841);
  t1 = esp_timer_get_time();
  report(label, "addSat", FRAME_BYTES * 2, t1 - t0);

  Serial.println();
}

}  // namespace

void runMemoryBenchmark() {
  Serial.println();
  Serial.println("--- bandwidth, one 360x360 RGB565 frame (259200 B) ---");
  Serial.printf("  %d iterations per measurement\n\n", ITERS);

  uint16_t *ps = static_cast<uint16_t *>(
      heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_SPIRAM));
  benchBuffer("PSRAM", ps);
  if (ps) heap_caps_free(ps);

  uint16_t *in = static_cast<uint16_t *>(
      heap_caps_malloc(FRAME_BYTES, MALLOC_CAP_INTERNAL));
  benchBuffer("internal", in);
  if (in) heap_caps_free(in);

  Serial.println("------------------------------------------------------");
  Serial.println("  Budget at 30fps is 33 ms/frame/core, two cores.");
  Serial.println("  Count how many full-frame passes fit, and fuse the rest.");
  Serial.println();
}

}  // namespace esp32
