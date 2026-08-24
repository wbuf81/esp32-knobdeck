# Knob Spotify Player — working notes

Hardware facts, measured numbers and traps: **`docs/BOARD_REFERENCE.md`**. Read it before
optimising or touching the panel, encoder, or memory layout.

## Build and flash

- `HOMEBREW_PREFIX=/opt/homebrew` is required on *every* `pio` command or the toolchain isn't found.
- Three targets: `pio run -e native`, `pio test -e test`, `pio run -e esp32`. **Verify all three before committing** — device-only code silently breaks the desktop build.
- `test_build_src = yes` is what makes PlatformIO compile `src/` into the test binary.
- `net/`, `spotify/`, `config/`, `art/CoverCache.cpp` are device-only; they're excluded from the `native` and `test` filters.
- macOS has no `timeout` command.

## Serial

- **Opening the port resets the board.** Counters that accumulate across a user interaction are destroyed by reading them.
- Hold one stream open (`scratchpad/tail.py`) rather than reopening per look, and stream line-by-line — a capture that buffers until it exits is unreadable exactly when you need it.
- **Kill the capture before flashing.** Upload fails while the port is held, and it fails quietly if you're grepping for `SUCCESS`.
- A crash loop takes the USB-CDC serial down with it: no log, and esptool can't sync in any reset mode. Recovery is unplug/replug and catching the boot window.

## Hard-won rules

- **Measure before optimising, and again after.** Every guess made in this project was wrong at least once; per-pass timers in the status line are how each real cost was found.
- **Clip by discarding, never clamping.** Clamping an off-screen range folds it onto the boundary — three separate bugs, one mistake.
- **Never add a pass that reads *and* writes every pixel.** Fold work into a write you're already making.
- **Never do per-pixel what can be per-row, or per-band what can be per-frame.** `prepare()`/`render()` split exists for this; violating it cost half the frame rate twice.
- **A diagnostic can be the bug.** A left-in `pinMode` paused the PCNT it was debugging; a 600-sample burst poll cost 24 ms/frame.
- **Verify with the strict client.** A response curl accepts, Node's `fetch` may reject — testing with the forgiving one hid a bug where both halves looked correct.

## Invariants

- `KNOB_BANDS=1` must render **byte-identical** to a full frame. This single assertion caught three clipping bugs; check it after any change to a draw path.
- Headless runs are bit-exact: effects take `dt` and a seed, never read a clock. Keep it that way or the pixel tests go flaky.
- Unknown renders as unknown — `volume_pct == -1` and `liked_known == false` must never draw as a confident default.

## Patching source

- **Assert that a string replacement matched, then grep the file, before flashing.** Silent no-op patches meant flashing unchanged code twice while reasoning about the output.

## Secrets

- Never read or print `src/config/secrets.h`. `tools/get_refresh_token.py` needs a real TTY (the `!` prefix has none) and preserves fields you skip.
- The playlist browser needs `playlist-read-private`; the boot banner prints granted scopes, which is the only place a missing one shows up.
