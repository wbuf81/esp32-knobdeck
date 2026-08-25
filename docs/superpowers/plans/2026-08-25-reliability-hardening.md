# Reliability Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the five remaining gaps where this firmware detects a problem and then does nothing visible about it.

**Architecture:** Every task follows the pattern the project already uses for `Backlight`, `HostLink`, `GestureFlash::transportFeedbackVisible` and `core::CrashPolicy`: the decision is a pure function or small state machine in a host-testable header, and the device-only file is reduced to a call site. Nothing new talks to hardware. CI comes first because it protects every task after it.

**Tech Stack:** C++14, PlatformIO, Unity (host tests), ESP32-S3 Arduino/IDF 4.4.7, GitHub Actions.

**Spec:** No separate design doc. This plan implements the audit recorded in commits `156d2a9` and `02b6740` and the invariants in `CLAUDE.md`. Read both before starting.

## Global Constraints

- `HOMEBREW_PREFIX=/opt/homebrew` is required on **every** local `pio` command.
- Three targets must pass before every commit: `pio run -e native`, `pio test -e test`, `pio run -e esp32`.
- `KNOB_BANDS=1` must render **byte-identical** to a full frame. Any new draw path needs a bands-match-full-frame test.
- Effects and views take `dt` and a seed. Never read a clock inside a view; headless renders must stay bit-exact.
- Unknown renders as unknown. Never draw a confident default for a value the device does not have.
- Clip by discarding, never clamping. Clamping an off-screen range folds it onto the boundary.
- Never add a pass that reads *and* writes every pixel. Fold work into a write already being made.
- Never do per-pixel what can be per-row, or per-band what can be per-frame. `prepare()`/`render()` exists for this.
- `net/`, `spotify/`, `config/`, `art/CoverCache.cpp` are device-only and excluded from the `native` and `test` filters.
- Assert that a string replacement matched, then grep the file, before flashing.
- Never read or print `src/config/secrets.h`.
- macOS has no `timeout` command.

---

### Task 1: CI for all three targets

CI first: it is the only task that makes every later task's verification automatic rather than remembered.

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing.
- Produces: nothing consumed by later tasks. A green check on every push.

Key fact that shapes this file: the `test` env's `build_src_filter` excludes `platform/desktop/SdlPresent.cpp` and its `build_flags` carry no `-lSDL2`, so **`pio test -e test` and `pio run -e esp32` need no system libraries at all.** Only `env:native` needs SDL2, and it reads `${sysenv.HOMEBREW_PREFIX}` — on Ubuntu that is `/usr`, where `libsdl2-dev` puts headers in `/usr/include/SDL2`.

- [ ] **Step 1: Write the workflow**

```yaml
name: CI

on:
  push:
    branches: [main]
  pull_request:

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4

      - uses: actions/setup-python@v5
        with:
          python-version: '3.12'

      # Both the PlatformIO package index and the toolchains are large and
      # unchanging. Without this the esp32 toolchain is re-downloaded every run.
      - name: Cache PlatformIO
        uses: actions/cache@v4
        with:
          path: |
            ~/.platformio
            ~/.cache/pip
          key: pio-${{ runner.os }}-${{ hashFiles('platformio.ini') }}

      - name: Install PlatformIO
        run: pip install --upgrade platformio

      # env:native is the only target that needs SDL. Headers land in
      # /usr/include/SDL2, which is why HOMEBREW_PREFIX is set to /usr below.
      - name: Install SDL2
        run: sudo apt-get update && sudo apt-get install -y libsdl2-dev

      # secrets.h is gitignored and must never be committed. The example file
      # is what the device-only build compiles against in CI.
      - name: Provide a secrets stub
        run: cp src/config/secrets.h.example src/config/secrets.h

      - name: Host unit tests
        env:
          HOMEBREW_PREFIX: /usr
        run: pio test -e test

      - name: Desktop build
        env:
          HOMEBREW_PREFIX: /usr
        run: pio run -e native

      - name: Device build
        env:
          HOMEBREW_PREFIX: /usr
        run: pio run -e esp32
```

- [ ] **Step 2: Verify the secrets stub actually compiles**

The device build is the only one that includes `secrets.h`. Confirm the example file has every field the code reads, locally:

```bash
cd /Users/you/Vibecoding/esp32knobtouch
cp src/config/secrets.h src/config/secrets.h.mine
cp src/config/secrets.h.example src/config/secrets.h
HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 2>&1 | tail -3
```

Expected: SUCCESS. If it fails, add the missing fields to `secrets.h.example` (empty string values only — never real credentials).

- [ ] **Step 3: Restore your real secrets**

