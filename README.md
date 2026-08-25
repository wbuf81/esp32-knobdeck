# Knob Spotify Player

A Spotify appliance for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** — a 360×360
round touchscreen you turn. Album art floats in 3D over a beat-reactive particle
field, a ring around the bezel tracks the track, the knob is the volume, and
swiping up gets you your playlists.

Descended from [a M5Stack Spotify controller](../m5stackfirmware) — the state
model, network task and Spotify client came across nearly unchanged. Everything
you can see was rebuilt, because the two constraints that shaped the original's
rendering (no PSRAM, no microphone) do not exist on this board.

Also builds as a desktop app, rendering the identical source through SDL, which
is where the visuals are actually developed.

## What it does

- **Cover Light**, the one view so far: the album cover as a perspective-projected
  quad with a rippling reflection, ~900 additive particle streaks coloured from
  the artwork, expanding shockwave rings on the beat, and bloom over all of it.
- **Beat reactivity** from the onboard microphone — real FFT, spectral-flux onset
  detection and tempo tracking — with a procedural fallback that crossfades in
  when the room goes quiet, so it never freezes.
- **A playlist browser**: swipe up, turn the knob to scroll a centre-weighted
  wheel, tap to open a playlist, tap a track to play it *in that playlist's
  context*.
- **Live album artwork**, downloaded into PSRAM and decoded by the chip's ROM
  JPEG decoder. **No SD card required.**
- Title, artist and timecodes, laid out against the *chord* of the disc.

## Controls

| Gesture | Player | Browser |
|---|---|---|
| Turn knob | volume, with a haptic tick per detent | scroll the wheel |
| Tap centre | play / pause | open playlist / play track |
| Swipe left / right | previous / next track | — |
| Swipe up | open the playlist browser | — |
| Swipe down | — | back a level |
| Long press | save to Liked Songs | — |

The knob's *press* is not wired to the ESP32-S3 — see
[docs/BOARD_REFERENCE.md](docs/BOARD_REFERENCE.md) §2 — so selection is a tap.

## Quick start

```sh
brew install platformio sdl2 pkg-config     # one time, macOS
export HOMEBREW_PREFIX=/opt/homebrew        # Apple Silicon

# Spotify credentials. Run in a real terminal: it prompts for the sensitive
# values itself and writes src/config/secrets.h mode 0600.
python3 tools/get_refresh_token.py

pio run -e esp32 -t upload && pio device monitor
```

The boot banner reports reset reason, crash streak, PSRAM total **and largest
free block**, and the Spotify scopes actually granted. It is the first thing to
read when anything misbehaves — a missing scope shows up there and nowhere else.

### Spotify app setup

A free developer app at <https://developer.spotify.com/dashboard>. Notes that
save an afternoon:

- **Development Mode allows 5 users.** Add each listener's email under User
  Management.
- The **app owner needs active Premium**, or playback control returns 403.
- The token needs `playlist-read-private` for the browser. `get_refresh_token.py`
  requests it; a token issued before the browser existed will not have it, and
  `/me/playlists` answers 403 with no explanation.
- Post-February-2026 apps must use `/me/library` rather than `/me/tracks`. This
  firmware already does.

## The desktop build

Not a mockup — the same source, rendering at the real 360×360.

```sh
pio run -e native && ./.pio/build/native/program
```

| Variable | Effect |
|---|---|
| `KNOB_HEADLESS=1` | no window; render, dump, exit |
| `KNOB_EXIT_MS=<ms>` | quit after this much *simulated* time |
| `KNOB_DUMP=<path>` | write the framebuffer as a 24-bit BMP |
| `KNOB_BANDS=1` | composite in 20-row bands, exactly as the device does |
| `KNOB_WAV=<path>` | drive the analyser from a WAV instead of the fallback |
| `KNOB_SCREEN=list` | preview the playlist browser |
| `KNOB_POS=<f>` | scroll position within it, fractional |
| `KNOB_SEED=<n>` | track seed, which sets hue and tempo |
| `KNOB_NOCOVER=1` | the no-artwork path |
| `KNOB_PARTICLES=0` | disable the particle layer |
| `KNOB_BLOOM=<0-255>` | bloom strength override |

Time is simulated and fixed-step, so a headless run is bit-identical every time.
That is what the pixel assertions rest on.

### The invariant worth knowing about

`KNOB_BANDS=1` renders the way the device does — 18 bands composited
separately — and the result must be **byte-identical** to a single full frame.
That single assertion caught three separate clipping bugs that were invisible
otherwise, and it is what makes the desktop a trustworthy stand-in for the panel.

```sh
pio test -e test        # 104 host unit tests
```

## Architecture

