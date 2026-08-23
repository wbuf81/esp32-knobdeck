# Desktop Renderer, Audio Chain and "Cover Light" — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the complete rendering and audio-reactivity engine for the round Spotify knob player as a desktop (SDL) application, ending in a fully tuned "Cover Light" view — with no ESP32 hardware involved.

**Architecture:** A custom compositor owns a 360x360 native-endian RGB565 framebuffer. Effects draw into it with saturating-additive and fading blend operations, then a bloom pass, a circular mask and an ordered dither run over the result before presentation. Every effect reads its animation inputs from a single `Modulation` struct — assembled from a live FFT of microphone audio, with a procedural fallback — and never touches a clock, the audio device, or playback state directly. That indirection is what makes the whole engine deterministic and testable frame-by-frame.

**Tech Stack:** C++14, PlatformIO (`native` + `test` environments), SDL2 for presentation, Unity for host unit tests, Python 3 for visual regression tests, `stb_image` (vendored, single header) for JPEG decode on desktop.

**Spec:** `docs/superpowers/specs/2026-08-23-knob-spotify-player-design.md`

## Global Constraints

- **C++14.** `-std=c++14`. The device build will later need `-std=gnu++14` with `build_unflags = -std=gnu++11`, because the Arduino ESP32 core appends `-std=gnu++11` which otherwise wins.
- **Screen is 360x360, round.** Visible area is a circle of radius 180 centred at (180, 180). Corners are never visible; never place information there.
- **Framebuffer is native-endian RGB565 throughout.** Byte-swapping happens only at presentation. Any code that byte-swaps mid-pipeline is a bug — see spec section 5.
- **No allocation in per-frame code paths.** All buffers are allocated once at construction. This is a desktop build, but every line here is destined for a device with a fragmenting heap; the spec's ancestor project documents heap fragmentation as its single largest source of field crashes.
- **Effects never read a clock.** The frame loop passes `dt` (seconds, float) and a `Modulation` reference. Effects that need randomness take an `Rng&`. This is what makes headless replay bit-exact, which is what makes visual assertions stable.
- **`dt` is clamped** to a maximum of `0.100f` seconds so a stall cannot teleport a particle field.
- **Presentation is capped at 60 fps**; effects are correct at any frame rate.
- **Unknown values render as unknown.** `volume_pct == -1` and `liked_known == false` must never render as a confident default. This rule is inherited verbatim from the ancestor project's `PlaybackState`.
- **Fonts must cover Latin-1 at minimum.** ASCII-only faces render "Björk" as "Bj?rk", which is unacceptable for a real music library.
- Every task ends with a passing test suite and a commit.

## File Structure

```
platformio.ini                       native (SDL) + test (Unity) environments
src/
  main.cpp                           frame loop, wiring, env-var hooks
  core/
    Rng.h                            xorshift32, explicitly seeded
    Hash.h                           FNV-1a  [ported from ancestor]
    FrameClock.h                     dt production, clamping, 60fps cap
    Deadline.h                       wrap-safe deadline  [ported]
    PlaybackState.h                  plain playback data  [ported]
    AppState.h                       single source of truth  [ported]
    CommandQueue.h                   UI -> source commands  [ported]
    ProgressClock.h                  progress extrapolation  [ported]
    MergePolicy.h                    settle-window merge  [ported]
  gfx/
    Geometry.h                        W, H, RADIUS, CX, CY
    Color.h                           RGB565 pack/unpack/lerp
    Blend.h                           addSat, fade, lerp565
    Framebuffer.h/.cpp                owns the 360x360 buffer
    CircleMask.h/.cpp                 blacken outside the visible disc
    Dither.h/.cpp                     ordered 4x4 dither
    Bloom.h/.cpp                      bright-pass, downscale, blur, upscale-add
    Quad3D.h/.cpp                     perspective-correct textured quad
  fx/
    ParticleSystem.h/.cpp             SoA particle field, additive render
  audio/
    Modulation.h                      the modulation bus struct
    Procedural.h/.cpp                 modulation from progress + seed
    Fft.h/.cpp                        512-point radix-2 complex FFT
    BandEnergy.h/.cpp                 magnitudes -> bass/mid/treble
    OnsetDetector.h/.cpp              spectral flux + adaptive threshold
    TempoTracker.h/.cpp               onset intervals -> bpm + beat phase
    MicSource.h                       audio input interface
    AudioAnalyzer.h/.cpp              the whole chain -> Modulation
  art/
    Image.h                           RGB565 image buffer + loader interface
    Palette.h/.cpp                    16-entry palette + dominant hue
  views/
    View.h                            the view interface
    CoverLight.h/.cpp                 the showcase view
  sources/
    FakeSource.h/.cpp                 deterministic fixture playback  [ported]
  platform/desktop/
    SdlPresent.h/.cpp                 window + texture upload
    WavMic.h/.cpp                     MicSource backed by a WAV file
    JpegLoad.cpp                      Image loader via stb_image
    FrameDump.h/.cpp                  framebuffer -> 24-bit BMP
    stb_image.h                       vendored third-party, unmodified
test/test_logic/main.cpp              Unity host tests
tools/
  make_test_wav.py                    generates synthetic audio fixtures
  visual_tests.py                     pixel-property assertions
assets/art/                           fixture album covers
```

Files are split by responsibility rather than by layer, and each blend/effect
primitive is its own header so the hot-path code can be reviewed and tested in
isolation. `Blend.h` in particular is the single most performance-critical file
in the project and the easiest to get subtly wrong, so it is separated from
everything that uses it.

---

### Task 1: Project skeleton, framebuffer, and a window

**Files:**
- Create: `platformio.ini`
- Create: `src/gfx/Geometry.h`
- Create: `src/gfx/Framebuffer.h`, `src/gfx/Framebuffer.cpp`
- Create: `src/platform/desktop/SdlPresent.h`, `src/platform/desktop/SdlPresent.cpp`
- Create: `src/main.cpp`
- Test: `test/test_logic/main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `gfx::W`, `gfx::H` (`int`, 360), `gfx::RADIUS` (180), `gfx::CX`, `gfx::CY` (180)
  - `class gfx::Framebuffer` — `uint16_t* pixels()`, `const uint16_t* pixels() const`, `void fill(uint16_t)`, `uint16_t at(int x, int y) const`, `void set(int x, int y, uint16_t)`, `static constexpr int width()`, `static constexpr int height()`, `static constexpr size_t count()`
  - `class desktop::SdlPresent` — `bool begin(const char* title, int scale)`, `void present(const gfx::Framebuffer&)`, `bool pumpEvents()` (returns false on quit), `void end()`

- [ ] **Step 1: Write `platformio.ini`**

```ini
; Round-screen Spotify knob player.
;
;   pio run -e native && ./.pio/build/native/program
;   pio test -e test

[platformio]
default_envs = native

[env]
build_flags = -std=c++14 -DAPP_NAME=\"knob-spotify\"

; ---------------------------------------------------------------------------
; Desktop SDL build. This is the primary development target for this plan.
; Apple Silicon Homebrew lives at /opt/homebrew; export HOMEBREW_PREFIX.
; ---------------------------------------------------------------------------
[env:native]
platform = native
build_flags =
    ${env.build_flags}
    -DDESKTOP=1
    -I src
    -I${sysenv.HOMEBREW_PREFIX}/include
    -I${sysenv.HOMEBREW_PREFIX}/include/SDL2
    -L${sysenv.HOMEBREW_PREFIX}/lib
    -lSDL2
    -g
    -O2
build_src_filter = +<*>

; ---------------------------------------------------------------------------
; Host unit tests. No SDL, no hardware: everything under test is display-free
; or operates on a plain memory buffer, which is the point — it runs anywhere
; and in milliseconds.
; ---------------------------------------------------------------------------
[env:test]
platform = native
test_framework = unity
build_flags =
    -std=c++14
    -I src
    -O1
build_src_filter =
    +<gfx/>
    +<fx/>
    +<audio/>
    +<art/>
    +<core/>
    +<sources/>
```

- [ ] **Step 2: Write the failing test**

Create `test/test_logic/main.cpp`:

```cpp
// Host unit tests for the display-free engine.
//
// Everything here runs without SDL, without hardware, and in milliseconds.
//
//   pio test -e test

#include <unity.h>

#include "gfx/Framebuffer.h"

void test_framebuffer_is_360_square(void) {
  TEST_ASSERT_EQUAL_INT(360, gfx::Framebuffer::width());
  TEST_ASSERT_EQUAL_INT(360, gfx::Framebuffer::height());
  TEST_ASSERT_EQUAL_UINT32(129600u, (uint32_t)gfx::Framebuffer::count());
}

void test_framebuffer_fill_sets_every_pixel(void) {
  gfx::Framebuffer fb;
  fb.fill(0xF800);
  TEST_ASSERT_EQUAL_HEX16(0xF800, fb.at(0, 0));
  TEST_ASSERT_EQUAL_HEX16(0xF800, fb.at(359, 359));
  TEST_ASSERT_EQUAL_HEX16(0xF800, fb.at(180, 180));
}

void test_framebuffer_set_is_bounds_checked(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  fb.set(-1, 0, 0xFFFF);      // must not crash or corrupt
  fb.set(360, 0, 0xFFFF);
  fb.set(0, 360, 0xFFFF);
  fb.set(5, 7, 0x07E0);
  TEST_ASSERT_EQUAL_HEX16(0x07E0, fb.at(5, 7));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(0, 0));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_framebuffer_is_360_square);
  RUN_TEST(test_framebuffer_fill_sets_every_pixel);
  RUN_TEST(test_framebuffer_set_is_bounds_checked);
  return UNITY_END();
}
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `HOMEBREW_PREFIX=/opt/homebrew pio test -e test`
Expected: FAIL — `fatal error: 'gfx/Framebuffer.h' file not found`

- [ ] **Step 4: Write `src/gfx/Geometry.h`**

```cpp
#pragma once

// The one place the screen's shape is stated.
//
// The panel is a 360x360 pixel grid behind a round window: only the disc of
// RADIUS centred on (CX, CY) is physically visible. Nothing informative may be
// placed outside it, and CircleMask blackens it at the end of every frame so a
// bug there shows up as a missing corner rather than as invisible garbage.

#include <cstdint>

namespace gfx {

constexpr int W = 360;
constexpr int H = 360;
constexpr int CX = 180;
constexpr int CY = 180;
constexpr int RADIUS = 180;

// Squared radius, so hot loops compare against this without a sqrt.
constexpr int RADIUS_SQ = RADIUS * RADIUS;

}  // namespace gfx
```

- [ ] **Step 5: Write `src/gfx/Framebuffer.h`**

```cpp
#pragma once

// The frame, as plain memory.
//
// Native-endian RGB565, one allocation for the life of the program. The byte
// swap the panel bus wants happens once at presentation and nowhere else, so
// every blend, blur and texture fetch in this project operates on pixels whose
// channels are where they look like they are. The ancestor project stored
// sprites byte-swapped and documented that as the single most confusing trap on
// its board, having been bitten twice; this is that trap designed out.
//
// at()/set() are bounds-checked and are for tests and cold paths. Hot loops
// take pixels() and index it directly, having already clipped.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Geometry.h"

namespace gfx {

class Framebuffer {
 public:
  Framebuffer() : px_(count(), 0) {}

  static constexpr int width() { return W; }
  static constexpr int height() { return H; }
  static constexpr size_t count() { return static_cast<size_t>(W) * H; }

  uint16_t *pixels() { return px_.data(); }
  const uint16_t *pixels() const { return px_.data(); }

  void fill(uint16_t c);

  uint16_t at(int x, int y) const {
    if (x < 0 || y < 0 || x >= W || y >= H) return 0;
    return px_[static_cast<size_t>(y) * W + x];
  }

  void set(int x, int y, uint16_t c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    px_[static_cast<size_t>(y) * W + x] = c;
  }

 private:
  std::vector<uint16_t> px_;
};

}  // namespace gfx
```

- [ ] **Step 6: Write `src/gfx/Framebuffer.cpp`**

```cpp
#include "Framebuffer.h"

namespace gfx {

void Framebuffer::fill(uint16_t c) {
  uint16_t *p = px_.data();
  const size_t n = count();
  for (size_t i = 0; i < n; ++i) p[i] = c;
}

}  // namespace gfx
```