```bash
cd /Users/you/Vibecoding/esp32knobtouch
mv src/config/secrets.h.mine src/config/secrets.h
git status --short   # must NOT list src/config/secrets.h
```

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/ci.yml src/config/secrets.h.example
git commit -m "CI, so the three-target rule stops being a thing I remember"
```

- [ ] **Step 5: Push and confirm the run goes green**

```bash
git push origin main
gh run watch
```

Expected: all three steps pass. If the esp32 step fails on a missing `secrets.h` field, fix `secrets.h.example` and push again.

---

### Task 2: Render toasts

`AppState::showToast()` writes a field nothing reads. `toastActive()` has zero callers and `NowPlaying::prepare()` only receives `PlaybackState`, so it cannot see the toast. Six call sites are silent today. Task 3 depends on this one, so it comes first.

`AppState`'s own comment says the toast is "shown in the bottom strip in place of the time row" — so the timecodes must be suppressed while a toast is up, not drawn under it.

**Files:**
- Create: `src/shell/Toast.h`, `src/shell/Toast.cpp`
- Modify: `src/shell/NowPlaying.h` (add a `suppress_times` parameter to `prepare`)
- Modify: `src/shell/NowPlaying.cpp` (honour it)
- Modify: `src/main_esp32.cpp` (feed and draw it)
- Test: `test/test_logic/main.cpp`

**Interfaces:**
- Consumes: `gfx::drawTextCentered`, `gfx::fontArtist()`, `gfx::halfChordAt`, `shell::NowPlaying::TIME_BASELINE`, `shell::NowPlaying::MARGIN`.
- Produces:
  - `shell::Toast::prepare(const char *msg, bool active)` — once per frame.
  - `shell::Toast::render(gfx::Surface &s, uint16_t tint) const` — once per band.
  - `shell::Toast::visible() const -> bool`
  - `shell::NowPlaying::prepare(const PlaybackState &pb, uint32_t shown_ms, bool suppress_times)` — third parameter is new.

- [ ] **Step 1: Write the failing tests**

Add to `test/test_logic/main.cpp`, immediately before the `// Themes and the picker` banner:

```cpp
// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------

void test_toast_draws_nothing_when_inactive(void) {
  // The field is non-empty long after the deadline passes - showToast never
  // clears the text, only the Deadline. Drawing on text alone would leave the
  // last error on screen forever.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::Toast t;
  t.prepare("No active device", false);
  TEST_ASSERT_FALSE(t.visible());
  gfx::Surface s = fullSurface(fb);
  t.render(s, 0x07E0);
  int ink = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0x0000) ++ink;
  TEST_ASSERT_EQUAL_INT(0, ink);
}

void test_toast_draws_when_active(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::Toast t;
  t.prepare("No active device", true);
  TEST_ASSERT_TRUE(t.visible());
  gfx::Surface s = fullSurface(fb);
  t.render(s, 0x07E0);
  int ink = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0x0000) ++ink;
  TEST_ASSERT_TRUE(ink > 100);
}

void test_toast_is_not_visible_for_an_empty_message(void) {
  shell::Toast t;
  t.prepare("", true);
  TEST_ASSERT_FALSE(t.visible());
  t.prepare(nullptr, true);
  TEST_ASSERT_FALSE(t.visible());
}

void test_toast_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  shell::Toast a, b;
  a.prepare("Volume not supported", true);
  b.prepare("Volume not supported", true);
  gfx::Surface s = fullSurface(whole);
  a.render(s, 0x07E0);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.render(bs, 0x07E0);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_toast_stays_inside_the_chord(void) {
  // Text is measured against the CHORD at its baseline, not the 360px panel.
  // A long message that overflowed would run under the bezel where it cannot
  // be read, which is the same as not showing it.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::Toast t;
  t.prepare("a spotify error message far too long for a round screen", true);
  gfx::Surface s = fullSurface(fb);
  t.render(s, 0xFFFF);
  const int half = gfx::halfChordAt(shell::NowPlaying::TIME_BASELINE,
                                    shell::NowPlaying::MARGIN);
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0x0000) {
        TEST_ASSERT_TRUE(x >= gfx::CX - half - 1);
        TEST_ASSERT_TRUE(x <= gfx::CX + half + 1);
      }
}

void test_nowplaying_suppresses_times_for_a_toast(void) {
  // The toast takes the time row's place rather than overlapping it. Two
  // strings on one baseline is unreadable, and the timecodes are the less
  // urgent of the two.
  PlaybackState pb;
  pb.has_track = true;
  pb.duration_ms = 200000;
  pb.progress_ms = 60000;
  setStr(pb.title, sizeof(pb.title), "Title");
  setStr(pb.artist, sizeof(pb.artist), "Artist");

  gfx::Framebuffer with_times, without;
  with_times.fill(0x0000);
  without.fill(0x0000);

  shell::NowPlaying a, b;
  a.prepare(pb, 60000, false);
  b.prepare(pb, 60000, true);
  gfx::Surface sa = fullSurface(with_times);
  gfx::Surface sb = fullSurface(without);
  a.render(sa, 0x07E0);
  b.render(sb, 0x07E0);

  int ink_a = 0, ink_b = 0;
  for (int y = shell::NowPlaying::TIME_BASELINE - 14;
       y <= shell::NowPlaying::TIME_BASELINE + 2; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      if (with_times.at(x, y) != 0x0000) ++ink_a;
      if (without.at(x, y) != 0x0000) ++ink_b;
    }
  TEST_ASSERT_TRUE(ink_a > 20);
  TEST_ASSERT_EQUAL_INT(0, ink_b);
}
```

Register them next to the other `RUN_TEST` lines:

```cpp
  RUN_TEST(test_toast_draws_nothing_when_inactive);
  RUN_TEST(test_toast_draws_when_active);
  RUN_TEST(test_toast_is_not_visible_for_an_empty_message);
  RUN_TEST(test_toast_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_toast_stays_inside_the_chord);
  RUN_TEST(test_nowplaying_suppresses_times_for_a_toast);
```

And add the include next to `#include "shell/NowPlaying.h"`:

```cpp
#include "shell/Toast.h"
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | grep -E "fatal error|error:" | head -3
```

Expected: `fatal error: 'shell/Toast.h' file not found`.