```
src/
  core/       AppState, PlaybackState, merge policy, wrap-safe Deadline,
              deterministic Rng, clamped FrameClock, locked single-write logger
  net/        NetWorker (FreeRTOS task, core 0), WifiLink, HTTP
  spotify/    auth refresh, polling, commands, playlist/track listings
  art/        PSRAM cover cache, ROM JPEG decode, palette extraction
  audio/      mic capture, 512-point FFT, band energy, onset, tempo,
              the Modulation bus and its procedural fallback
  gfx/        Surface, blend ops, band bloom, Quad3D, bitmap text
  fx/         particle field
  views/      Cover Light
  shell/      radial rings, now-playing text, the wheel list
  input/      PCNT encoder, CST816 touch, gestures, DRV2605 haptics
  platform/   esp32/ and desktop/ behind the same interfaces
```

Three ideas do most of the work:

**No framebuffer.** PSRAM writes cap at 33 MB/s on this board no matter how you
do them, so any full-frame pass over one costs ≥7.45 ms. Instead 20-row bands are
composited in internal SRAM — 181 MB/s with 32-bit stores — and DMA'd straight to
the panel. That cost is never paid at all.

**Effects read one struct.** Every animation input comes from a `Modulation`
struct: three band energies, loudness, an onset flag, a phase-locked beat, track
progress, and a per-track seed. Effects never touch the microphone, playback
state, or a clock. That is what makes the whole renderer deterministic enough to
assert on frame by frame — and what lets a mic that hears nothing degrade into
something that still looks deliberate.

**Radially symmetric effects are free.** The backdrop is a function of radius
alone, so it is one table lookup per pixel however much is happening in it —
which is why the beat shockwaves are rings, and why a train of them costs
nothing. Banding in a radial gradient also runs along iso-radius contours, so
dithering the table *index* rather than the colour breaks it up for a single add.

## Performance

~20 fps at 360×360 with everything running, measured on hardware:

| Pass | ms/frame |
|---|---|
| backdrop + glow + dither | 11.4 |
| cover quad + reflection | 12.2 |
| particles | 7.9 |
| bloom accumulate | 5.7 |
| bloom blur (90×90) | 4.5 |
| rings | 2.3 |
| byte-swap + DMA | 3.2 |
| text | 0.9 |

Getting there took six rounds of measurement, and **every guess made before
measuring was wrong at least once**. The details, and the numbers that constrain
any design on this board, are in
[docs/BOARD_REFERENCE.md](docs/BOARD_REFERENCE.md) — written to be copied into
another project as-is.

## Not done yet

- Only one view. The ancestor had eight.
- No setup portal, so credentials come from a compiled `secrets.h`.
- Microphone reactivity is built and tested but not yet wired to the I²S pins.
- No screen dim/sleep, battery reporting, or watchdog.
- Text is Latin-1; CJK renders a fallback glyph.

## License

MIT.

## The Mac helper (optional)

`HostLink` on the device has always listened for a Mac to report itself; this is
the sender. It carries host state up — lock state, computer name, system volume,
and Spotify's own now-playing read locally via AppleScript — and carries a small
allowlisted command back down, currently just "set the system output volume".

It is entirely optional. Without it the knob is exactly what it was: a Spotify
player. With it, the knob can turn your Mac's volume, and it stops asking
Spotify's API what is playing every two seconds — which is what exhausted the
API quota during development.

**Set it up:**

```sh
# 1. A shared secret, in two places. Never commit either.
python3 -c "import secrets; print(secrets.token_urlsafe(24))" > ~/.config/knob-spotify/token
chmod 600 ~/.config/knob-spotify/token
#    Then add the SAME value to src/config/secrets.h as:
#      #define MAC_LINK_TOKEN "..."
#    and reflash. An empty token disables the command channel entirely.

# 2. Run it in the foreground first. A background process is the wrong place to
#    discover a typo.
KNOB_HOST=<device-ip> /usr/bin/python3 tools/mac_link.py
#    Expected: "starting, host=..." then silence. Silence is correct - a held
#    request that returns no command produces no output. Turn the knob with
#    nothing playing and you should see "output volume -> N".

# 3. Install it at login.
sed -e "s|REPLACE_WITH_ABSOLUTE_PATH|$PWD|" \
    -e "s|REPLACE_WITH_DEVICE_HOST|<device-ip>|" \
    tools/com.knobspotify.maclink.plist \
    > ~/Library/LaunchAgents/com.knobspotify.maclink.plist
launchctl load ~/Library/LaunchAgents/com.knobspotify.maclink.plist
tail -f /tmp/knob-maclink.log
```

**Remove it:**

```sh
launchctl unload ~/Library/LaunchAgents/com.knobspotify.maclink.plist
rm ~/Library/LaunchAgents/com.knobspotify.maclink.plist
```

Killing the helper loses knob-to-Mac control and nothing else. Your volume, your
mic and your music are untouched — the helper only ever acts when the device
asks it to, and it asks for one thing.

Measured cost while running: **0.4% CPU, ~25 MB**. It spends almost all its life
asleep on a socket read the device holds open, which is cheaper than a fast
heartbeat would be. The first version measured 1.6% because it spawned four or
five processes a second building the body; the readers are now cached and
combined.

macOS will likely prompt once for Automation permission so the helper can talk
to Spotify. That is expected.