- [ ] **Step 7: Run the test to verify it passes**

Run: `HOMEBREW_PREFIX=/opt/homebrew pio test -e test`
Expected: PASS, 3 tests.

- [ ] **Step 8: Write `src/platform/desktop/SdlPresent.h`**

```cpp
#pragma once

// The desktop's panel: an SDL window that shows the framebuffer.
//
// Deliberately NOT LovyanGFX's Panel_sdl. The ancestor project used it and
// documented an unsynchronised startup race upstream that segfaulted roughly
// one run in eighty. Since this project owns its framebuffer outright, showing
// it is a texture upload and needs none of that machinery.

#include <cstdint>
#include <vector>

#include "gfx/Framebuffer.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace desktop {

class SdlPresent {
 public:
  // scale is an integer window magnification; the texture stays 360x360.
  bool begin(const char *title, int scale);
  void present(const gfx::Framebuffer &fb);

  // Returns false when the user has asked to quit.
  bool pumpEvents();

  void end();

 private:
  SDL_Window *win_ = nullptr;
  SDL_Renderer *ren_ = nullptr;
  SDL_Texture *tex_ = nullptr;
  bool running_ = true;
};

}  // namespace desktop
```

- [ ] **Step 9: Write `src/platform/desktop/SdlPresent.cpp`**

```cpp
#include "SdlPresent.h"

#include <SDL.h>

namespace desktop {

bool SdlPresent::begin(const char *title, int scale) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
  if (scale < 1) scale = 1;

  win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED, gfx::W * scale,
                          gfx::H * scale, SDL_WINDOW_SHOWN);
  if (!win_) return false;

  ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED);
  if (!ren_) return false;

  // RGB565 straight through: no per-pixel conversion on the host, and the
  // bytes we hand SDL are the bytes the device's DMA will push (modulo the
  // swap the panel bus wants, which is the device's business alone).
  tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGB565,
                           SDL_TEXTUREACCESS_STREAMING, gfx::W, gfx::H);
  return tex_ != nullptr;
}

void SdlPresent::present(const gfx::Framebuffer &fb) {
  SDL_UpdateTexture(tex_, nullptr, fb.pixels(), gfx::W * 2);
  SDL_RenderClear(ren_);
  SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
  SDL_RenderPresent(ren_);
}

bool SdlPresent::pumpEvents() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) running_ = false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q) running_ = false;
  }
  return running_;
}

void SdlPresent::end() {
  if (tex_) SDL_DestroyTexture(tex_);
  if (ren_) SDL_DestroyRenderer(ren_);
  if (win_) SDL_DestroyWindow(win_);
  tex_ = nullptr;
  ren_ = nullptr;
  win_ = nullptr;
  SDL_Quit();
}

}  // namespace desktop
```

- [ ] **Step 10: Write `src/main.cpp`**

```cpp
// Desktop entry point.
//
// This grows into the real frame loop over the course of this plan. Right now
// it proves the build, the window and the framebuffer agree with each other.

#include <SDL.h>

#include "gfx/Framebuffer.h"
#include "platform/desktop/SdlPresent.h"

int main(int, char **) {
  desktop::SdlPresent screen;
  if (!screen.begin("knob-spotify", 2)) return 1;

  gfx::Framebuffer fb;
  fb.fill(0x03BF);  // a mid teal, so "it drew nothing" and "it drew black" differ

  while (screen.pumpEvents()) {
    screen.present(fb);
    SDL_Delay(16);
  }

  screen.end();
  return 0;
}
```

- [ ] **Step 11: Build and run**

Run: `HOMEBREW_PREFIX=/opt/homebrew pio run -e native && ./.pio/build/native/program`
Expected: a 720x720 window (360x360 at scale 2) filled with mid teal. `q` or the close button exits.

- [ ] **Step 12: Commit**

```bash
git add platformio.ini src test
git commit -m "Framebuffer and a window: 360x360 native-endian RGB565, shown by SDL

Not LovyanGFX's Panel_sdl - owning the framebuffer outright makes showing
it a texture upload, and avoids the upstream startup race the ancestor
project hit one run in eighty."
```

---

### Task 2: Frame dump and the visual-test harness

Established now rather than at the end, so every subsequent task can assert on
pixels. The ancestor project's note is the justification: *"Every display bug
found in this project was invisible to logic tests and obvious in a
screenshot."*

**Files:**
- Create: `src/platform/desktop/FrameDump.h`, `src/platform/desktop/FrameDump.cpp`
- Create: `tools/visual_tests.py`
- Modify: `src/main.cpp`

**Interfaces:**
- Consumes: `gfx::Framebuffer` (Task 1).
- Produces:
  - `bool desktop::dumpFrameBmp(const gfx::Framebuffer&, const char* path)` — writes a 24-bit BMP.
  - Environment hooks honoured by `main`: `KNOB_DUMP=<path>`, `KNOB_EXIT_MS=<ms>`, `KNOB_HEADLESS=1`.
  - `tools/visual_tests.py` helpers: `read_bmp(path)`, `run_case(env)`, `region_mean(px, rect)`, `region_max(px, rect)`, `nonblack_fraction(px, rect)`.

- [ ] **Step 1: Write the failing test**

Append to `test/test_logic/main.cpp` (and add `RUN_TEST` lines in `main`):

```cpp
#include "platform/desktop/FrameDump.h"
#include <cstdio>

void test_frame_dump_writes_a_readable_bmp(void) {
  gfx::Framebuffer fb;
  fb.fill(0xF800);  // pure red
  const char *path = "/tmp/knob_test_dump.bmp";
  TEST_ASSERT_TRUE(desktop::dumpFrameBmp(fb, path));

  FILE *f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL(f);
  unsigned char hdr[54] = {0};
  TEST_ASSERT_EQUAL_UINT32(54u, (uint32_t)std::fread(hdr, 1, 54, f));
  std::fclose(f);

  TEST_ASSERT_EQUAL_UINT8('B', hdr[0]);
  TEST_ASSERT_EQUAL_UINT8('M', hdr[1]);
  // Width and height at offsets 18 and 22, little-endian int32.
  const uint32_t w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
  const uint32_t h = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
  TEST_ASSERT_EQUAL_UINT32(360u, w);
  TEST_ASSERT_EQUAL_UINT32(360u, h);
}
```

Note: `FrameDump.cpp` lives under `platform/desktop/`, which the `test`
environment's `build_src_filter` does not include. Add `+<platform/desktop/FrameDump.cpp>`
to that filter in `platformio.ini` as part of this step — it is the one desktop
file with no SDL dependency, which is exactly why the BMP writer takes a
`Framebuffer` rather than reading a panel back.

- [ ] **Step 2: Run the test to verify it fails**

Run: `HOMEBREW_PREFIX=/opt/homebrew pio test -e test`
Expected: FAIL — `'platform/desktop/FrameDump.h' file not found`

- [ ] **Step 3: Write `src/platform/desktop/FrameDump.h`**

```cpp
#pragma once

// Framebuffer -> 24-bit BMP.
//
// Screen-capturing the SDL window means hunting for its position and fighting
// Retina scaling. Writing the buffer out gives an exact 360x360 image, which is
// what makes visual regression checks possible at all.
//
// Takes a Framebuffer rather than reading the panel back: it therefore has no
// SDL dependency and runs inside the unit tests.

#include "gfx/Framebuffer.h"

namespace desktop {

bool dumpFrameBmp(const gfx::Framebuffer &fb, const char *path);

}  // namespace desktop
```

- [ ] **Step 4: Write `src/platform/desktop/FrameDump.cpp`**

```cpp
#include "FrameDump.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace desktop {
namespace {

void put16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
  v.push_back(x & 0xff);
  v.push_back((x >> 8) & 0xff);
  v.push_back((x >> 16) & 0xff);
  v.push_back((x >> 24) & 0xff);
}

}  // namespace

bool dumpFrameBmp(const gfx::Framebuffer &fb, const char *path) {
  const int w = gfx::Framebuffer::width();
  const int h = gfx::Framebuffer::height();
  const int row_bytes = w * 3;
  const int pad = (4 - (row_bytes % 4)) % 4;
  const uint32_t image_bytes = static_cast<uint32_t>((row_bytes + pad) * h);

  std::vector<uint8_t> out;
  out.reserve(54 + image_bytes);

  out.push_back('B');
  out.push_back('M');
  put32(out, 54 + image_bytes);
  put32(out, 0);
  put32(out, 54);

  put32(out, 40);
  put32(out, static_cast<uint32_t>(w));
  put32(out, static_cast<uint32_t>(h));
  put16(out, 1);
  put16(out, 24);
  put32(out, 0);
  put32(out, image_bytes);
  put32(out, 2835);
  put32(out, 2835);
  put32(out, 0);
  put32(out, 0);

  // BMP rows run bottom-up and pixels are BGR. The framebuffer is
  // native-endian RGB565, so channels unpack directly with no swap - unlike the
  // ancestor project, whose equivalent function had to swap first.
  const uint16_t *px = fb.pixels();
  for (int y = h - 1; y >= 0; --y) {
    for (int x = 0; x < w; ++x) {
      const uint16_t c = px[static_cast<size_t>(y) * w + x];
      const uint8_t r5 = (c >> 11) & 0x1f;
      const uint8_t g6 = (c >> 5) & 0x3f;
      const uint8_t b5 = c & 0x1f;
      out.push_back(static_cast<uint8_t>((b5 << 3) | (b5 >> 2)));
      out.push_back(static_cast<uint8_t>((g6 << 2) | (g6 >> 4)));
      out.push_back(static_cast<uint8_t>((r5 << 3) | (r5 >> 2)));
    }
    for (int p = 0; p < pad; ++p) out.push_back(0);
  }

  FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  const size_t n = std::fwrite(out.data(), 1, out.size(), f);
  std::fclose(f);
  return n == out.size();
}

}  // namespace desktop
```

- [ ] **Step 5: Run the test to verify it passes**

Run: `HOMEBREW_PREFIX=/opt/homebrew pio test -e test`
Expected: PASS, 4 tests.

- [ ] **Step 6: Add the environment hooks to `src/main.cpp`**

Replace `src/main.cpp` with:

```cpp
// Desktop entry point.
//
// Environment hooks exist so every state is reachable headlessly, which is what
// the visual tests are built on:
//
//   KNOB_DUMP=<path>    write the framebuffer as a 24-bit BMP before exiting
//   KNOB_EXIT_MS=<ms>   quit after this much simulated time
//   KNOB_HEADLESS=1     no window at all; render, dump, exit

#include <SDL.h>

#include <cstdlib>
#include <cstring>

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
  desktop::SdlPresent screen;
  if (!headless && !screen.begin("knob-spotify", 2)) return 1;

  // Simulated time, not wall-clock: a headless run must produce the same frames
  // every time regardless of how fast the machine is.
  uint32_t sim_ms = 0;
  const uint32_t step_ms = 16;

  for (;;) {
    fb.fill(0x03BF);

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
```

- [ ] **Step 7: Write `tools/visual_tests.py`**