- [ ] **Step 3: Create `src/shell/Toast.h`**

```cpp
#pragma once

// A transient message on the bottom strip.
//
// This exists because showToast() spent the project's whole life writing a
// field no renderer read. Six call sites - "No active device", "Nothing
// playing", "Volume not supported", "Not allowed", "Command failed", "Bad
// response" - were all silent, which is worse than having no error reporting
// at all: the code LOOKS like it reports errors, so nobody goes looking for
// the reason a gesture did nothing.
//
// prepare()/render() split like NowPlaying, for the same measured reason:
// measuring and fitting a string eighteen times per frame once cost half the
// frame rate.
//
// Drawn in place of the time row, not over it. Two strings sharing one
// baseline is unreadable, and between a timecode you can infer and an error you
// cannot, the error wins.

#include <cstdint>

#include "gfx/Surface.h"

namespace shell {

class Toast {
 public:
  // Once per frame. `msg` may be null or empty, and `active` is the caller's
  // deadline check - both must be respected, because showToast never clears
  // the text, only the Deadline. Drawing on non-empty text alone would leave
  // the last error on screen forever.
  void prepare(const char *msg, bool active);

  bool visible() const { return visible_; }

  // Once per band. Draws only; measures nothing.
  void render(gfx::Surface &s, uint16_t tint) const;

 private:
  char text_[64] = {};
  int x_ = 0;
  bool visible_ = false;
};

}  // namespace shell
```

- [ ] **Step 4: Create `src/shell/Toast.cpp`**

```cpp
#include "Toast.h"

#include "NowPlaying.h"
#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"

namespace shell {

void Toast::prepare(const char *msg, bool active) {
  visible_ = active && msg != nullptr && msg[0] != '\0';
  if (!visible_) {
    text_[0] = '\0';
    return;
  }
  setStr(text_, sizeof(text_), msg);
  // Measured against the chord at this baseline, not the panel width. Anything
  // wider is truncated by drawTextFit at render time rather than running under
  // the bezel.
  x_ = gfx::CX;
}

void Toast::render(gfx::Surface &s, uint16_t tint) const {
  if (!visible_) return;
  const int half =
      gfx::halfChordAt(NowPlaying::TIME_BASELINE, NowPlaying::MARGIN);
  gfx::drawTextFit(s, gfx::fontArtist(), x_, NowPlaying::TIME_BASELINE, text_,
                   half * 2, tint);
}

}  // namespace shell
```

`setStr` is declared at `src/core/PlaybackState.h:53`, so add that include to `Toast.cpp`:

```cpp
#include "core/PlaybackState.h"
```

- [ ] **Step 5: Add the `suppress_times` parameter to NowPlaying**

In `src/shell/NowPlaying.h`:

```cpp
  // Once per frame, before any band. `suppress_times` drops the timecode row,
  // which is where a toast goes - see shell::Toast.
  void prepare(const PlaybackState &pb, uint32_t shown_ms, bool suppress_times);
```

In `src/shell/NowPlaying.cpp`, the signature is at line 74 and `have_times_` is set `false` at line 76 (the reset) and `true` at line 112 (the end of the timecode block). Change the signature:

```cpp
void NowPlaying::prepare(const PlaybackState &pb, uint32_t shown_ms,
                         bool suppress_times) {
```

and gate the `true` at line 112 — this is the only assignment that needs touching, because line 76 already defaults it off:

```cpp
    have_times_ = !suppress_times;
```

Verify both edits landed before building:

```bash
grep -n "suppress_times" src/shell/NowPlaying.cpp
```

Expected: two hits — the signature and line 112.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -4
```

Expected: PASS, count increased by 6.

- [ ] **Step 7: Wire it into the device**

In `src/main_esp32.cpp`:

Add the include next to the other `shell/` includes:

```cpp
#include "shell/Toast.h"
```

Add the global next to `shell::GestureFlash g_flash;`:

```cpp
shell::Toast g_toast;
```

In `loop()`, where `g_nowplaying.prepare(...)` is called, add the toast prepare immediately before it and pass its visibility through:

```cpp
  g_toast.prepare(st.toast, st.toastActive(now));
  g_nowplaying.prepare(st.pb, shown_progress, g_toast.visible());
```

In the band loop, draw the toast immediately before `g_flash.render(s, view_tint)` so the flash still sits on top of everything:

```cpp
      g_toast.render(s, view_tint);
```

The toast must draw on the browser screens too — a "No active device" from a list is exactly as real — so put that line outside the `if (g_screen == ...)` chain, next to the `g_flash.render` call.

- [ ] **Step 8: Verify all three targets**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio run -e native 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 2>&1 | tail -2
```

Expected: SUCCESS, PASS, SUCCESS.

