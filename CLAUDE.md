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
- A crash loop takes the USB-CDC serial down with it: no log, and esptool can't sync in any reset mode. Recovery is unplug/replug and catching the boot window — script the catch (poll for the port, then upload immediately); a human cannot time it.
- No pyserial on this machine. Raw `os.open` + `termios` at B115200 works. A **DTR pulse does not reset an S3** over native USB-CDC — reflash to reset.
- **Verify device-only behaviour with a build-flag-gated probe**, then remove it. `-DWDT_SELFTEST` proved the watchdog barks and safe mode catches the loop; `-DWAKE_PROBE` proved a JSON parse against the live API without a side effect.

## Hard-won rules

- **A detector with no visible response is not a feature.** This codebase keeps building the sensor and skipping the actuator: `has_device` counted and unread, `showToast` writing a field no renderer read, `crashStreak` persisted and ignored, `Diag.h`'s watchdog designed and never implemented, `pushCoalesced` discarding the one bool that mattered. Grep for callers before believing something works.
- **Make a failure visible before fixing it.** A 429 was invisible for the project's whole life — its only signal was a toast that rendered nowhere — so "it stopped working" was a correctly-waiting device with no way to say so.
- **Backoff state in RAM is erased by every flash.** Twenty reflashes meant twenty fresh strikes against the Spotify quota. Persist it — but as a signal to *probe slowly*, not a duration to serve: there is no wall clock on this board, so a remembered wait can outlive the limit.
- **Measure before optimising, and again after.** Every guess made in this project was wrong at least once, in both directions; per-pass timers in the status line are how each real cost was found. The Record backdrop was 23.8 ms before it was 8.2 ms, and the Mac helper's "essentially zero" CPU was 1.6% — process spawns, not the socket wait it was blamed on.
- **Clip by discarding, never clamping.** Clamping an off-screen range folds it onto the boundary — three separate bugs, one mistake.
- **Never add a pass that reads *and* writes every pixel.** Fold work into a write you're already making.
- **Never do per-pixel what can be per-row, or per-band what can be per-frame.** `prepare()`/`render()` split exists for this; violating it cost half the frame rate twice.
- **A diagnostic can be the bug.** A left-in `pinMode` paused the PCNT it was debugging; a 600-sample burst poll cost 24 ms/frame.
- **Verify with the strict client.** A response curl accepts, Node's `fetch` may reject — testing with the forgiving one hid a bug where both halves looked correct.

## Invariants

- `LinkStatus::Online` means **WiFi is associated**, not that Spotify answered. `HostLink` is gated on it and has nothing to do with Spotify — conflating the two meant a device that booted into a rate limit never started its HTTP server at all.
- `KNOB_BANDS=1` must render **byte-identical** to a full frame. This single assertion caught three clipping bugs; check it after any change to a draw path.
- Headless runs are bit-exact: effects take `dt` and a seed, never read a clock. Keep it that way or the pixel tests go flaky.
- Unknown renders as unknown — `volume_pct == -1` and `liked_known == false` must never draw as a confident default.

## Patching source

- **Assert that a string replacement matched, then grep the file, before flashing.** Silent no-op patches meant flashing unchanged code twice while reasoning about the output.
- **A silent no-op patch passes all three builds.** Twice in one session: a bad anchor aborted a script before two of its four edits, and a line-range slice ran one past the `case` onto a blank separator. Both times every target went green — a default argument and the untouched old code masked it. Verify the *edit*, never the build result.

## Mac link

- Helper is `tools/mac_link.py` (stdlib only, runs as a LaunchAgent). Restart: `launchctl kickstart -k gui/$(id -u)/com.knobspotify.maclink`. Log: `/tmp/knob-maclink.log`.
- Its tests are pure and in CI: `cd tools && python3 -m unittest discover -s . -p 'test_*.py'`.
- **Spotify does not answer Apple events while the screen is locked** — the read hangs for the whole subprocess timeout, which outran the beat's own budget and lost the `locked=1` that tells the device to sleep. Read lock state first and skip AppleScript when locked.
- Automation permission is **per-executable**: a grant to your shell does not cover launchd. Denials appear only in the helper's log.
- Lock state: `IOConsoleLocked` and `CGSSessionScreenIsLocked` both flip, and the latter is *absent* while unlocked. No Quartz in system python 3.9; `pmset -g powerstate` fails on Apple Silicon.
- **Never print a `/beat` response** — it echoes the shared token by design, for mutual verification.
- Any device→Mac command must stay a fixed allowlist with typed, range-checked arguments. Never a string the Mac evaluates.
- **The device's mDNS answers in ~5.4s and SUCCEEDS**, so a `.local`-first host list silently taxes every beat with no error and no log. IP first, name as fallback. A timeout generous enough to absorb the resolve is what hid it.
- **TCC grants bind to the binary's ad-hoc signature** — every clang rebuild voids them silently while `AXIsProcessTrusted` still says true. Never build on event posting; draw windows instead (permissionless).
- Latency triage is one knob gesture: merge `/tmp/knob-maclink.log` (timestamped, includes the `[knobhud]` ledger) with serial `knob->mac` stamps and read the hop deltas. The instrumentation is permanent.

## Secrets

- Never read or print `src/config/secrets.h`. `tools/get_refresh_token.py` needs a real TTY (the `!` prefix has none) and preserves fields you skip.
- The playlist browser needs `playlist-read-private`; the boot banner prints granted scopes, which is the only place a missing one shows up.