```python
#!/usr/bin/env python3
"""Visual regression tests against real framebuffers.

The unit tests cover logic. These cover rendering, because display bugs are
invisible to logic tests and obvious in a screenshot.

No golden images: those rot every time a colour or a font changes. These assert
on properties that should hold regardless.

  python3 tools/visual_tests.py
"""

import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, ".pio", "build", "native", "program")

W = H = 360
CX = CY = 180
RADIUS = 180

FAILURES = []


def read_bmp(path):
    """Return px[y][x] = (r, g, b), top-down."""
    d = open(path, "rb").read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    row = w * 3
    pad = (4 - row % 4) % 4
    px = [[None] * w for _ in range(h)]
    for ry in range(h):
        base = off + ry * (row + pad)
        y = h - 1 - ry
        for x in range(w):
            i = base + x * 3
            px[y][x] = (d[i + 2], d[i + 1], d[i])
    return px


def run_case(env=None, exit_ms=1000):
    """Run the binary headless and return the decoded framebuffer."""
    e = dict(os.environ)
    e["KNOB_HEADLESS"] = "1"
    e["KNOB_EXIT_MS"] = str(exit_ms)
    if env:
        e.update({k: str(v) for k, v in env.items()})
    fd, path = tempfile.mkstemp(suffix=".bmp")
    os.close(fd)
    e["KNOB_DUMP"] = path
    r = subprocess.run([BIN], env=e, capture_output=True, timeout=120)
    if r.returncode != 0:
        raise RuntimeError(
            "binary exited %d: %s" % (r.returncode, r.stderr.decode()[-2000:])
        )
    px = read_bmp(path)
    os.unlink(path)
    return px


def region_pixels(px, rect):
    x0, y0, w, h = rect
    return [px[y][x] for y in range(y0, y0 + h) for x in range(x0, x0 + w)]


def region_mean(px, rect):
    ps = region_pixels(px, rect)
    n = len(ps)
    return tuple(sum(p[i] for p in ps) / n for i in range(3))


def region_max(px, rect):
    ps = region_pixels(px, rect)
    return tuple(max(p[i] for p in ps) for i in range(3))


def nonblack_fraction(px, rect, threshold=12):
    ps = region_pixels(px, rect)
    lit = sum(1 for p in ps if max(p) > threshold)
    return lit / len(ps)


def check(name, condition, detail=""):
    if condition:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s  %s" % (name, detail))
        FAILURES.append(name)


# ---------------------------------------------------------------------------
# Cases
# ---------------------------------------------------------------------------


def case_window_renders_something():
    px = run_case()
    check(
        "frame is not entirely black",
        nonblack_fraction(px, (0, 0, W, H)) > 0.5,
        "the render produced a black or near-black frame",
    )


def main():
    if not os.path.exists(BIN):
        print("build first: HOMEBREW_PREFIX=/opt/homebrew pio run -e native")
        return 1
    for fn in sorted(k for k in globals() if k.startswith("case_")):
        print(fn[5:])
        globals()[fn]()
    print()
    if FAILURES:
        print("%d visual check(s) failed" % len(FAILURES))
        return 1
    print("all visual checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 8: Run both suites**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test
HOMEBREW_PREFIX=/opt/homebrew pio run -e native
python3 tools/visual_tests.py
```
Expected: unit tests PASS (4), visual tests PASS (1 case).

- [ ] **Step 9: Commit**

```bash
git add src tools platformio.ini test
git commit -m "Frame dump and a visual test harness, before there is anything to look at

Every display bug in the ancestor project was invisible to logic tests
and obvious in a screenshot, so the screenshot harness comes first and
every later task can assert on pixels."
```

---

### Task 3: Colour and blend operations

The hottest and most easily-wrong file in the project, hence its own task.

**Files:**
- Create: `src/gfx/Color.h`, `src/gfx/Blend.h`
- Modify: `test/test_logic/main.cpp`

**Interfaces:**
- Produces (all `inline`, `namespace gfx`):
  - `uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)` — from 8-bit channels
  - `void unpack565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b)` — to 8-bit channels
  - `uint16_t addSat(uint16_t a, uint16_t b)` — per-channel saturating add
  - `uint16_t fade(uint16_t c, uint8_t num)` — multiply by `num/256`
  - `uint16_t lerp565(uint16_t a, uint16_t b, uint16_t t)` — `t` in `0..256`, 0 = `a`

- [ ] **Step 1: Write the failing tests**

```cpp
#include "gfx/Blend.h"
#include "gfx/Color.h"

void test_rgb565_roundtrips_channel_extremes(void) {
  TEST_ASSERT_EQUAL_HEX16(0xF800, gfx::rgb565(255, 0, 0));
  TEST_ASSERT_EQUAL_HEX16(0x07E0, gfx::rgb565(0, 255, 0));
  TEST_ASSERT_EQUAL_HEX16(0x001F, gfx::rgb565(0, 0, 255));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, gfx::rgb565(255, 255, 255));
  TEST_ASSERT_EQUAL_HEX16(0x0000, gfx::rgb565(0, 0, 0));
}

void test_add_sat_clamps_each_channel_independently(void) {
  // Red saturates; green and blue must be untouched by the overflow.
  const uint16_t red = gfx::rgb565(255, 0, 0);
  const uint16_t both = gfx::addSat(red, red);
  TEST_ASSERT_EQUAL_HEX16(0xF800, both);

  // A channel overflowing must not bleed into its neighbour. This is the bug
  // that produces rainbow noise instead of a glow.
  const uint16_t hot = gfx::addSat(gfx::rgb565(200, 200, 200),
                                   gfx::rgb565(200, 200, 200));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, hot);
}

void test_add_sat_is_identity_against_black(void) {
  const uint16_t c = gfx::rgb565(90, 140, 30);
  TEST_ASSERT_EQUAL_HEX16(c, gfx::addSat(c, 0x0000));
  TEST_ASSERT_EQUAL_HEX16(c, gfx::addSat(0x0000, c));
}

void test_fade_converges_to_black(void) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < 200; ++i) c = gfx::fade(c, 230);
  TEST_ASSERT_EQUAL_HEX16(0x0000, c);
}

void test_fade_by_zero_is_black_and_full_is_near_identity(void) {
  TEST_ASSERT_EQUAL_HEX16(0x0000, gfx::fade(0xFFFF, 0));
  // 256/256 is not representable in a uint8_t, so 255 is the maximum and is
  // deliberately a hair below identity - that hair is what makes trails decay.
  const uint16_t nearly = gfx::fade(0xFFFF, 255);
  TEST_ASSERT_TRUE(nearly > 0xF000);
}

void test_lerp565_hits_both_endpoints_exactly(void) {
  const uint16_t a = gfx::rgb565(255, 0, 0);
  const uint16_t b = gfx::rgb565(0, 0, 255);
  TEST_ASSERT_EQUAL_HEX16(a, gfx::lerp565(a, b, 0));
  TEST_ASSERT_EQUAL_HEX16(b, gfx::lerp565(a, b, 256));
}

void test_lerp565_midpoint_is_between(void) {
  const uint16_t mid = gfx::lerp565(gfx::rgb565(255, 0, 0),
                                    gfx::rgb565(0, 0, 255), 128);
  uint8_t r, g, b;
  gfx::unpack565(mid, r, g, b);
  TEST_ASSERT_TRUE(r > 100 && r < 160);
  TEST_ASSERT_TRUE(b > 100 && b < 160);
  TEST_ASSERT_TRUE(g < 20);
}
```

- [ ] **Step 2: Run to verify failure.** `pio test -e test` → `'gfx/Blend.h' file not found`

- [ ] **Step 3: Write `src/gfx/Color.h`**

```cpp
#pragma once

// RGB565 packing, native-endian.

#include <cstdint>

namespace gfx {

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Expands back to 8-bit with the top bits replicated into the low ones, so
// white round-trips to 255 rather than 248.
inline void unpack565(uint16_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  const uint8_t r5 = (c >> 11) & 0x1F;
  const uint8_t g6 = (c >> 5) & 0x3F;
  const uint8_t b5 = c & 0x1F;
  r = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
  g = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
  b = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
}

}  // namespace gfx
```

- [ ] **Step 4: Write `src/gfx/Blend.h`**

```cpp
#pragma once

// Per-pixel blend operations. The hot path of every effect in the project.
//
// All three operate on packed RGB565 without unpacking to separate channels,
// because unpack-blend-repack is roughly three times the instruction count and
// these run tens of millions of times per second. The masking is what keeps
// channels from bleeding into each other; getting that wrong produces rainbow
// noise rather than a subtle error, which is at least an honest failure.

#include <cstdint>

namespace gfx {

// Per-channel saturating add. This is what turns particles into light rather
// than dots, and it is the single most visually important function here.
inline uint16_t addSat(uint16_t a, uint16_t b) {
  uint32_t r = (a & 0xF800u) + (b & 0xF800u);
  uint32_t g = (a & 0x07E0u) + (b & 0x07E0u);
  uint32_t bl = (a & 0x001Fu) + (b & 0x001Fu);
  if (r > 0xF800u) r = 0xF800u;
  if (g > 0x07E0u) g = 0x07E0u;
  if (bl > 0x001Fu) bl = 0x001Fu;
  return static_cast<uint16_t>(r | g | bl);
}

// Multiply toward black by num/256. Used once per frame over the persistence
// buffer, which is what turns moving particles into comet trails.
inline uint16_t fade(uint16_t c, uint8_t num) {
  const uint32_t r = ((c & 0xF800u) * num) >> 8;
  const uint32_t g = ((c & 0x07E0u) * num) >> 8;
  const uint32_t b = ((c & 0x001Fu) * num) >> 8;
  return static_cast<uint16_t>((r & 0xF800u) | (g & 0x07E0u) | (b & 0x001Fu));
}

// Linear interpolation, t in 0..256 so both endpoints are exact. A 0..255
// range cannot represent "all of b" and quietly darkens every full-strength
// blend by one step.
inline uint16_t lerp565(uint16_t a, uint16_t b, uint16_t t) {
  if (t > 256) t = 256;
  const uint32_t it = 256u - t;
  const uint32_t ar = (a >> 11) & 0x1Fu, ag = (a >> 5) & 0x3Fu, ab = a & 0x1Fu;
  const uint32_t br = (b >> 11) & 0x1Fu, bg = (b >> 5) & 0x3Fu, bb = b & 0x1Fu;
  const uint32_t r = (ar * it + br * t) >> 8;
  const uint32_t g = (ag * it + bg * t) >> 8;
  const uint32_t bl = (ab * it + bb * t) >> 8;
  return static_cast<uint16_t>((r << 11) | (g << 5) | bl);
}

}  // namespace gfx
```

- [ ] **Step 5: Run to verify pass.** Expected: PASS, 11 tests.

- [ ] **Step 6: Commit**

```bash
git add src/gfx test
git commit -m "Blend ops: saturating add, fade, exact-endpoint lerp

Packed-RGB565 arithmetic rather than unpack-blend-repack, because these
run tens of millions of times a second. Channel masking is what stops a
saturating red from bleeding into green as rainbow noise."
```

---

### Task 4: Circular mask and ordered dither

**Files:**
- Create: `src/gfx/CircleMask.h`, `src/gfx/CircleMask.cpp`, `src/gfx/Dither.h`, `src/gfx/Dither.cpp`
- Modify: `test/test_logic/main.cpp`, `tools/visual_tests.py`

**Interfaces:**
- `void gfx::maskToCircle(Framebuffer& fb)` — sets every pixel outside `RADIUS` to 0.
- `void gfx::ditherFrame(Framebuffer& fb)` — applies a 4x4 ordered dither in place; deterministic, changes each channel by at most one LSB.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "gfx/CircleMask.h"
#include "gfx/Dither.h"

void test_mask_blackens_corners_and_keeps_centre(void) {
  gfx::Framebuffer fb;
  fb.fill(0xFFFF);
  gfx::maskToCircle(fb);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(0, 0));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(359, 0));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(0, 359));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(359, 359));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(180, 180));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(180, 5));    // top of the disc
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(5, 180));    // left of the disc
}

void test_dither_is_deterministic(void) {
  gfx::Framebuffer a, b;
  a.fill(0x4208);
  b.fill(0x4208);
  gfx::ditherFrame(a);
  gfx::ditherFrame(b);
  for (int y = 0; y < 360; y += 37)
    for (int x = 0; x < 360; x += 41)
      TEST_ASSERT_EQUAL_HEX16(a.at(x, y), b.at(x, y));
}

void test_dither_perturbs_by_at_most_one_step_per_channel(void) {
  gfx::Framebuffer fb;
  const uint16_t base = gfx::rgb565(100, 100, 100);
  fb.fill(base);
  gfx::ditherFrame(fb);
  uint8_t br, bg, bb;
  gfx::unpack565(base, br, bg, bb);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      uint8_t r, g, b;
      gfx::unpack565(fb.at(x, y), r, g, b);
      TEST_ASSERT_TRUE(abs((int)r - (int)br) <= 9);  // one 5-bit step is 8
      TEST_ASSERT_TRUE(abs((int)g - (int)bg) <= 5);  // one 6-bit step is 4
      TEST_ASSERT_TRUE(abs((int)b - (int)bb) <= 9);
    }
  }
}