- [ ] **Step 9: Flash and confirm on hardware**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 -t upload 2>&1 | tail -3
```

With nothing playing, long-press the screen. Expected: "Nothing playing" appears on the bottom strip for about two seconds. This is the first time that message has ever been visible.

- [ ] **Step 10: Commit**

```bash
git add -A
git commit -m "Toasts that render, after a lifetime of writing to nobody"
```

---

### Task 3: Report dropped commands

`CommandQueue::push()` returns `false` when the 8-deep ring is full — "drop rather than block" — and `NetWorker::submit()` is declared `void`, so that return is discarded at both call sites. A dropped command is a gesture that did nothing, *after* the glyph already flashed to say it worked. That is the exact bug class `GestureFlash` was built to eliminate, reintroduced one layer down.

Depends on Task 2: the feedback channel has to exist first.

**Files:**
- Modify: `src/net/NetWorker.h:45` (`submit` returns `bool`)
- Modify: `src/net/NetWorker.cpp` (return what the queue said)
- Modify: `src/main_esp32.cpp:337` and the `send && g_net` site (act on it)
- Test: `test/test_logic/main.cpp` (the queue's own contract)

**Interfaces:**
- Consumes: `shell::Toast` from Task 2 via `AppState::showToast`.
- Produces: `bool NetWorker::submit(const Command &in)` — `true` if queued, `false` if dropped.

Note: `net/` is excluded from the host build, so `NetWorker` itself cannot be host-tested. `core/CommandQueue.h` **is** in the host build, so the contract that matters gets a test.

- [ ] **Step 1: Write the failing test**

Add to `test/test_logic/main.cpp` before the `// Themes and the picker` banner:

```cpp
// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------

void test_command_queue_reports_a_drop_rather_than_hiding_it(void) {
  // The ring holds CAPACITY-1 entries; the last slot distinguishes full from
  // empty. What matters is that the caller is TOLD - a silently dropped
  // command is a gesture that did nothing after the glyph said it worked.
  CommandQueue<4> q;
  Command c;
  c.type = CommandType::Next;
  TEST_ASSERT_TRUE(q.push(c));
  TEST_ASSERT_TRUE(q.push(c));
  TEST_ASSERT_TRUE(q.push(c));
  TEST_ASSERT_FALSE(q.push(c));
}

void test_command_queue_accepts_again_after_a_pop(void) {
  CommandQueue<4> q;
  Command c;
  c.type = CommandType::Next;
  while (q.push(c)) {
  }
  Command out;
  TEST_ASSERT_TRUE(q.pop(&out));
  TEST_ASSERT_TRUE(q.push(c));
}

void test_coalesced_push_reports_a_drop_when_it_cannot_coalesce(void) {
  // The path that was silently dropping: nothing of that type pending AND the
  // ring full, so pushCoalesced falls through to push() - whose answer it used
  // to throw away.
  CommandQueue<4> q;
  Command n;
  n.type = CommandType::Next;
  while (q.push(n)) {
  }
  Command v;
  v.type = CommandType::SetVolume;
  TEST_ASSERT_FALSE(q.pushCoalesced(v));
}

void test_coalesced_volume_never_fills_the_queue(void) {
  // A fast spin must not be able to push out a pending play/pause. Coalescing
  // replaces the pending volume rather than appending, so forty detents cost
  // one slot.
  CommandQueue<4> q;
  Command v;
  v.type = CommandType::SetVolume;
  for (int i = 0; i < 40; ++i) {
    v.arg = i;
    q.pushCoalesced(v);
  }
  Command other;
  other.type = CommandType::PlayPause;
  TEST_ASSERT_TRUE(q.push(other));
}
```

Register them:

```cpp
  RUN_TEST(test_command_queue_reports_a_drop_rather_than_hiding_it);
  RUN_TEST(test_command_queue_accepts_again_after_a_pop);
  RUN_TEST(test_coalesced_push_reports_a_drop_when_it_cannot_coalesce);
  RUN_TEST(test_coalesced_volume_never_fills_the_queue);
```

Add the include if absent:

```cpp
#include "core/CommandQueue.h"
```

- [ ] **Step 2: Run the tests**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | grep -E "command_queue|coalesced|error:" | head
```

Expected: `test_coalesced_push_reports_a_drop_when_it_cannot_coalesce` **fails to compile** — `pushCoalesced` returns `void` today, so it cannot be used in `TEST_ASSERT_FALSE`. That is the genuine RED for this task.

The other three will **pass immediately**; they document an existing contract rather than a new one. That is acceptable for those three only: they are the regression guard for Step 3, which changes `submit`'s signature and could otherwise quietly invert the meaning of the return. If any of *them* fails, the queue does not behave the way the rest of this task assumes — stop and re-read `core/CommandQueue.h`.

- [ ] **Step 3: Make `submit` return the queue's answer**

In `src/net/NetWorker.h`, replace line 45:

```cpp
  // True if queued. FALSE means the ring was full and the command was dropped -
  // the caller must say so, because the glyph has usually already flashed by
  // the time this is called.
  bool submit(const Command &in);
```

In `src/net/NetWorker.cpp`:

```cpp
bool NetWorker::submit(const Command &in) {
  Command c = in;
  c.submitted_ms = nowMs();
  std::lock_guard<std::mutex> lk(mtx_);
  if (c.type == CommandType::SetVolume) {
    // Only the final volume matters; replace any pending one. Coalescing
    // cannot fail - it either replaces a pending entry or pushes one.
    return cmds_.pushCoalesced(c);
  }
  return cmds_.push(c);
}
```

`pushCoalesced` currently returns **`void`** and, worse, calls `push(c)` and throws away its `bool` — so a coalesced volume command into a full ring is silently dropped one level deeper than `submit`. Both need fixing. In `src/core/CommandQueue.h`, replace the whole method:

```cpp
  // Returns false only when there was nothing to coalesce with AND the ring
  // was full. The discarded push() return here was the same silent-drop bug as
  // submit()'s void, one level further down.
  bool pushCoalesced(Command c) {
    for (int i = tail_; i != head_; i = (i + 1) % CAPACITY) {
      if (buf_[i].type == c.type) {
        buf_[i] = c;
        return true;
      }
    }
    return push(c);
  }
