#include "Log.h"

#include <cstdio>
#include <cstring>

#if defined(DEVICE)
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#include <mutex>
#endif

namespace corelog {
namespace {

// Long enough for a Spotify URL plus a scope list, which are the longest lines
// this project emits. Truncation is marked rather than silent.
constexpr size_t BUF = 512;

#if defined(DEVICE)
SemaphoreHandle_t g_mtx = nullptr;

void ensureMutex() {
  // Created on first use rather than in a constructor: static initialisation
  // order against the Arduino core's own setup is not something to rely on.
  if (!g_mtx) g_mtx = xSemaphoreCreateMutex();
}
#else
std::mutex g_mtx;
#endif

}  // namespace

void vline(const char *fmt, va_list ap) {
  char buf[BUF];
  int n = std::vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
  if (n < 0) return;
  if (n > static_cast<int>(sizeof(buf) - 3)) {
    n = static_cast<int>(sizeof(buf) - 3);
    // Say so, rather than letting a cut line read as a complete one.
    buf[n - 1] = '~';
  }
  buf[n] = '\n';
  buf[n + 1] = '\0';

#if defined(DEVICE)
  ensureMutex();
  if (g_mtx) xSemaphoreTake(g_mtx, portMAX_DELAY);
  Serial.write(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(n + 1));
  if (g_mtx) xSemaphoreGive(g_mtx);
#else
  std::lock_guard<std::mutex> lk(g_mtx);
  std::fwrite(buf, 1, static_cast<size_t>(n + 1), stderr);
#endif
}

void line(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vline(fmt, ap);
  va_end(ap);
}

}  // namespace corelog