void test_dither_leaves_black_and_white_alone(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::ditherFrame(fb);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(3, 3));
  fb.fill(0xFFFF);
  gfx::ditherFrame(fb);
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(3, 3));
}
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Write `src/gfx/CircleMask.h` / `.cpp`**

```cpp
// CircleMask.h
#pragma once
#include "Framebuffer.h"
namespace gfx {
// Blackens everything outside the visible disc. Runs last, so any effect that
// overdraws the corners fails as a missing edge rather than as garbage behind
// the bezel.
void maskToCircle(Framebuffer &fb);
}  // namespace gfx
```

```cpp
// CircleMask.cpp
#include "CircleMask.h"

namespace gfx {

void maskToCircle(Framebuffer &fb) {
  uint16_t *px = fb.pixels();
  for (int y = 0; y < H; ++y) {
    const int dy = y - CY;
    // Half-chord at this row: the largest dx with dx^2 + dy^2 <= RADIUS^2.
    const int rem = RADIUS_SQ - dy * dy;
    uint16_t *row = px + static_cast<size_t>(y) * W;
    if (rem <= 0) {
      for (int x = 0; x < W; ++x) row[x] = 0;
      continue;
    }
    int half = 0;
    while ((half + 1) * (half + 1) <= rem) ++half;
    const int x0 = CX - half;
    const int x1 = CX + half;
    for (int x = 0; x < x0; ++x) row[x] = 0;
    for (int x = x1 + 1; x < W; ++x) row[x] = 0;
  }
}

}  // namespace gfx
```

```cpp
// Dither.h
#pragma once
#include "Framebuffer.h"
namespace gfx {
// 4x4 ordered dither.
//
// The panel is 262K colour (18-bit) and the framebuffer is RGB565, so smooth
// gradients - which this project is full of - band visibly. Dithering trades
// that for imperceptible noise. Applied after bloom and before the mask.
void ditherFrame(Framebuffer &fb);
}  // namespace gfx
```

```cpp
// Dither.cpp
#include "Dither.h"

#include "Blend.h"
#include "Color.h"

namespace gfx {
namespace {

// Bayer 4x4, centred so the mean perturbation is zero and the image neither
// brightens nor darkens overall.
const int8_t kBayer[4][4] = {
    {-8, 0, -6, 2},
    {4, -4, 6, -2},
    {-5, 3, -7, 1},
    {7, -1, 5, -3},
};

inline uint8_t clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

void ditherFrame(Framebuffer &fb) {
  uint16_t *px = fb.pixels();
  for (int y = 0; y < H; ++y) {
    uint16_t *row = px + static_cast<size_t>(y) * W;
    const int8_t *bay = kBayer[y & 3];
    for (int x = 0; x < W; ++x) {
      const uint16_t c = row[x];
      if (c == 0x0000 || c == 0xFFFF) continue;  // nothing to dither
      uint8_t r, g, b;
      unpack565(c, r, g, b);
      const int d = bay[x & 3];
      row[x] = rgb565(clamp8(r + d), clamp8(g + (d >> 1)), clamp8(b + d));
    }
  }
}

}  // namespace gfx
```

- [ ] **Step 4: Run to verify pass.** Expected PASS, 15 tests.

- [ ] **Step 5: Add a visual case to `tools/visual_tests.py`**

```python
def case_corners_are_masked():
    px = run_case()
    for name, (x, y) in [
        ("top-left", (4, 4)),
        ("top-right", (355, 4)),
        ("bottom-left", (4, 355)),
        ("bottom-right", (355, 355)),
    ]:
        check(
            "%s corner is black" % name,
            max(px[y][x]) < 8,
            "corner pixel was %r; the circular mask did not run" % (px[y][x],),
        )
    check(
        "centre is lit",
        max(px[CY][CX]) > 16,
        "the mask blackened the visible disc as well",
    )
```

This requires `main.cpp` to call `gfx::maskToCircle(fb)` and `gfx::ditherFrame(fb)`
after filling — add both calls in this step.

- [ ] **Step 6: Commit**

```bash
git add src/gfx test tools
git commit -m "Circular mask and ordered dither

The panel is a 360x360 grid behind a round window, so the mask runs last
and an effect that overdraws fails as a missing edge. Dither is Bayer 4x4
centred on zero, because an 18-bit panel bands RGB565 gradients visibly."
```

---

### Task 5: Bloom

**Files:**
- Create: `src/gfx/Bloom.h`, `src/gfx/Bloom.cpp`
- Modify: `test/test_logic/main.cpp`

**Interfaces:**
- `class gfx::Bloom` — `void apply(Framebuffer& fb, uint8_t threshold, uint8_t strength)`.
  Allocates its scratch buffers once in the constructor. `threshold` is the
  8-bit luma below which a pixel contributes nothing; `strength` scales the
  added glow (`0..255`).
- `static constexpr int gfx::Bloom::SMALL_W = 90`, `SMALL_H = 90`.

The single highest visual-impact-per-cycle operation in the project. It runs at
1/16 area, which nobody can perceive and which makes it affordable.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "gfx/Bloom.h"

void test_bloom_leaves_a_black_frame_black(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Bloom bloom;
  bloom.apply(fb, 40, 200);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(180, 180));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(90, 270));
}

void test_bloom_spreads_a_bright_point_to_its_neighbourhood(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  // A 4x4 white block, so it survives the 4x downscale.
  for (int y = 178; y < 182; ++y)
    for (int x = 178; x < 182; ++x) fb.set(x, y, 0xFFFF);

  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(196, 180));
  gfx::Bloom bloom;
  bloom.apply(fb, 40, 255);
  // Energy must have reached pixels that were black, several pixels away.
  TEST_ASSERT_TRUE(fb.at(196, 180) > 0x0000);
  TEST_ASSERT_TRUE(fb.at(180, 196) > 0x0000);
}

void test_bloom_ignores_pixels_below_threshold(void) {
  gfx::Framebuffer fb;
  const uint16_t dim = gfx::rgb565(20, 20, 20);
  fb.fill(dim);
  gfx::Bloom bloom;
  bloom.apply(fb, 200, 255);  // threshold well above the fill
  TEST_ASSERT_EQUAL_HEX16(dim, fb.at(180, 180));
}

void test_bloom_never_darkens(void) {
  gfx::Framebuffer fb;
  const uint16_t c = gfx::rgb565(120, 60, 200);
  fb.fill(c);
  gfx::Bloom bloom;
  bloom.apply(fb, 40, 128);
  uint8_t r0, g0, b0, r1, g1, b1;
  gfx::unpack565(c, r0, g0, b0);
  gfx::unpack565(fb.at(180, 180), r1, g1, b1);
  TEST_ASSERT_TRUE(r1 >= r0 && g1 >= g0 && b1 >= b0);
}
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Write `src/gfx/Bloom.h`**

```cpp
#pragma once

// Bloom: the cheapest way to make drawn things look lit.
//
// bright-pass -> 4x downscale to 90x90 -> two box-blur passes -> bilinear
// upscale, added back saturating. Running the blur at 1/16 the area is what
// makes it affordable; at this radius nobody can tell it was low resolution.
//
// Scratch buffers are allocated once here and never per frame. On the device
// this file's allocation pattern is the difference between working and a
// fragmentation reboot loop ten minutes in.

#include <cstdint>
#include <vector>

#include "Framebuffer.h"

namespace gfx {

class Bloom {
 public:
  static constexpr int SMALL_W = 90;
  static constexpr int SMALL_H = 90;

  Bloom();

  // threshold: 8-bit luma floor for contributing to the glow.
  // strength:  0..255 scale on the added glow.
  void apply(Framebuffer &fb, uint8_t threshold, uint8_t strength);

 private:
  void brightPassDownscale(const Framebuffer &fb, uint8_t threshold);
  void blurPass(std::vector<uint16_t> &src, std::vector<uint16_t> &dst);
  void upscaleAdd(Framebuffer &fb, uint8_t strength);

  std::vector<uint16_t> small_;
  std::vector<uint16_t> tmp_;
};

}  // namespace gfx
```

- [ ] **Step 4: Write `src/gfx/Bloom.cpp`**

```cpp
#include "Bloom.h"

#include "Blend.h"
#include "Color.h"

namespace gfx {
namespace {

constexpr int SW = Bloom::SMALL_W;
constexpr int SH = Bloom::SMALL_H;
constexpr int SCALE = W / SW;  // 4

inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b) {
  // Integer approximation of Rec.601: (77r + 150g + 29b) / 256.
  return static_cast<uint8_t>((77 * r + 150 * g + 29 * b) >> 8);
}

}  // namespace

Bloom::Bloom()
    : small_(static_cast<size_t>(SW) * SH, 0),
      tmp_(static_cast<size_t>(SW) * SH, 0) {}

void Bloom::brightPassDownscale(const Framebuffer &fb, uint8_t threshold) {
  const uint16_t *px = fb.pixels();
  for (int sy = 0; sy < SH; ++sy) {
    for (int sx = 0; sx < SW; ++sx) {
      uint32_t ar = 0, ag = 0, ab = 0;
      for (int y = 0; y < SCALE; ++y) {
        const uint16_t *row = px + static_cast<size_t>(sy * SCALE + y) * W +
                              static_cast<size_t>(sx) * SCALE;
        for (int x = 0; x < SCALE; ++x) {
          uint8_t r, g, b;
          unpack565(row[x], r, g, b);
          if (luma(r, g, b) < threshold) continue;
          ar += r;
          ag += g;
          ab += b;
        }
      }
      const uint32_t n = SCALE * SCALE;
      small_[static_cast<size_t>(sy) * SW + sx] = rgb565(
          static_cast<uint8_t>(ar / n), static_cast<uint8_t>(ag / n),
          static_cast<uint8_t>(ab / n));
    }
  }
}

// Separable 3-tap box blur, run as one horizontal then one vertical sweep.
void Bloom::blurPass(std::vector<uint16_t> &src, std::vector<uint16_t> &dst) {
  // Horizontal.
  for (int y = 0; y < SH; ++y) {
    for (int x = 0; x < SW; ++x) {
      const int xm = x > 0 ? x - 1 : 0;
      const int xp = x < SW - 1 ? x + 1 : SW - 1;
      uint8_t r0, g0, b0, r1, g1, b1, r2, g2, b2;
      unpack565(src[static_cast<size_t>(y) * SW + xm], r0, g0, b0);
      unpack565(src[static_cast<size_t>(y) * SW + x], r1, g1, b1);
      unpack565(src[static_cast<size_t>(y) * SW + xp], r2, g2, b2);
      dst[static_cast<size_t>(y) * SW + x] =
          rgb565(static_cast<uint8_t>((r0 + r1 + r1 + r2) >> 2),
                 static_cast<uint8_t>((g0 + g1 + g1 + g2) >> 2),
                 static_cast<uint8_t>((b0 + b1 + b1 + b2) >> 2));
    }
  }
  // Vertical, back into src.
  for (int y = 0; y < SH; ++y) {
    const int ym = y > 0 ? y - 1 : 0;
    const int yp = y < SH - 1 ? y + 1 : SH - 1;
    for (int x = 0; x < SW; ++x) {
      uint8_t r0, g0, b0, r1, g1, b1, r2, g2, b2;
      unpack565(dst[static_cast<size_t>(ym) * SW + x], r0, g0, b0);
      unpack565(dst[static_cast<size_t>(y) * SW + x], r1, g1, b1);
      unpack565(dst[static_cast<size_t>(yp) * SW + x], r2, g2, b2);
      src[static_cast<size_t>(y) * SW + x] =
          rgb565(static_cast<uint8_t>((r0 + r1 + r1 + r2) >> 2),
                 static_cast<uint8_t>((g0 + g1 + g1 + g2) >> 2),
                 static_cast<uint8_t>((b0 + b1 + b1 + b2) >> 2));
    }
  }
}

void Bloom::upscaleAdd(Framebuffer &fb, uint8_t strength) {
  uint16_t *px = fb.pixels();
  for (int y = 0; y < H; ++y) {
    // Bilinear source coordinate in 8.8 fixed point.
    const int fy = (y * 256) / SCALE;
    const int sy0 = fy >> 8;
    const int sy1 = sy0 < SH - 1 ? sy0 + 1 : SH - 1;
    const uint16_t wy = static_cast<uint16_t>(fy & 0xFF);
    uint16_t *row = px + static_cast<size_t>(y) * W;
    for (int x = 0; x < W; ++x) {
      const int fx = (x * 256) / SCALE;
      const int sx0 = fx >> 8;
      const int sx1 = sx0 < SW - 1 ? sx0 + 1 : SW - 1;
      const uint16_t wx = static_cast<uint16_t>(fx & 0xFF);

      const uint16_t a = small_[static_cast<size_t>(sy0) * SW + sx0];
      const uint16_t b = small_[static_cast<size_t>(sy0) * SW + sx1];
      const uint16_t c = small_[static_cast<size_t>(sy1) * SW + sx0];
      const uint16_t d = small_[static_cast<size_t>(sy1) * SW + sx1];

      const uint16_t top = lerp565(a, b, wx);
      const uint16_t bot = lerp565(c, d, wx);
      uint16_t glow = lerp565(top, bot, wy);
      glow = fade(glow, strength);
      row[x] = addSat(row[x], glow);
    }
  }
}

void Bloom::apply(Framebuffer &fb, uint8_t threshold, uint8_t strength) {
  if (strength == 0) return;
  brightPassDownscale(fb, threshold);
  blurPass(small_, tmp_);
  blurPass(small_, tmp_);
  upscaleAdd(fb, strength);
}

}  // namespace gfx
```