```

- [ ] **Step 4: Act on it at both call sites**

In `src/main_esp32.cpp`, the volume site (around line 337):

```cpp
        if (!g_net->submit(c)) {
          // The knob outran the net task. Saying so is the whole point: this
          // used to be a turn that changed the ring and nothing else.
          g_net->mutate([now](AppState &a) { a.showToast("Busy", now); });
        }
```

And the main gesture site (around line 658):

```cpp
  if (send && g_net && !g_net->submit(c)) {
    g_net->mutate([now](AppState &a) { a.showToast("Busy, try again", now); });
    LOGF("command DROPPED: queue full");
  }
```

- [ ] **Step 5: Verify all three targets**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio run -e native 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 2>&1 | tail -2
```

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "A dropped command says so, instead of flashing a glyph and lying"
```

---

### Task 4: Heap floor alarm

`Diag.h` declared `heapTick()` and was deleted with it unimplemented. The number matters: TLS handshakes are the largest transient allocation on the device, and free internal heap measured 48–51 KB during playback. Falling below the handshake's needs shows up as network requests failing for no visible reason.

**Files:**
- Create: `src/core/HeapPolicy.h`
- Modify: `src/platform/esp32/Boot.h`, `src/platform/esp32/Boot.cpp` (the device call site)
- Modify: `src/main_esp32.cpp` (call it once per loop)
- Test: `test/test_logic/main.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `core::HeapWatch` with `bool observe(size_t free_bytes)` — returns `true` exactly once per crossing below the floor.
  - `core::HEAP_FLOOR_BYTES`, `core::HEAP_CLEAR_BYTES`
  - `esp32::heapTick(uint32_t now_ms)` — device call site.

- [ ] **Step 1: Write the failing tests**

```cpp
// ---------------------------------------------------------------------------
// Heap floor
// ---------------------------------------------------------------------------

void test_heap_watch_is_quiet_while_there_is_room(void) {
  core::HeapWatch w;
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES + 1));
  TEST_ASSERT_FALSE(w.observe(200000));
}

void test_heap_watch_fires_once_on_crossing(void) {
  // Once, not every frame. At 126 fps a per-frame warning is a serial flood
  // that pushes the thing you needed to read off the top of the buffer - and
  // this project has already learned that a diagnostic can be the bug.
  core::HeapWatch w;
  TEST_ASSERT_TRUE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(1000));
}

void test_heap_watch_rearms_only_after_real_recovery(void) {
  // Hysteresis. Rearming at the floor itself would make a value hovering on
  // the boundary fire on alternate frames, which is the flood again.
  core::HeapWatch w;
  TEST_ASSERT_TRUE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES + 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_CLEAR_BYTES));
  TEST_ASSERT_TRUE(w.observe(core::HEAP_FLOOR_BYTES - 1));
}

void test_the_heap_floor_leaves_room_for_a_tls_handshake(void) {
  // Measured on hardware: free internal heap sits at 48-51 KB during playback
  // with artwork decoded. The floor has to be below that and above what a
  // handshake needs, or it either never fires or never stops firing.
  TEST_ASSERT_TRUE(core::HEAP_FLOOR_BYTES < 48000);
  TEST_ASSERT_TRUE(core::HEAP_FLOOR_BYTES > 16000);
  TEST_ASSERT_TRUE(core::HEAP_CLEAR_BYTES > core::HEAP_FLOOR_BYTES);
}
```

Register:

```cpp
  RUN_TEST(test_heap_watch_is_quiet_while_there_is_room);
  RUN_TEST(test_heap_watch_fires_once_on_crossing);
  RUN_TEST(test_heap_watch_rearms_only_after_real_recovery);
  RUN_TEST(test_the_heap_floor_leaves_room_for_a_tls_handshake);
```

Include:

```cpp
#include "core/HeapPolicy.h"
```

- [ ] **Step 2: Run to verify failure**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | grep -E "fatal error" | head -2
```

Expected: `fatal error: 'core/HeapPolicy.h' file not found`.

- [ ] **Step 3: Create `src/core/HeapPolicy.h`**

```cpp
#pragma once

// When free heap has fallen far enough to matter.
//
// Pure, like CrashPolicy, and for the same reason: the interesting part is the
// hysteresis, not the reading. `Diag.h` declared a heapTick() that was never
// written, and the number it would have watched is the one that decides
// whether TLS keeps working - mbedTLS handshakes are the largest transient
// allocation on this device, and when they start failing the symptom is
// requests quietly not happening.
//
// Measured on hardware: free internal heap sits at 48-51 KB during playback
// with artwork decoded, and at ~104 KB before the first cover lands.

#include <cstddef>

namespace core {

// Below this, a handshake is at risk.
constexpr size_t HEAP_FLOOR_BYTES = 24000;

// And it must climb back to here before the alarm rearms. Rearming at the
// floor would make a value hovering on the boundary fire on alternate frames,
// which is a serial flood - and this project has already learned twice that a
// diagnostic can be the bug.
constexpr size_t HEAP_CLEAR_BYTES = 34000;

class HeapWatch {
 public:
  // Call every frame with the current free heap. True EXACTLY ONCE per
  // crossing, so the caller can log or toast without rate-limiting it.
  bool observe(size_t free_bytes) {
    if (armed_ && free_bytes < HEAP_FLOOR_BYTES) {
      armed_ = false;
      return true;
    }
    if (!armed_ && free_bytes >= HEAP_CLEAR_BYTES) armed_ = true;
    return false;
  }

