// Desktop entry point.
//
// Environment hooks exist so every state is reachable headlessly, which is what
// the visual tests are built on:
//
//   KNOB_DUMP=<path>    write the framebuffer as a 24-bit BMP before exiting
//   KNOB_EXIT_MS=<ms>   quit after this much simulated time
//   KNOB_HEADLESS=1     no window at all; render, dump, exit
//   KNOB_SCALE=<n>      window magnification (default 2)

#include <SDL.h>

#include <cstdlib>
#include <cstring>

#include "core/FrameClock.h"
#include "core/Rng.h"
#include "gfx/Bloom.h"
#include "gfx/CircleMask.h"
#include "gfx/Dither.h"
#include "gfx/Framebuffer.h"
#include "platform/desktop/FrameDump.h"
#include "platform/desktop/SdlPresent.h"

namespace {

const char *envStr(const char *k) { return std::getenv(k); }

int envInt(const char *k, int fallback) {
  const char *v = std::getenv(k);
  return v ? std::atoi(v) : fallback;
}

bool envFlag(const char *k) {
  const char *v = std::getenv(k);
  return v && std::strcmp(v, "0") != 0;
}

}  // namespace

int main(int, char **) {
  const bool headless = envFlag("KNOB_HEADLESS");
  const int exit_ms = envInt("KNOB_EXIT_MS", 0);
  const char *dump = envStr("KNOB_DUMP");

  gfx::Framebuffer fb;
  gfx::Bloom bloom;
  desktop::SdlPresent screen;
  if (!headless && !screen.begin("knob-spotify", envInt("KNOB_SCALE", 2)))
    return 1;

  // Simulated time, not wall-clock: a headless run must produce the same frames
  // every time regardless of how fast the machine is.
  uint32_t sim_ms = 0;
  const uint32_t step_ms = 16;

  for (;;) {
    fb.fill(0x03BF);
    gfx::ditherFrame(fb);
    gfx::maskToCircle(fb);

    if (!headless) {
      if (!screen.pumpEvents()) break;
      screen.present(fb);
      SDL_Delay(step_ms);
    }

    sim_ms += step_ms;
    if (exit_ms > 0 && sim_ms >= static_cast<uint32_t>(exit_ms)) break;
    if (headless && exit_ms == 0) break;
  }

  if (dump) desktop::dumpFrameBmp(fb, dump);
  if (!headless) screen.end();
  return 0;
}