- [ ] **Step 5: Run to verify pass.** Expected PASS, 19 tests.

- [ ] **Step 6: Commit**

```bash
git add src/gfx test
git commit -m "Bloom at 1/16 area: bright-pass, downscale, two box blurs, bilinear add

Running the blur at 90x90 is what makes it affordable, and at this radius
the low resolution is imperceptible. Scratch buffers allocated once in the
constructor, never per frame."
```

---

### Task 6: Deterministic RNG, hash, and the frame clock

**Files:**
- Create: `src/core/Rng.h`, `src/core/Hash.h`, `src/core/FrameClock.h`
- Modify: `test/test_logic/main.cpp`

**Interfaces:**
- `class core::Rng` — `explicit Rng(uint32_t seed)`, `uint32_t next()`, `float unit()` (0..1), `float range(float lo, float hi)`, `void reseed(uint32_t)`
- `uint32_t fnv1a(const char* s)` — ported verbatim from the ancestor project so per-track choices stay comparable between the two.
- `class core::FrameClock` — `float tick(uint32_t now_ms)` returns clamped `dt` in seconds; `static constexpr float MAX_DT = 0.100f`.

- [ ] **Step 1: Write the failing tests**

```cpp
#include "core/FrameClock.h"
#include "core/Hash.h"
#include "core/Rng.h"

void test_rng_is_reproducible_from_a_seed(void) {
  core::Rng a(12345), b(12345);
  for (int i = 0; i < 100; ++i) TEST_ASSERT_EQUAL_UINT32(a.next(), b.next());
}

void test_rng_differs_between_seeds(void) {
  core::Rng a(1), b(2);
  bool differed = false;
  for (int i = 0; i < 20; ++i)
    if (a.next() != b.next()) differed = true;
  TEST_ASSERT_TRUE(differed);
}

void test_rng_unit_stays_in_range(void) {
  core::Rng r(7);
  for (int i = 0; i < 10000; ++i) {
    const float u = r.unit();
    TEST_ASSERT_TRUE(u >= 0.0f && u < 1.0f);
  }
}

void test_rng_range_respects_bounds(void) {
  core::Rng r(9);
  for (int i = 0; i < 5000; ++i) {
    const float v = r.range(-3.0f, 5.0f);
    TEST_ASSERT_TRUE(v >= -3.0f && v <= 5.0f);
  }
}

void test_fnv1a_is_stable_and_distinguishes(void) {
  TEST_ASSERT_EQUAL_UINT32(fnv1a("abc"), fnv1a("abc"));
  TEST_ASSERT_TRUE(fnv1a("abc") != fnv1a("abd"));
  TEST_ASSERT_EQUAL_UINT32(2166136261u, fnv1a(""));
  TEST_ASSERT_EQUAL_UINT32(2166136261u, fnv1a(nullptr));
}

void test_frame_clock_reports_elapsed_seconds(void) {
  core::FrameClock fc;
  fc.tick(1000);                      // first call establishes the baseline
  const float dt = fc.tick(1016);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.016f, dt);
}

void test_frame_clock_clamps_a_stall(void) {
  core::FrameClock fc;
  fc.tick(0);
  // A five-second stall must not teleport anything.
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, core::FrameClock::MAX_DT, fc.tick(5000));
}

void test_frame_clock_first_tick_is_zero(void) {
  core::FrameClock fc;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, fc.tick(99999));
}
```

- [ ] **Step 2: Run to verify failure.**

- [ ] **Step 3: Write the three headers**

```cpp
// src/core/Rng.h
#pragma once

// xorshift32, seeded explicitly and never from a clock.
//
// Effects that need randomness take an Rng by reference rather than calling a
// global. That is what makes a headless replay bit-exact, which is what makes
// the visual assertions stable rather than flaky.

#include <cstdint>

namespace core {

class Rng {
 public:
  explicit Rng(uint32_t seed) : s_(seed ? seed : 0x9E3779B9u) {}

  void reseed(uint32_t seed) { s_ = seed ? seed : 0x9E3779B9u; }

  uint32_t next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }

  // [0, 1). 24 bits of mantissa is more than any effect here can perceive.
  float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }

  float range(float lo, float hi) { return lo + unit() * (hi - lo); }

 private:
  uint32_t s_;
};

}  // namespace core
```

```cpp
// src/core/Hash.h
#pragma once

// FNV-1a, the project's one string hash.
//
// Everything that wants a stable per-track choice hashes the track id through
// this. Ported unchanged from the ancestor project so a given song lands on
// comparable choices in both.

#include <cstdint>

inline uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  while (s && *s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}
```

```cpp
// src/core/FrameClock.h
#pragma once

// Produces the dt every effect integrates against.
//
// Clamped, because a stall - a GC pause on the host, a TLS handshake on the
// device - must not teleport a particle field across the screen. Effects are
// correct at any frame rate; they are not correct across an unbounded jump.

#include <cstdint>

namespace core {

class FrameClock {
 public:
  static constexpr float MAX_DT = 0.100f;

  // Returns seconds since the previous call. The first call returns 0.
  float tick(uint32_t now_ms) {
    if (!started_) {
      started_ = true;
      last_ms_ = now_ms;
      return 0.0f;
    }
    // Unsigned subtraction is correct across the uint32 wrap.
    const uint32_t elapsed = now_ms - last_ms_;
    last_ms_ = now_ms;
    const float dt = static_cast<float>(elapsed) * 0.001f;
    return dt > MAX_DT ? MAX_DT : dt;
  }

 private:
  bool started_ = false;
  uint32_t last_ms_ = 0;
};

}  // namespace core
```

- [ ] **Step 4: Run to verify pass.** Expected PASS, 27 tests.

- [ ] **Step 5: Commit**

```bash
git add src/core test
git commit -m "Deterministic RNG, FNV-1a, and a clamped frame clock

Effects take an Rng by reference rather than calling a global, and dt is
clamped: that pair is what makes headless replay bit-exact, which is what
makes the visual assertions stable rather than flaky."
```

---

## Remaining tasks

Tasks 1-6 establish the primitives and are specified step-by-step above. From
here the tasks are specified as **files, interfaces, the non-obvious code, and
the required tests**. The TDD cycle is unchanged and is not restated: write the
failing test, run it, implement minimally, run it, commit.

---

### Task 7: Particle system

**Files:** create `src/fx/ParticleSystem.h`, `src/fx/ParticleSystem.cpp`; modify `test/test_logic/main.cpp`.

**Interfaces produced:**

```cpp
namespace fx {

struct SpawnParams {
  float x, y;          // origin, pixels
  float speed_min, speed_max;
  float life_min, life_max;   // seconds
  float size_min, size_max;   // pixels, 1..6
  uint16_t colors[16];        // palette to draw from
  int color_count;
  float drag;                 // per-second velocity multiplier, 0.85..1.0
  float gravity_y;            // pixels/s^2
};

class ParticleSystem {
 public:
  static constexpr int MAX = 3000;

  void configure(const SpawnParams &p);
  // Emit n particles now. Silently emits fewer if the pool is full: dropping a
  // particle is always preferable to growing a buffer mid-frame.
  void emit(int n, core::Rng &rng);
  // Radial burst from the configured origin, used on beat onsets.
  void burst(int n, float speed_scale, core::Rng &rng);
  void update(float dt);
  // Additive, so overlapping particles read as light rather than as occlusion.
  void render(gfx::Framebuffer &fb) const;

  int liveCount() const { return live_; }
  void clear() { live_ = 0; }

 private:
  // Structure of arrays: update() touches position and velocity only, and
  // keeping them contiguous is the difference between streaming cleanly and
  // thrashing the cache on every particle.
  float x_[MAX], y_[MAX], vx_[MAX], vy_[MAX];
  float life_[MAX], life0_[MAX], size_[MAX];
  uint16_t col_[MAX];
  int live_ = 0;
  SpawnParams p_ = {};
};

}  // namespace fx
```

**Implementation notes:**
- `update` compacts the array by swapping the last live particle into a dead
  slot, so there is no per-particle allocation and no gaps to skip.
- Brightness must fall off with remaining life (`life_/life0_`) via
  `gfx::fade`, or particles vanish abruptly and read as a glitch.
- `render` clips per-particle against the framebuffer bounds **before** the
  inner loop, and uses `gfx::addSat`. Particles up to 6 px draw as a filled
  square; the bloom pass supplies the roundness, which is far cheaper than
  drawing a circle per particle.

**Required tests:**
- emitting `MAX + 500` yields exactly `MAX` live particles and does not crash
- a particle with `life 0.1s` is gone after `update(0.2f)`
- two systems with identically-seeded `Rng`s produce identical live counts and
  identical framebuffers after the same call sequence
- emitting at `x = -50, y = -50` then rendering leaves the framebuffer
  untouched and does not write out of bounds (run under `-fsanitize=address`)
- `render` on an empty system leaves the framebuffer untouched
- `burst` distributes particles in more than one direction (check that both
  positive and negative `vx` appear)

---

### Task 8: The modulation bus and its procedural fallback

**Files:** create `src/audio/Modulation.h`, `src/audio/Procedural.h`, `src/audio/Procedural.cpp`; modify `test/test_logic/main.cpp`.

**Interfaces produced:**

```cpp
namespace audio {

// The one struct every effect reads. Effects see this and nothing else: no
// microphone, no AppState, no clock. That indirection is the whole reason the
// engine is testable and the reason a mic that hears nothing degrades instead
// of freezing.
struct Modulation {
  float bass = 0.0f;      // 0..1, smoothed
  float mid = 0.0f;
  float treble = 0.0f;
  float loudness = 0.0f;
  bool onset = false;     // a beat was detected on this frame
  float beat_phase = 0.0f;  // 0..1 sawtooth, phase-locked to tracked tempo
  float progress01 = 0.0f;
  float volume01 = 0.0f;
  uint32_t track_seed = 0;
  bool live = false;      // the mic is genuinely hearing music
};

// Generates plausible band energies from track progress and seed alone.
// Not a degraded mode to be tolerated: the ancestor project's ambient scenes
// ran entirely on inputs like these and looked good, and this is the path a
// headphone listener sees every time.
class Procedural {
 public:
  void reseed(uint32_t track_seed);
  // Advances internal oscillators and fills the audio-derived fields.
  void fill(Modulation *m, float dt);
 private:
  float clock_ = 0.0f;
  float bpm_ = 112.0f;
  float phase_ = 0.0f;
  uint32_t seed_ = 0;
};

}  // namespace audio
```

**Implementation notes:**
- `Procedural::reseed` derives a plausible BPM in `[84, 148]` from the seed, so
  a given track always pulses at the same rate.
- Bands are sums of two detuned sines per band at different rates, offset by
  the seed, so the three bands move independently rather than in lockstep.