 private:
  bool armed_ = true;
};

}  // namespace core
```

- [ ] **Step 4: Run to verify the tests pass**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -3
```

- [ ] **Step 5: Add the device call site**

In `src/platform/esp32/Boot.h`, before the closing `}  // namespace esp32`:

```cpp
// Call once per loop. Watches free internal heap and shouts once if it falls
// below the level where TLS handshakes start failing. See core/HeapPolicy.h.
void heapTick(uint32_t now_ms);
```

In `src/platform/esp32/Boot.cpp`, add the include:

```cpp
#include "core/HeapPolicy.h"
```

and the implementation next to `noteUptime`:

```cpp
void heapTick(uint32_t now_ms) {
  static core::HeapWatch watch;
  // INTERNAL heap specifically. PSRAM is 8 MB and irrelevant here: mbedTLS
  // allocates its handshake buffers from internal SRAM, so a PSRAM figure
  // would read healthy right up until every request started failing.
  const size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (watch.observe(free_internal)) {
    Serial.printf(
        "HEAP LOW: %lu bytes internal free at %lu s - TLS may start failing\n",
        (unsigned long)free_internal, (unsigned long)(now_ms / 1000));
  }
}
```

- [ ] **Step 6: Call it from the loop**

In `src/main_esp32.cpp`, next to `esp32::noteUptime(millis());`:

```cpp
  esp32::heapTick(millis());
```

- [ ] **Step 7: Verify all three targets and flash**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio run -e native 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 -t upload 2>&1 | tail -2
```

Expected on hardware: **no** HEAP LOW line during normal playback. If one appears immediately, the floor is set above this board's working range — re-read the `heap` figure in the fps status line and lower `HEAP_FLOOR_BYTES` below it, then update `test_the_heap_floor_leaves_room_for_a_tls_handshake`.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "The heap floor Diag.h promised, with hysteresis so it says it once"
```

---

### Task 5: Make Player-screen gesture routing testable

~120 lines of screen×gesture switch live in `main_esp32.cpp`, which is device-only and excluded from both host filters. The two swipe guards shipped in `156d2a9` are verified by flashing alone, and the constraint that keeps the device usable — *swipe-up is the only route to Playlists, and Playlists is the only way to start playback or reach THEMES* — is a comment someone has to remember rather than an assertion.

**Scoped to the Player screen only.** That is where every rule added recently lives. The list screens keep their existing inline handling; extracting them is follow-on work and should not block this.

**Files:**
- Create: `src/input/Route.h`, `src/input/Route.cpp`
- Modify: `src/main_esp32.cpp` (the `case Screen::Player:` block becomes a call)
- Test: `test/test_logic/main.cpp`

**Interfaces:**
- Consumes: `input::Gesture`, `PlaybackState`, `CommandType`, `shell::Glyph`, `views::DaisyIdle::Reaction`, `shell::transportFeedbackVisible`.
- Produces:
  - `input::Screen` — moved out of `main_esp32.cpp` so it can be named in a signature.
  - `input::Action` — the struct below.
  - `input::Action routePlayer(Gesture g, const PlaybackState &pb)`

`Screen` currently lives as a file-local enum in `main_esp32.cpp`. Moving it to `input/Route.h` is part of this task; `main_esp32.cpp` then uses `input::Screen` throughout.

- [ ] **Step 1: Write the failing tests**