- `onset` fires when `phase_` wraps, giving a steady beat.

**Required tests:**
- every field stays within `0..1` across 10,000 frames at `dt = 1/60`
- the same `track_seed` produces an identical sequence; a different one differs
- `onset` fires at a rate consistent with the derived BPM (±15% over 30 s)
- `beat_phase` is monotonic between wraps and never exceeds 1

---

### Task 9: FFT

**Files:** create `src/audio/Fft.h`, `src/audio/Fft.cpp`; modify `test/test_logic/main.cpp`.

**Interfaces produced:**

```cpp
namespace audio {

// 512-point radix-2 complex FFT, used on real input with the imaginary part
// zeroed. A real-input FFT packing two reals per complex bin would halve the
// work, but this is ~2ms on the target and correctness here is worth more than
// a millisecond: an FFT that is subtly wrong produces a visualiser that is
// subtly unconvincing, which is impossible to debug by eye.
class Fft {
 public:
  static constexpr int N = 512;
  static constexpr int BINS = N / 2 + 1;

  Fft();  // precomputes twiddles and the bit-reversal table

  // Applies a Hann window to `in` (N samples, -1..1) and writes BINS
  // magnitudes to `out_mag`.
  void magnitudes(const float *in, float *out_mag);

 private:
  float cos_[N / 2], sin_[N / 2], win_[N];
  uint16_t rev_[N];
  float re_[N], im_[N];
};

}  // namespace audio
```

**Implementation** — standard iterative Cooley-Tukey. Write it exactly as
follows; this loop structure is easy to get subtly wrong:

```cpp
#include "Fft.h"
#include <cmath>

namespace audio {

Fft::Fft() {
  for (int i = 0; i < N / 2; ++i) {
    const double a = -2.0 * M_PI * i / N;
    cos_[i] = static_cast<float>(std::cos(a));
    sin_[i] = static_cast<float>(std::sin(a));
  }
  for (int i = 0; i < N; ++i) {
    win_[i] = static_cast<float>(
        0.5 * (1.0 - std::cos(2.0 * M_PI * i / (N - 1))));
  }
  int bits = 0;
  while ((1 << bits) < N) ++bits;
  for (int i = 0; i < N; ++i) {
    unsigned r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b)) r |= 1u << (bits - 1 - b);
    rev_[i] = static_cast<uint16_t>(r);
  }
}

void Fft::magnitudes(const float *in, float *out_mag) {
  for (int i = 0; i < N; ++i) {
    re_[rev_[i]] = in[i] * win_[i];
    im_[rev_[i]] = 0.0f;
  }
  for (int len = 2; len <= N; len <<= 1) {
    const int half = len >> 1;
    const int step = N / len;
    for (int i = 0; i < N; i += len) {
      for (int j = 0; j < half; ++j) {
        const int t = j * step;
        const float wr = cos_[t], wi = sin_[t];
        const int a = i + j, b = a + half;
        const float xr = re_[b] * wr - im_[b] * wi;
        const float xi = re_[b] * wi + im_[b] * wr;
        re_[b] = re_[a] - xr;
        im_[b] = im_[a] - xi;
        re_[a] += xr;
        im_[a] += xi;
      }
    }
  }
  for (int i = 0; i < BINS; ++i)
    out_mag[i] = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]);
}

}  // namespace audio
```

**Required tests:**
- a unit-amplitude sine at exactly bin 32 peaks at `out[32]`, and that bin is at
  least 20x the mean of all other bins
- a sine at bin 100 peaks at `out[100]` — proves the twiddle indexing is right
  across the range, not just at one point
- silence in gives all-zero magnitudes
- DC input (all `1.0f`) puts its energy in bin 0, not spread across the spectrum
- two identical calls produce identical output (no state leaking between calls)

---

### Task 10: Band energy, onset detection, tempo tracking

**Files:** create `src/audio/BandEnergy.h/.cpp`, `src/audio/OnsetDetector.h/.cpp`, `src/audio/TempoTracker.h/.cpp`; modify `test/test_logic/main.cpp`; create `tools/make_test_wav.py`.

**Interfaces produced:**

```cpp
namespace audio {

// Sample rate is fixed at 16 kHz: the FFT's 512 bins then span 0-8 kHz at
// 31.25 Hz resolution, which is plenty for three bands and cheap to capture.
constexpr int SAMPLE_RATE = 16000;

class BandEnergy {
 public:
  // Log-spaced: bass 40-250 Hz, mid 250-2000 Hz, treble 2000-8000 Hz.
  // Writes smoothed 0..1 values. `attack` and `release` are per-second
  // smoothing rates - asymmetric, because a visualiser that rises instantly
  // and falls slowly reads as punchy, and the reverse reads as broken.
  void process(const float *mag, int bins, float dt,
               float *bass, float *mid, float *treble, float *loudness);
  void reset();
 private:
  float b_ = 0, m_ = 0, t_ = 0, l_ = 0;
};

class OnsetDetector {
 public:
  // Spectral flux against an adaptive threshold: the median of a rolling
  // window, times a sensitivity factor. A fixed threshold works on one track
  // and fails on the next, which is worse than none.
  bool process(const float *mag, int bins);
  void reset();
 private:
  static constexpr int HIST = 43;   // ~1s of hops at 16kHz/hop 256+
  float prev_[Fft::BINS] = {};
  float flux_[HIST] = {};
  int head_ = 0;
  bool primed_ = false;
  int since_onset_ = 0;
};

class TempoTracker {
 public:
  // Feed every frame; `onset` on the frames a beat was detected.
  void process(bool onset, float dt);
  float bpm() const { return bpm_; }
  // 0..1 sawtooth, wrapping on each predicted beat. Effects that want to move
  // *with* the music rather than *react to* it use this, because reaction is
  // always one frame late and prediction is not.
  float beatPhase() const { return phase_; }
 private:
  float intervals_[8] = {};
  int n_ = 0;
  float since_ = 0.0f;
  float bpm_ = 120.0f;
  float phase_ = 0.0f;
};

}  // namespace audio
```

**`tools/make_test_wav.py`** generates the fixtures the tests need — all
16-bit mono 16 kHz:
- `silence.wav` — 3 s of zeros
- `sine440.wav` — 3 s of a 440 Hz sine at 0.5 amplitude
- `clicks120.wav` — 8 s of 5 ms noise bursts at exactly 120 BPM
- `sweep.wav` — 4 s log sweep, 40 Hz to 8 kHz

**Required tests:**
- `BandEnergy`: a 100 Hz tone raises `bass` above 0.5 and leaves `treble` below
  0.1; a 5 kHz tone does the reverse. This catches a band-edge-to-bin-index
  arithmetic error, which is otherwise invisible.
- `BandEnergy`: silence decays all four outputs to below 0.02 within 1 s
- `BandEnergy`: attack is faster than release (step up reaches 0.5 in fewer
  frames than the step down back to 0.5)
- `OnsetDetector`: fed `clicks120.wav` frames, reports 15-17 onsets in 8 s
  (16 expected at 120 BPM)
- `OnsetDetector`: fed `sine440.wav` (steady tone, no transients) reports fewer
  than 3 onsets — a detector that fires on sustained sound is useless
- `OnsetDetector`: fed silence, reports zero onsets
- `TempoTracker`: after 8 s of 120 BPM onsets, `bpm()` is within 6 of 120
- `TempoTracker`: `beatPhase()` stays in `0..1` and wraps at the expected rate

---

### Task 11: Mic source and the audio analyzer

**Files:** create `src/audio/MicSource.h`, `src/audio/AudioAnalyzer.h/.cpp`, `src/platform/desktop/WavMic.h/.cpp`; modify `test/test_logic/main.cpp`.

**Interfaces produced:**

```cpp
namespace audio {

// The one seam between the analyzer and where audio comes from. Desktop reads
// a WAV; the device will read I2S. Neither the analyzer nor any effect knows
// which.
class MicSource {
 public:
  virtual ~MicSource() = default;
  // Fills up to `n` samples in -1..1, returns how many were written. Returning
  // fewer than requested is normal and means "no more audio right now"; it must
  // never block.
  virtual int read(float *out, int n) = 0;
  virtual void restart() {}
};

class AudioAnalyzer {
 public:
  static constexpr int HOP = 256;         // 16ms at 16kHz
  static constexpr float SILENCE_RMS = 0.004f;
  static constexpr float CROSSFADE_S = 0.5f;

  void begin(MicSource *mic);
  void setTrack(uint32_t track_seed);
  // Drains the mic, runs the chain if a full hop arrived, and fills the
  // audio-derived fields of `m`. Crossfades to Procedural when the mic has been
  // below SILENCE_RMS for longer than a short hold.
  void update(Modulation *m, float dt);

 private:
  MicSource *mic_ = nullptr;
  Fft fft_;
  BandEnergy bands_;
  OnsetDetector onset_;
  TempoTracker tempo_;
  Procedural fallback_;
  float ring_[Fft::N] = {};
  int filled_ = 0;
  float mag_[Fft::BINS] = {};
  float live_mix_ = 0.0f;   // 0 = fully procedural, 1 = fully live
  float quiet_s_ = 0.0f;
};

}  // namespace audio
```

**Implementation notes:**
- The ring is a simple shift-by-HOP buffer: on each full hop, move the tail
  down and append. At 512 samples that memmove is trivial and far easier to
  reason about than a circular index, which is where off-by-one FFT bugs live.
- Crossfade both the band values and `onset`: while `live_mix_ < 0.5`, `onset`
  comes from `Procedural`, above it from `OnsetDetector`. Blending a boolean is
  meaningless; switching it at the midpoint is honest.
- `m->live` is `live_mix_ > 0.5f`.

**`WavMic`** parses a 16-bit PCM mono WAV (validate `RIFF`/`WAVE`, find the
`fmt ` and `data` chunks by walking the chunk list rather than assuming
offsets — files from different tools have different chunk layouts), and loops
by default so a short fixture drives a long run.

**Required tests:**
- `WavMic` on `sine440.wav` returns non-zero samples within `-1..1`
- `WavMic` on a nonexistent path fails cleanly and `read` returns 0 rather than
  crashing
- `WavMic` loops: reading past the end wraps to the start
- `AudioAnalyzer` fed `silence.wav` reports `live == false` after 1 s, and its
  band values are still non-constant (the procedural fallback is running)
- `AudioAnalyzer` fed `clicks120.wav` reports `live == true` within 1 s
- `AudioAnalyzer` with a null mic does not crash and produces procedural values
- the transition from loud to silent takes at least `CROSSFADE_S` to complete
  (no visible snap)

---

### Task 12: Port the playback state model and the fixture source

**Files:** copy from `../m5stackfirmware/src/` into `src/core/` and
`src/sources/`: `PlaybackState.h`, `AppState.h`, `CommandQueue.h`,
`ProgressClock.h`, `MergePolicy.h`, `Deadline.h`, `FakeSource.h`,
`FakeSource.cpp`. Modify `test/test_logic/main.cpp`.

**Notes:**
- These are already display-free and hardware-free. The only edits needed are
  include-path fixes and removing any `M5Unified.h` include.
- `FakeSource.cpp` references fixture art paths; point them at `assets/art/`
  and copy the five fixture JPEGs from the ancestor project's `assets/art/`.
- Port the ancestor's existing unit tests for `Deadline`, `MergePolicy`,
  `ProgressClock` and `CommandQueue` verbatim from
  `../m5stackfirmware/test/test_logic/main.cpp`. They already encode real bugs;
  rewriting them would only lose that.

**Required additional tests:**
- `CommandQueue::pushCoalesced` replaces a pending `SetVolume` rather than
  appending, so a fast knob spin sends one command and not forty
- a full queue drops rather than blocks or grows
- `FakeSource` publishes only on its poll interval, and a command's effect
  appears only after `FAKE_LATENCY_MS`

---

### Task 13: Image loading and palette extraction

**Files:** create `src/art/Image.h`, `src/art/Palette.h/.cpp`, `src/platform/desktop/JpegLoad.cpp`; vendor `src/platform/desktop/stb_image.h`; modify `test/test_logic/main.cpp`.

**Interfaces produced:**