```cpp
// ---------------------------------------------------------------------------
// Player gesture routing
// ---------------------------------------------------------------------------

namespace {
PlaybackState playingTrack() {
  PlaybackState pb;
  pb.has_track = true;
  pb.has_device = true;
  pb.is_playing = true;
  return pb;
}
PlaybackState nothingListening() {
  PlaybackState pb;
  pb.has_track = false;
  pb.has_device = false;
  return pb;
}
}  // namespace

void test_tap_with_a_track_sends_playpause_and_flashes(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::Tap, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::PlayPause, a.command);
  TEST_ASSERT_TRUE(a.show_glyph);
  TEST_ASSERT_EQUAL(input::Screen::Player, a.screen);
}

void test_tap_with_nothing_listening_is_the_dog_alone(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::Tap, nothingListening());
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_FALSE(a.show_glyph);
  TEST_ASSERT_TRUE(a.poke_dog);
  TEST_ASSERT_EQUAL(views::DaisyIdle::Reaction::Touch, a.dog);
}

void test_swipe_down_with_a_track_opens_the_queue(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::SwipeDown, playingTrack());
  TEST_ASSERT_EQUAL(input::Screen::Tracks, a.screen);
  TEST_ASSERT_EQUAL(CommandType::FetchQueue, a.command);
}

void test_swipe_down_with_no_track_is_swallowed(void) {
  // The queue is what comes up AFTER something. With no current track it used
  // to jump to a list that was always empty, and shoved the dog aside to do it.
  const input::Action a =
      input::routePlayer(input::Gesture::SwipeDown, nothingListening());
  TEST_ASSERT_EQUAL(input::Screen::Player, a.screen);
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_TRUE(a.poke_dog);
}

void test_swipe_up_always_reaches_playlists(void) {
  // THE load-bearing assertion of this file. Swipe up is the only route to
  // Playlists, and Playlists is the only way to start playback or reach
  // THEMES. Gating it on a track would mean nothing could ever be started -
  // precisely when nothing is playing. Symmetry with swipe-down would be a bug,
  // and this test is here so a future tidy-up cannot introduce it.
  const input::Action with = input::routePlayer(input::Gesture::SwipeUp,
                                                playingTrack());
  const input::Action without = input::routePlayer(input::Gesture::SwipeUp,
                                                   nothingListening());
  TEST_ASSERT_EQUAL(input::Screen::Playlists, with.screen);
  TEST_ASSERT_EQUAL(input::Screen::Playlists, without.screen);
  TEST_ASSERT_EQUAL(CommandType::FetchPlaylists, without.command);
}

void test_swipes_with_a_track_skip_and_flash(void) {
  const input::Action l = input::routePlayer(input::Gesture::SwipeLeft,
                                             playingTrack());
  const input::Action r = input::routePlayer(input::Gesture::SwipeRight,
                                             playingTrack());
  TEST_ASSERT_EQUAL(CommandType::Previous, l.command);
  TEST_ASSERT_EQUAL(CommandType::Next, r.command);
  TEST_ASSERT_TRUE(l.show_glyph);
  TEST_ASSERT_TRUE(r.show_glyph);
}

void test_swipes_with_nothing_listening_only_wag(void) {
  const input::Action l = input::routePlayer(input::Gesture::SwipeLeft,
                                             nothingListening());
  TEST_ASSERT_EQUAL(CommandType::None, l.command);
  TEST_ASSERT_FALSE(l.show_glyph);
  TEST_ASSERT_EQUAL(views::DaisyIdle::Reaction::Swipe, l.dog);
}

void test_long_press_with_a_track_toggles_like(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::LongPress, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::ToggleLike, a.command);
  TEST_ASSERT_TRUE(a.show_glyph);
}

void test_long_press_with_nothing_listening_gets_zoomies(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::LongPress, nothingListening());
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_EQUAL(views::DaisyIdle::Reaction::Hold, a.dog);
  TEST_ASSERT_TRUE(a.toast[0] != '\0');
}

void test_no_gesture_routes_to_nothing(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::None, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_FALSE(a.show_glyph);
  TEST_ASSERT_FALSE(a.poke_dog);
}
```

Register all ten, and include:

```cpp
#include "input/Route.h"
```

- [ ] **Step 2: Run to verify failure**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | grep -E "fatal error" | head -2
```

Expected: `fatal error: 'input/Route.h' file not found`.

- [ ] **Step 3: Create `src/input/Route.h`**

```cpp
#pragma once

// What a gesture on the player screen means, as a pure function.
//
// This exists because the rules got subtle and lived somewhere no test could
// reach. main_esp32.cpp is device-only - excluded from both the native and
// test filters - so every rule in it was verified by flashing and squinting.
// Two of those rules are now load-bearing and non-obvious:
//
//   - transport feedback is suppressed when nothing is listening, so a glyph
//     never promises a player that is not there;
//   - swipe DOWN is swallowed with no track, but swipe UP is NOT, because up
//     is the only route to Playlists and Playlists is the only way to start
//     playback or reach THEMES. Making them symmetric would leave the device
//     unable to start music, precisely when nothing is playing.
//
// The second one is a comment in a switch today. Here it is a test.

#include <cstdint>

#include "core/CommandQueue.h"
#include "core/PlaybackState.h"
#include "input/Gesture.h"
#include "shell/Glyphs.h"
#include "views/DaisyIdle.h"

namespace input {

enum class Screen : uint8_t { Player, Playlists, Tracks, Themes, Confirm };

// Everything the caller has to do, as data. No side effects here: the point is
// that the decision can be made in a test and applied on a device.
struct Action {
  Screen screen = Screen::Player;          // same as current = stay
  CommandType command = CommandType::None; // None = send nothing
  shell::Glyph glyph = shell::Glyph::Play;
  bool show_glyph = false;
  views::DaisyIdle::Reaction dog = views::DaisyIdle::Reaction::Touch;
  bool poke_dog = false;
  // Optimistic local edits the caller must apply BEFORE queueing, because
  // runCommand picks its verb by reading the flipped field.
  bool flip_playing = false;
  bool flip_liked = false;
  bool haptic_bump = false;  // false means the lighter click
  char toast[32] = {};
};

// `pb` carries both has_track and has_device; the transport rules read both.
Action routePlayer(Gesture g, const PlaybackState &pb);

}  // namespace input
```

- [ ] **Step 4: Create `src/input/Route.cpp`**

```cpp
#include "Route.h"

#include <cstring>

#include "shell/GestureFlash.h"