```cpp
namespace art {

// A decoded cover, native-endian RGB565, owning its pixels.
struct Image {
  int w = 0, h = 0;
  std::vector<uint16_t> px;
  bool valid() const { return w > 0 && h > 0 && px.size() == size_t(w) * h; }
  uint16_t at(int x, int y) const;
  // Bilinear sample at normalised coordinates, clamped at the edges. This is
  // the function Quad3D calls once per rasterised pixel, so it is inline and
  // does no bounds arithmetic beyond the clamp.
  uint16_t sample(float u, float v) const;
};

// Platform-provided. Desktop decodes with stb_image; the device will use
// TJpgDec streaming off SD. Same signature, so nothing above this cares.
bool loadJpeg(const char *path, int max_dim, Image *out);

struct Palette {
  static constexpr int N = 16;
  uint16_t colors[N] = {};
  int count = 0;
  uint16_t dominant = 0;   // most-covered non-grey colour
  uint16_t tint = 0;       // dominant, brightness-normalised for UI accents
};

// Median-cut over a downsampled copy. Deterministic: the same image always
// yields the same palette in the same order, because a palette that reshuffles
// between frames makes every particle change colour at random.
Palette extractPalette(const Image &img);

}  // namespace art
```

**Implementation notes:**
- `extractPalette` samples at most 64x64 pixels regardless of input size, so
  cost is independent of cover resolution.
- Near-black and near-white pixels are excluded from `dominant`, or almost every
  cover reports "black" and the whole view loses its colour identity.
- `tint` is `dominant` scaled so its luma lands in `110..190`: covers range from
  nearly black to nearly white, and an accent colour taken raw is unreadable at
  both ends. The ancestor project's `ArtCache` tint sampling has the same
  purpose and is worth reading first.

**Required tests:**
- a synthetic all-red `Image` yields `dominant` within one RGB565 step of pure
  red
- a synthetic image that is 75% blue and 25% yellow reports blue as `dominant`
- an all-black image still returns a usable `tint` (non-zero) rather than black
- `extractPalette` called twice on the same image returns byte-identical results
- `sample(0,0)` and `sample(1,1)` return the corner pixels
- `sample` at out-of-range coordinates clamps rather than reading out of bounds
- `loadJpeg` on a fixture returns a valid image with the expected aspect ratio
- `loadJpeg` on a nonexistent or truncated file returns false and leaves `out`
  invalid rather than half-populated

---

### Task 14: Quad3D — the perspective-textured quad

**Files:** create `src/gfx/Quad3D.h`, `src/gfx/Quad3D.cpp`; modify `test/test_logic/main.cpp`.

This is the "3D" in the view. A textured quad with a real perspective divide is
enough: the cover is a flat object, and a full triangle pipeline would buy
nothing a quad cannot express.

**Interfaces produced:**

```cpp
namespace gfx {

struct Vec3 { float x, y, z; };

// Draws a texture-mapped quad with perspective-correct interpolation.
//
// Corners are given in view space (camera at the origin looking down +z) and
// projected with a fixed focal length. UVs are the unit square. The quad is
// rasterised as two triangles, interpolating u/w, v/w and 1/w linearly in
// screen space - which is what perspective-correct means, and what separates
// this from the affine warp that makes texture edges visibly bend.
class Quad3D {
 public:
  static constexpr float FOCAL = 320.0f;
  static constexpr float NEAR_Z = 0.05f;

  // corners: TL, TR, BR, BL in view space. `alpha` 0..256 blends against what
  // is already there; 256 is opaque. `tint_mul` fades the sampled texel, for
  // the dimmed reflection.
  void draw(Framebuffer &fb, const art::Image &tex, const Vec3 corners[4],
            uint16_t alpha, uint8_t tint_mul);

 private:
  void triangle(Framebuffer &fb, const art::Image &tex,
                const float sx[3], const float sy[3], const float inv_w[3],
                const float u_w[3], const float v_w[3],
                uint16_t alpha, uint8_t tint_mul);
};

}  // namespace gfx
```

**Implementation notes:**
- Project as `sx = CX + x * FOCAL / z`, `sy = CY + y * FOCAL / z`, and reject the
  whole quad if any `z < NEAR_Z` — clipping a quad properly against the near
  plane is real work and this view never needs it, because the cover never
  passes through the camera. Reject explicitly rather than letting it divide by
  something near zero and spray geometry across the screen.
- Rasterise with the standard edge-function/barycentric method over the
  triangle's integer bounding box, intersected with the framebuffer bounds.
- At each covered pixel: interpolate `inv_w`, `u_w`, `v_w`; then
  `u = u_w / inv_w`, `v = v_w / inv_w`. That division is the perspective
  correction and it is per-pixel; there is no cheaper honest version.
- Backface culling by the sign of the edge-function area, so the reflection quad
  does not need separate winding handling.

**Required tests:**
- a quad at constant `z` facing the camera fills a rectangle whose corners are
  within 1 px of the analytically projected positions
- the same quad drawn with a 2x2 checkerboard texture puts the correct texel
  colour at each of the four quadrant centres — this is the test that catches
  transposed or flipped UVs, which look plausible and are wrong
- a quad rotated 45 degrees about the y axis produces a trapezoid: its far edge
  is measurably shorter than its near edge (this is what proves the perspective
  divide is happening at all, rather than an affine warp)
- a quad entirely behind the camera (`z < 0`) draws nothing
- a quad with any corner at `z < NEAR_Z` draws nothing
- a quad larger than the screen is clipped and writes nothing out of bounds
  (run under `-fsanitize=address`)
- `alpha = 0` leaves the framebuffer untouched; `alpha = 256` fully replaces

---

### Task 15: The "Cover Light" view

**Files:** create `src/views/View.h`, `src/views/CoverLight.h/.cpp`; modify `src/main.cpp`, `tools/visual_tests.py`.

**Interfaces produced:**

```cpp
namespace views {

struct ViewCtx {
  const art::Image *cover;    // may be null: no artwork yet
  const art::Palette *pal;
  const AppState *state;
  const audio::Modulation *mod;
};

class View {
 public:
  virtual ~View() = default;
  virtual const char *name() const = 0;
  // Called when this view becomes active or the track changes.
  virtual void enter(const ViewCtx &ctx, core::Rng &rng) = 0;
  // Composites one full frame. Full-frame every time: this board has the PSRAM
  // for it and the dirty-rect split its ancestor needed bought complexity that
  // is no longer paid for.
  virtual void render(gfx::Framebuffer &fb, const ViewCtx &ctx, float dt,
                      core::Rng &rng) = 0;
};

class CoverLight : public View { /* ... */ };

}  // namespace views
```

**Composition order inside `CoverLight::render`:**

1. Fade the persistence buffer by `0xE6` (≈90%), scaled toward `0xF2` as
   `mod->loudness` rises, so louder passages leave longer trails.
2. Backdrop: radial gradient from `pal->tint` at the centre to near-black at
   the rim, radius modulated by `mod->mid`.
3. Cover quad: orbit angle advances at `0.18 rad/s`, tilt oscillates ±12°.
   Scale pulses with `mod->bass`. Drawn opaque.
4. Reflection: the same quad mirrored in y below the original, `alpha = 110`,
   `tint_mul = 150`, with a per-scanline horizontal offset of
   `sin(y * 0.09 + clock) * (2 + 6 * mod->bass)`.
5. Particles: `update` then `render`. `emit(3, rng)` per frame as a steady
   drizzle; on `mod->onset`, `burst(90, 1.0f + mod->bass, rng)`.
6. Bloom: `threshold 48`, `strength = 90 + 120 * mod->loudness`.
7. Dither, then circular mask.

**Behaviour requirements:**
- With `ctx.cover == nullptr`, render a flat `pal->tint` disc with the particle
  field still running. A missing cover must look deliberate — the ancestor
  project's `art_loading` flag exists because "no artwork" text during a normal
  two-second download made a working device look broken.
- `enter` reseeds `rng` from `mod->track_seed` and calls
  `ParticleSystem::clear()`, so a track change is a clean start rather than the
  previous track's particles drifting through the new one.

**Required visual tests** (added to `tools/visual_tests.py`, each driven by env
hooks `KNOB_FAKE=1`, `KNOB_TRACK=<n>`, `KNOB_WAV=<path>`, `KNOB_MOD_BASS=<f>`
etc., which `main.cpp` must honour):
- the centre region is lit and its mean hue is within a tolerance of the
  fixture cover's dominant colour — proves the palette reached the view
- `nonblack_fraction` over the disc exceeds 0.35 with particles running
- with `KNOB_PARTICLES=0`, that fraction drops measurably — proves the particle
  layer is actually contributing and not being drawn over
- the four corners are black in every case
- with `KNOB_NOCOVER=1`, the frame is still lit (the missing-art path renders)
- with `KNOB_WAV=assets/audio/clicks120.wav`, at least one frame in a 4 s run
  is measurably brighter than the dimmest — proves onsets reach the bloom
- two headless runs with identical env produce byte-identical BMPs — the
  determinism guarantee the whole test layer rests on

---

### Task 16: Interactive harness

**Files:** modify `src/main.cpp`; create `tools/harness.sh`.

A keyboard-driven harness so every state is reachable on demand instead of by
waiting for a track to end. Modelled on the ancestor project's, which its
README credits for most of its visual fixes.

| Key | |
|---|---|
| `space` | play / pause |
| `[` `]` | previous / next fixture track |
| `,` `.` | scrub ∓10 s |
| `v` `b` | volume ∓5 |
| `f` | toggle liked |
| `m` | toggle mic between the WAV fixture and silence |
| `p` | toggle the particle layer |
| `o` | toggle bloom |
| `d` | dump the current frame to `/tmp/knob.bmp` |
| `r` | re-enter the view (reseed) |
| `q` | quit |

`tools/harness.sh` builds and runs with the fixture source and a looping WAV.

---

## Hardware milestones

Reordered deliberately: the spec listed bring-up after the desktop work, but
unverified pins plus an unproven QSPI init sequence is the largest unknown in
the project, and it is cheaper to discover a wrong pin now than after three
views are tuned against assumptions. **Task 17 should run as soon as the board
is available, in parallel with Tasks 3-16.**

---

### Task 17: ESP32 environment, boot banner, and pin confirmation

**Files:** modify `platformio.ini`; create `src/platform/esp32/Boot.h/.cpp`, `src/platform/esp32/Pins.h`, `src/main_esp32.cpp`.

**`platformio.ini` addition:**

```ini
; ---------------------------------------------------------------------------
; Waveshare ESP32-S3-Knob-Touch-LCD-1.8. ESP32-S3R8: 8MB octal PSRAM, 16MB
; flash. USB-JTAG serial, so the port is a usbmodem and not a usbserial.
; ---------------------------------------------------------------------------
[env:esp32]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

; The Arduino ESP32 core appends -std=gnu++11, which beats anything set in
; [env]. Unset theirs, then set ours, or C++14 features fail to compile.
build_unflags = -std=gnu++11
build_flags =
    ${env.build_flags}
    -std=gnu++14
    -DDEVICE=1
    -I src
    -DBOARD_HAS_PSRAM
    -DARDUINO_USB_MODE=1
    -DARDUINO_USB_CDC_ON_BOOT=1

; Octal PSRAM (the R8 part), not quad. Getting this wrong gives a board that
; boots and reports zero PSRAM, which reads as a hardware fault.
board_build.arduino.memory_type = qio_opi
board_build.psram_type = opi
board_build.flash_mode = qio
board_build.f_flash = 80000000L
board_build.partitions = huge_app.csv
board_upload.flash_size = 16MB
board_build.flash_size = 16MB

monitor_speed = 115200
monitor_filters = esp32_exception_decoder
build_src_filter =
    +<*>
    -<platform/desktop/>
    -<main.cpp>
```

**`src/platform/esp32/Pins.h`** — every pin in one place, each marked with how
it was established. Waveshare's product page and wiki both return HTTP 403, so
these start as **unconfirmed**, sourced from community ESPHome and Tasmota
configurations:

```cpp
#pragma once

// Pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// UNCONFIRMED as of writing: Waveshare's product page and wiki both answer 403,
// so these came from community ESPHome and Tasmota configurations for the same
// panel. Task 17 confirms each one on hardware and this comment gets updated to
// say so. Do not build anything on top of an unconfirmed pin.

namespace pins {

// Display: ST77916, QSPI.
constexpr int LCD_SCK = 13;
constexpr int LCD_CS = 14;
constexpr int LCD_D0 = 15;
constexpr int LCD_D1 = 16;
constexpr int LCD_D2 = 17;
constexpr int LCD_D3 = 18;
constexpr int LCD_RST = 21;
constexpr int LCD_BL = 47;

// Touch: CST816, I2C.
constexpr int TP_SDA = 11;
constexpr int TP_SCL = 12;
constexpr int TP_INT = 9;
constexpr int TP_RST = 10;

// Rotary encoder, quadrature.
constexpr int ENC_A = 8;
constexpr int ENC_B = 7;

}  // namespace pins
```

**`Boot.cpp` — the boot banner.** The ancestor project's board notes say this
"answered more questions than any other single thing." It must print:

- reset reason (`esp_reset_reason()`) — distinguishes a real crash from the
  reset that attaching to the serial port causes
- consecutive abnormal-reset count, persisted in NVS
- chip model, revision, core count, CPU frequency
- flash size and mode
- **PSRAM total and largest free block** — the number the whole render design
  depends on, and the one that silently reads zero when `memory_type` is wrong
- internal heap total and largest free block

**Steps:**
- [ ] Add the `esp32` environment; build a sketch that prints only the banner
- [ ] Flash: `HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 -t upload --upload-port /dev/cu.usbmodem201301`
- [ ] Confirm PSRAM reports ~8 MB. **If it reports 0, stop and fix
      `memory_type` before anything else** — every later task assumes it.
- [ ] Confirm each pin in `Pins.h` by toggling it and observing, or by reading
      the board's schematic if it can be obtained. Update the header comment.
- [ ] Commit

---

### Task 18: ST77916 QSPI panel and the bandwidth benchmark

**Files:** vendor `src/platform/esp32/st77916/` from the ESP Component Registry (`esp_lcd_st77916`); create `src/platform/esp32/Panel.h/.cpp`, `src/platform/esp32/Bench.cpp`.

**Interfaces produced:**
- `bool esp32::panelBegin()` — brings up the QSPI bus and the panel.
- `void esp32::panelPush(const gfx::Framebuffer&)` — byte-swaps and pushes one
  full frame; blocks until the previous DMA has completed, not until this one
  has.
- `void esp32::panelBacklight(uint8_t duty)`

**Known traps, from community reports — build these in from the start:**
- QSPI mode wraps every command in a 32-bit opcode frame: `0x02` for a command
  write, `0x32` to enter QSPI pixel data. The vendor driver handles this; a
  hand-rolled SPI driver that ignores it produces a black screen with no error.
- A **~120 ms delay is required after sleep-exit and before display-enable.**
  Omitting it gives an intermittently blank panel that looks like bad wiring.
- **40 MHz works where 50 MHz does not** on ESP32 for this panel. Start at 40.

**The bandwidth benchmark** is the deliverable that unblocks Task 15's final
tuning. It must report, averaged over 200 iterations:
- PSRAM sequential write MB/s (`fill` over a 259 KB buffer)
- PSRAM sequential read MB/s
- PSRAM read-modify-write MB/s (the `fade` pass — the real hot loop)
- internal-SRAM equivalents for all three, as the ratio that decides whether
  banded rendering is needed
- measured full-frame QSPI push time, and whether it overlaps computation

**Decision gate:** if PSRAM read-modify-write comes in below ~35 MB/s, raise a
follow-up task to move the fade, bloom-upscale and particle passes into
internal-SRAM bands with ping-pong DMA, as spec section 5 anticipates. Record
the measured numbers in the spec's performance table, replacing the estimates.

**Steps:**
- [ ] Vendor the driver; bring up the panel showing a solid colour
- [ ] Show a test pattern that makes orientation, colour order and the visible
      disc boundary all obvious at a glance: coloured quadrants, a centred
      cross, and a one-pixel ring at r=179
- [ ] Confirm red renders red. If channels are swapped, fix it in `panelPush`'s
      swap step and nowhere else — the framebuffer stays native-endian
- [ ] Write and run the benchmark; record results in the spec
- [ ] Commit, with the measured numbers in the commit message

---

### Task 19: Touch, encoder, and haptics

**Files:** create `src/input/Encoder.h/.cpp`, `src/input/Touch.h/.cpp`, `src/input/GestureRecognizer.h/.cpp`, `src/input/Haptics.h/.cpp`; port `src/input/ButtonLogic.h` from the ancestor project.

**Interfaces produced:**

```cpp
namespace input {

// PCNT-based quadrature decode. Hardware counting rather than interrupt-driven
// software decode: a fast spin on a software decoder drops steps, and a knob
// that loses steps when spun quickly feels broken in a way no amount of UI
// polish hides.
class Encoder {
 public:
  bool begin(int pin_a, int pin_b);
  // Net detents since the last call. Positive is clockwise.
  int delta();
};

enum class Gesture : uint8_t { None, Tap, SwipeLeft, SwipeRight, SwipeUp,
                               SwipeDown, LongPress };

class GestureRecognizer {
 public:
  static constexpr int SWIPE_MIN_PX = 40;
  static constexpr uint32_t TAP_MAX_MS = 300;
  static constexpr uint32_t LONG_MS = 600;
  // Feed the raw touch state every frame.
  Gesture update(bool touching, int x, int y, uint32_t now_ms);
};

class Haptics {
 public:
  bool begin();
  void click();       // one detent
  void bump();        // a heavier confirmation, for like/skip
};

}  // namespace input
```

**Required tests** — `GestureRecognizer` is a pure state machine over
`(touching, x, y, now_ms)`, so it is fully host-testable with synthetic traces
and no hardware:
- a press and release within `TAP_MAX_MS` at the same point yields `Tap`
- a press held past `LONG_MS` yields `LongPress` once, and the subsequent
  release yields `None` — a long press must not also emit a tap, or every like
  would additionally toggle playback. This is the exact bug the ancestor
  project's `ButtonLogic` test suite was written against.
- a horizontal drag of `SWIPE_MIN_PX + 10` yields `SwipeLeft` or `SwipeRight`
  by sign
- a drag shorter than `SWIPE_MIN_PX` yields `Tap`, not a swipe
- a diagonal drag resolves to the dominant axis
- a touch that never releases emits `LongPress` and then nothing

**Steps:**
- [ ] Host-test the gesture recogniser to green before touching hardware
- [ ] Bring up CST816 over I2C; log raw coordinates; confirm the axes match the
      display's orientation as established in Task 18
- [ ] Bring up the encoder on PCNT; log detents; confirm one physical click is
      one detent and that direction matches expectation
- [ ] Bring up DRV2605; fire a click per detent and confirm it feels locked to
      the physical detent rather than lagging it
- [ ] Commit

---

### Task 20: I2S microphone

**Files:** create `src/platform/esp32/I2sMic.h/.cpp`.

`I2sMic` implements `audio::MicSource` (Task 11), so the entire analysis chain
and every effect above it are already written and tested by the time this
exists. That is the payoff of the `MicSource` seam.

**Steps:**
- [ ] Bring up I2S at 16 kHz mono; log RMS and confirm it tracks room loudness
- [ ] Establish the real noise floor with the room quiet, and set
      `AudioAnalyzer::SILENCE_RMS` from the measurement rather than the guess
- [ ] Play music on the desk speakers; confirm `live == true`, and that onset
      detection fires on kicks and not on steady tones
- [ ] Confirm the analysis chain fits its budget on core 0 without starving the
      network task
- [ ] Commit

---

### Task 21: The player — network, Spotify, config

**Files:** copy from `../m5stackfirmware/src/`: `net/` (`NetWorker`, `WifiLink`, `HttpClient`, `NetLog`), `spotify/` (`SpotifyAuth`, `SpotifySource`), `config/` (`DeviceConfig`), `art/ArtCache`, and `platform/esp32/` (`Esp32HttpClient`, `Esp32WifiLink`, `Esp32Storage`, `Esp32Portal`).

**Notes:**
- These port with include-path edits and little else. They are Arduino-based
  (`WiFiClientSecure`, `HTTPClient`, `Preferences`, `SD`), which is exactly why
  the Arduino framework was chosen in the spec.
- **Do not port** `Esp32Power.cpp`'s IP5306 boost-latch sequence or the
  shared-SPI-bus avoidance in the display path. Both are Core-Basic-specific and
  neither applies to this board.
- The one-TLS-session-at-a-time constraint was an ESP32 heap limit with no PSRAM.
  With 8 MB of PSRAM, re-measure before inheriting it — but keep the
  status-code guard before every body read, which was a protocol bug and not a
  memory one: a 204 or 304 carries no `Content-Length`, `getSize()` returns -1,
  and reading a body on -1 with keep-alive on hangs the task forever.
- Spotify credentials transfer unchanged. `tools/get_refresh_token.py` from the
  ancestor project still applies, and the setup portal stores into NVS the same
  way.

**Steps:**
- [ ] Port `core/` merge and settle logic; run the ported unit tests to green
- [ ] Port `net/` and confirm WiFi association plus the boot banner's link state
- [ ] Port `spotify/`; confirm a poll returns real playback state over serial
      before wiring any of it to the display
- [ ] Wire the knob to volume and touch to transport through `CommandQueue`,
      with the existing optimistic settle windows
- [ ] Port `ArtCache`; confirm covers download, cache to SD, and decode
- [ ] Swap `FakeSource` for `SpotifySource` in the real build; confirm
      "Cover Light" renders live playback
- [ ] Commit

**This task's completion is the project goal: a working Spotify player on the
board.**

---

## Self-Review

**Spec coverage.** Every section of the spec maps to a task:

| Spec section | Task |
|---|---|
| 4 Module layout | all |
| 5 Render pipeline | 1, 3, 4, 5, 15 |
| 5 Byte-order rule | 1 (framebuffer), 2 (dump), 18 (push) |
| 5 SRAM-band contingency | 18 (decision gate) |
| 6 Modulation bus | 8, 11 |
| 6 Procedural fallback + crossfade | 8, 11 |
| 7 Audio chain | 9, 10, 11, 20 |
| 8 Threading | 21 (`NetWorker` port); render/net split |
| 8 Elapsed-based timers | 6 (`FrameClock`), 12 (`Deadline`) |
| 9 Input | 19 |
| 9 Volume coalescing | 12 (queue), 21 (wiring) |
| 10 Cover Light | 13, 14, 15 |
| 11 Radial shell | **deferred to Plan 3**, per spec section 13 |
| 12 Testing | 2 and every task |
| 13 Milestones | task ordering |
| 14 Pin risk | 17 |
| 14 Bandwidth risk | 18 |

**Gap found and accepted:** spec section 11 (the radial shell — progress ring,
type, radial indicators, toasts) has no task here. The spec's own decomposition
assigns it to Plan 3, and "Cover Light" is judgeable without it. Task 21
therefore delivers a working player whose transport state is visible through the
view and the serial log, with the ring and typography following in Plan 3. This
is a deliberate scope boundary, not an omission.

**Placeholder scan.** No `TBD`, `TODO`, "handle edge cases", or "similar to Task
N". Tasks 7-21 give interfaces, non-obvious code, and enumerated test cases
rather than full step-by-step cycles; that is a deliberate density choice for
well-understood work, not a placeholder.

**Type consistency.** Checked across tasks: `gfx::Framebuffer`, `gfx::addSat`,
`gfx::fade`, `gfx::lerp565`, `gfx::rgb565`, `gfx::unpack565`,
`gfx::maskToCircle`, `gfx::ditherFrame`, `gfx::Bloom::apply`, `core::Rng`,
`core::FrameClock::MAX_DT`, `fnv1a`, `audio::Modulation`, `audio::Fft::N`,
`audio::Fft::BINS`, `audio::MicSource::read`, `art::Image::sample`,
`art::Palette::tint`, `views::ViewCtx`. `Fft::BINS` is referenced by
`OnsetDetector::prev_` and `AudioAnalyzer::mag_`, so `Fft.h` must be included by
both — noted in their interface blocks.