namespace input {

Action routePlayer(Gesture g, const PlaybackState &pb) {
  Action a;
  const bool transport =
      shell::transportFeedbackVisible(pb.has_track, pb.has_device);
  // Inside the player screen, "no track" is exactly "the dog is on screen".
  const bool dog = !pb.has_track;

  switch (g) {
    case Gesture::Tap:
      if (dog) {
        a.poke_dog = true;
        a.dog = views::DaisyIdle::Reaction::Touch;
      }
      if (!transport) return a;
      a.flip_playing = true;
      a.command = CommandType::PlayPause;
      // The glyph shows the state you are NOW IN, not the button you pressed.
      a.glyph = pb.is_playing ? shell::Glyph::Pause : shell::Glyph::Play;
      a.show_glyph = true;
      return a;

    case Gesture::SwipeLeft:
    case Gesture::SwipeRight:
      if (dog) {
        a.poke_dog = true;
        a.dog = views::DaisyIdle::Reaction::Swipe;
      }
      a.haptic_bump = true;
      if (!transport) return a;
      a.command = g == Gesture::SwipeLeft ? CommandType::Previous
                                         : CommandType::Next;
      a.glyph = g == Gesture::SwipeLeft ? shell::Glyph::Previous
                                        : shell::Glyph::Next;
      a.show_glyph = true;
      return a;

    case Gesture::LongPress:
      a.haptic_bump = true;
      if (!pb.has_track) {
        // The refusal is still owed - a refusal the user cannot see is the
        // same as a bug. But a crossed-out heart only refuses something
        // SPECIFIC, and with nothing listening there is no track to refuse.
        if (dog) {
          a.poke_dog = true;
          a.dog = views::DaisyIdle::Reaction::Hold;
        }
        if (transport) {
          a.glyph = shell::Glyph::HeartSlash;
          a.show_glyph = true;
        }
        std::snprintf(a.toast, sizeof(a.toast), "Nothing playing");
        return a;
      }
      a.flip_liked = true;
      a.command = CommandType::ToggleLike;
      a.glyph = pb.liked ? shell::Glyph::HeartOutline : shell::Glyph::HeartFilled;
      a.show_glyph = true;
      return a;

    case Gesture::SwipeDown:
      a.haptic_bump = true;
      if (!pb.has_track) {
        // The queue is what comes up AFTER something.
        a.poke_dog = true;
        a.dog = views::DaisyIdle::Reaction::Swipe;
        return a;
      }
      a.screen = Screen::Tracks;
      a.command = CommandType::FetchQueue;
      a.glyph = shell::Glyph::ChevronDown;
      a.show_glyph = true;
      return a;

    case Gesture::SwipeUp:
      // NOT gated on a track. See the header.
      a.haptic_bump = true;
      a.screen = Screen::Playlists;
      a.command = CommandType::FetchPlaylists;
      a.glyph = shell::Glyph::ChevronUp;
      a.show_glyph = true;
      return a;

    case Gesture::None:
    default:
      return a;
  }
}

}  // namespace input
```

Add `#include <cstdio>` if `snprintf` is not already reachable.

- [ ] **Step 5: Run to verify the tests pass**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -3
```

Expected: PASS, count increased by 10.

- [ ] **Step 6: Replace the inline Player switch with the call**

In `src/main_esp32.cpp`, delete the file-local `enum class Screen` and use `input::Screen` everywhere (`g_screen`'s type, and every `Screen::X` becomes `input::Screen::X`). Then replace the whole `case input::Screen::Player:` block with:

```cpp
      case input::Screen::Player: {
        const input::Action act = input::routePlayer(g, st.pb);
        if (act.poke_dog) g_dog.react(act.dog);
        if (act.show_glyph) g_flash.show(act.glyph, now);
        if (act.toast[0] && g_net) {
          const char *msg = act.toast;
          g_net->mutate([msg, now](AppState &a) { a.showToast(msg, now); });
        }
        // Local flips BEFORE the command is queued: runCommand chooses /play
        // vs /pause and PUT vs DELETE by reading these fields.
        if (act.flip_playing && g_net) {
          g_net->mutate([](AppState &a) {
            a.pb.is_playing = !a.pb.is_playing;
            a.settle_playing.arm(millis(), 1500);
          });
        }
        if (act.flip_liked && g_net) {
          g_net->mutate([](AppState &a) {
            a.pb.liked = !a.pb.liked;
            a.pb.liked_known = true;
            a.settle_liked.arm(millis(), 2000);
          });
        }
        if (act.screen != input::Screen::Player) {
          g_screen = act.screen;
          g_sel = 0;
          g_sel_pos = 0.0f;
        }
        if (act.command != CommandType::None) {
          c.type = act.command;
          send = true;
        }
        if (act.haptic_bump) esp32::hapticsBump();
        else if (g != input::Gesture::None) esp32::hapticsClick();
        break;
      }
```

- [ ] **Step 7: Verify all three targets**

```bash
HOMEBREW_PREFIX=/opt/homebrew pio run -e native 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio test -e test 2>&1 | tail -2
HOMEBREW_PREFIX=/opt/homebrew pio run -e esp32 2>&1 | tail -2
```

- [ ] **Step 8: Flash and re-check every player gesture by hand**

This task moves working code, so the risk is regression, not new behaviour. With nothing playing: tap → alert, swipe L/R → wag, swipe down → wag and **no** screen change, swipe up → Playlists opens, long press → zoomies plus a "Nothing playing" toast. With music playing: tap pauses, swipes skip, long press hearts, swipe down opens the queue, swipe up opens Playlists.

- [ ] **Step 9: Commit**

```bash
git add -A
git commit -m "Player routing becomes a pure function, so its rules become tests"
```

---

## Follow-on, deliberately not in this plan

- **Extract the list screens' routing** the same way. Worth doing once a fourth screen appears; not before.
- **`NetWorker::stalled()` does something.** It is called at `main_esp32.cpp:840` and only logs. The obvious response — restart the net task — needs care, because the heartbeat's own comment records that the age can legitimately read as "ahead of us" and an earlier version rebooted the device mid-song.
- **A desktop `HttpClient`** so `net/` and `spotify/` come into the host build. `platformio.ini`'s own comment already argues for it: it is how the ancestor project tuned optimistic-UI timing without a board. That single change would make the largest untested surface testable.
