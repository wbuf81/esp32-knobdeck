# ESP32-S3 Knob Spotify Player — Design

**Date:** 2026-08-23
**Target hardware:** Waveshare ESP32-S3-Knob-Touch-LCD-1.8
**Ancestor project:** `../m5stackfirmware` (M5Stack Core Basic v2.7 Spotify Desk Controller)

## 1. What this is

A Spotify desk appliance: shows what is playing, controls playback, and renders
album-art-driven reactive visuals on a round 360x360 display driven by a rotary
knob and a capacitive touch surface.

It is a **port of the architecture** and a **redesign of the presentation** of the
M5Stack firmware. The non-visual layers (state model, network task, Spotify Web
API client, config/NVS, fixture source) carry over close to verbatim. Every
rectangular layout and every dirty-rect drawing strategy is discarded, because
the constraints that produced them do not exist on this board.

## 2. Why the presentation is redesigned rather than ported

The M5 firmware's entire rendering strategy is a response to two hard limits,
both stated explicitly in its own source:

- `src/ui/modes/ViewMode.h`: *"Modes must not composite off-screen: a 320x240
  RGB565 buffer is 150KB and this board has no PSRAM. They draw straight to the
  panel, which is why the interface splits in two."*
- `src/ui/Scenes.h`: *"[the spectrum visualiser] replaced ... The bars were the
  problem: that shape promises beat-sync regardless of how it moves, and
  beat-sync is not available - the device never sees the audio, the Core Basic
  has no microphone, and Spotify's /audio-features and /audio-analysis both
  answer 403."*

Neither limit applies here. This board has 8 MB of octal PSRAM and a digital MEMS
microphone. Carrying the old strategy forward would inherit workarounds for
problems that no longer exist.

### Hardware delta

| | M5 Core Basic v2.7 | ESP32-S3 Knob Touch 1.8 |
|---|---|---|
| MCU | ESP32-D0WDQ6-V3, 2x LX6 @240MHz | ESP32-S3R8, 2x LX7 @240MHz |
| Screen | 320x240 rectangular, ILI9342C, 1-bit SPI | 360x360 **round**, ST77916, **QSPI** |
| PSRAM | none | **8 MB octal** |
| Flash | 16 MB | 16 MB |
| Input | 3 tactile buttons | rotary encoder + capacitive touch (CST816) |
| Haptics | none | vibration motor via DRV2605 |
| Audio in | none | **digital MEMS microphone** |
| Audio out | buzzer | PCM5100A I2S DAC + 3.5mm jack |
| Storage | microSD (shares SPI with LCD) | microSD |
| Power | IP5306 PMIC (25% steps, broken charge bit) | battery charger + PH1.25 connector |

The M5's shared-SPI-bus deadlock and its IP5306 boost-latch bug are both
board-specific and do not carry over. Those workarounds should be deleted, not
ported.

### Performance budget — MEASURED

Measured on the physical unit on 2026-08-23 (`src/platform/esp32/Bench.cpp`),
30 iterations per figure, one 360x360 RGB565 frame = 259,200 bytes. **These
replace the estimates this section previously carried, which were optimistic by
roughly 2x on PSRAM.**

| Operation | PSRAM | Internal SRAM |
|---|---|---|
| write u16 | 33.2 MB/s | 90.9 MB/s |
| write u32 | 33.2 MB/s | **181.7 MB/s** |
| memset | 33.2 MB/s | **725.3 MB/s** |
| read u32 | 56.1 MB/s | 151.5 MB/s |
| fade, arithmetic | 34.6 MB/s (14.3 ms/frame) | 43.3 MB/s (11.4 ms) |
| fade, 2x256 LUT | 40.7 MB/s (12.1 ms) | 53.5 MB/s (9.3 ms) |
| fade, skipping black | 61.3 MB/s (8.1 ms) | 94.3 MB/s (5.2 ms) |
| addSat over full frame | 33.4 MB/s (14.8 ms) | 41.3 MB/s (12.0 ms) |

Also measured at boot: PSRAM total 8,388,607 B, largest free block 8,257,524 B;
internal heap free 370,508 B, largest free block 327,668 B.

Three conclusions, each of which changes the design:

1. **PSRAM writes are hard-capped at 33.2 MB/s.** u16, u32 and `memset` all
   measure identically, so this is a bus ceiling rather than instruction count.
   Any full-frame pass over a PSRAM framebuffer costs at least 7.45 ms, which
   allows about two such passes per frame and no more.
2. **Internal SRAM scales with access width; PSRAM does not.** 32-bit stores are
   exactly twice 16-bit stores internally, and `memset` reaches 725 MB/s. Every
   fill loop must therefore write pixel pairs, and clearing an internal buffer
   is effectively free.
3. **The per-pixel operations are CPU-bound, not memory-bound.** `fade` costs
   11.4 ms in internal SRAM against 1.36 ms for a raw write of the same bytes.
   This invalidates the contingency this spec previously carried - moving hot
   passes into internal-SRAM bands "if PSRAM disappoints" would buy only ~25% on
   `fade`, not the order of magnitude implied. The win is in reducing per-pixel
   instruction count and in the *number* of passes, not in where the pixels
   live. A LUT saves 19%; skipping already-black pixels saves 54%.

## 2a. Render architecture (revised after measurement)

The original design - two 360x360 framebuffers in PSRAM, double-buffered, with
a seven-step pipeline over each - does not fit the measured budget. Six
full-frame passes over PSRAM would cost over 45 ms before any drawing happened.

**There is no full framebuffer.** Bands are composited in internal SRAM and DMA'd
straight to the panel, so the 33 MB/s PSRAM write cost is never paid at all.

```
internal SRAM, resident (~107 KB; leaves ~220 KB for WiFi and mbedTLS)
  band A / band B      360x40 RGB565 x2     57.6 KB   ping-pong, DMA out
  bloom small          90x90 RGB888         24.3 KB   persists across frames
  bloom blur scratch   90x90 RGB888         24.3 KB
  fade LUT             2 x 256 x uint16      1.0 KB
PSRAM
  decoded album art, particle arrays, fixtures
```

Per frame: nine bands of 40 rows. Per band, **one fused sweep** - bloom-add,
dither and mask all touch every pixel, so running them as separate passes would
triple the dominant cost:

```
1. fill band with the backdrop gradient        u32 pair writes
2. rasterise the cover quad rows in this band  bilinear from PSRAM art
3. additive particle streaks clipped to band
4. add bloom, bilinear from the resident 90x90 built LAST frame
5. accumulate this band's bright-pass into next frame's 90x90
6. dither + circular mask                      fused into step 4's sweep
7. kick DMA for this band; render the next into the other buffer
```

Two consequences of the measurements, both of which are also aesthetic
improvements rather than compromises:

- **Bloom runs one frame late.** The 90x90 bright-pass is accumulated as bands
  render and applied on the following frame. One frame of bloom latency at 30 fps
  is imperceptible, and it removes the need to hold a whole frame anywhere.
- **Particles are motion streaks, not a persistence buffer.** Each particle is
  drawn as an additive streak from its previous position to its current one.
  A persistence buffer would have needed a full frame in PSRAM at roughly 12 ms
  per frame for the read-fade-write; streaks cost nothing beyond the pixels they
  cover, and crisp motion streaks read better than a uniform frame fade.

Estimated total: 20-25 ms per frame for a perspective-textured cover, ~2,000
streak particles, bloom and dithering - i.e. 30-40 fps, with headroom.

**Implications for the desktop build.** The desktop target keeps a single full
360x360 buffer, since a host has no reason to band. The band abstraction is
therefore a `Surface` (pointer, width, height, y-offset) that effects draw into;
on the device it is a 40-row band, on the desktop the whole frame. Effects are
written against `Surface` and are identical on both.

## 3. Decisions taken

| Decision | Choice |
|---|---|
| View strategy | Redesign round-native; keep the M5 architecture |
| Primary input | Knob = volume (one fixed meaning); touch = transport |
| Framework | PlatformIO + Arduino (ESP32 Arduino core sits on ESP-IDF) |
| Panel driver | Vendored `esp_lcd_st77916` (ESP Component Registry), called from Arduino |
| Drawing | Custom compositor owning native-endian RGB565; LovyanGFX demoted to fonts/JPEG/primitives, never touches the panel |
| Desktop build | Yes — SDL target, same source |
| Build order | Visuals first, Spotify later |
| Audio reactivity | Lean hard on the mic (music plays on desk speakers) |
| First view aesthetic | Album art as the light source |

### Why Arduino rather than ESP-IDF

The ESP32 Arduino core is built on ESP-IDF, so `esp_lcd_panel_*` and the other
IDF APIs are callable directly from Arduino code. This gets the vendor-maintained
QSPI panel driver — removing the LovyanGFX-ST77916-QSPI support risk entirely —
while keeping `WiFiClientSecure`, `HTTPClient`, `Preferences` and `SD`, which is
what allows `core/`, `net/`, `spotify/` and `config/` to port near-verbatim.

### Why not LVGL

LVGL 9 is a retained widget tree with dirty-region rendering. For a particle
renderer that means either thousands of widget objects (catastrophic) or using
LVGL purely as a buffer holder while writing raw pixel code anyway — paying its
overhead for none of its benefit. LVGL remains a reasonable choice for a future
settings screen; it is the wrong shape of tool for the primary goal.

## 4. Module layout

```
src/
  core/       AppState, PlaybackState, MergePolicy, Deadline, CommandQueue,
              Clock, Hash, ProgressClock                      PORT ~as-is
  net/        NetWorker, WifiLink, HttpClient, NetLog          PORT ~as-is
  spotify/    SpotifyAuth, SpotifySource                       PORT ~as-is
  config/     DeviceConfig (NVS + compiled-secrets fallback)   PORT ~as-is
  sources/    FakeSource (deterministic offline fixtures)      PORT ~as-is
  art/        SD cache, JPEG decode                            PORT + extend
              + NEW Palette: 16-entry palette extraction from cover
  gfx/        NEW  Framebuffer, blend ops, Bloom, Dither, CircleMask,
                   Quad3D, TextRender
  fx/         NEW  ParticleSystem, effect modules
  audio/      NEW  MicCapture, Fft, BandEnergy, OnsetDetector, TempoTracker
  input/      NEW  Encoder (PCNT), Touch (CST816), GestureRecognizer, Haptics
  shell/      NEW  RadialShell — progress ring, type, transient overlays
  views/      NEW  round-native views
  platform/
    esp32/    Panel (esp_lcd ST77916 QSPI), I2sMic, I2cBus, Nvs, Sd, Power
    desktop/  SdlPresent, WavMic, FileArt, Harness
```

Approximately 60% of the M5 firmware's non-UI source carries over unmodified.

### Deliberately not ported

- `ui/` in its entirety — every layout constant is rectangular, and `StatusStrip`
  is a 48 px bottom band with no meaning on a circle.
- The SPI-bus-deadlock avoidance in `NowPlayingScreen` (board-specific).
- `Esp32Power.cpp`'s IP5306 boost-latch read-modify-write (board-specific).
- The dirty-rect `enter()`/`tick()` split in `ViewMode` — replaced by full-frame
  compositing.

## 5. Render pipeline

Two 360x360 native-endian RGB565 buffers in PSRAM (259 KB each, 518 KB total),
plus a 90x90 bloom scratch. Double-buffered so the DMA of frame *N* overlaps the
render of frame *N+1*.

```
1. fade persistence buffer     multiply toward black -> trails
2. scene layers                backdrop -> 3D cover quad -> particles (additive)
3. bloom                       bright-pass -> 90x90 downscale -> 2 blur passes
                               -> bilinear upscale-add
4. shell overlay               ring, type, volume overlay. Crisp, bloom-exempt.
5. circular mask               blacken outside r=180
6. ordered dither              hides RGB565 banding on the 18-bit panel
7. present                     byte-swap + async DMA  |  SDL texture upload
```

**Byte order.** The M5 board reference names sprite byte-swapping as *"the single
most confusing trap on the board, and it bit this project twice."* Here the
framebuffer is native-endian throughout and the swap happens only in step 7. All
effect arithmetic operates on native-endian pixels, so the trap is designed out
rather than commented around.

**Fallback if PSRAM bandwidth disappoints.** If M4 measures materially below
~80 MB/s, steps 1-3 move into internal-SRAM horizontal bands (360x40 = 28.8 KB)
with ping-pong DMA. This complicates every effect, so it is a contingency and not
the default. `Framebuffer` therefore exposes a surface abstraction that can
represent either a full frame or a band, even while only full frames are used.

## 6. The modulation bus

The single abstraction that decouples effects from their inputs, making them
testable, emulatable, and independent of whether a microphone exists.

```cpp
struct Modulation {
  float bass, mid, treble;   // 0..1, smoothed band energy
  float loudness;            // 0..1 overall
  bool  onset;               // beat detected on this frame
  float beat_phase;          // 0..1 sawtooth, phase-locked to tracked tempo
  float progress01;          // position through the current track
  float volume01;
  uint32_t track_seed;       // deterministic per track
  bool  live;                // mic is genuinely hearing music
};
```

Assembled once per frame on core 0. Effects read it and nothing else — never the
microphone, never `AppState` directly, never a wall clock. This is what makes the
visual tests deterministic and what lets the desktop build substitute a WAV file
for the microphone with no change to effect code.

When `live` is false (mic below noise floor for a sustained window), the
audio-derived fields are generated procedurally from `progress01` and
`track_seed` and crossfaded in over ~500 ms. The device listens on desk speakers
in the expected setup, so this is a graceful-degradation path rather than the
main one — but it exists so that headphone use does not produce a visualiser
frozen still, which is the failure mode `Scenes.h` warns about.

## 7. Audio chain

Core 0, ~3 ms per frame:

```
I2S mic -> 1024-sample ring @ 16 kHz -> Hann window -> 512-point real FFT
        -> 3 log-spaced bands (bass/mid/treble) -> spectral flux
        -> adaptive-threshold onset detection -> tempo tracker -> beat_phase
```

The desktop build feeds the identical chain from a WAV file, so onset and tempo
behaviour is developed and unit-tested against fixed audio rather than by
clapping at hardware.

## 8. Threading

Unchanged in principle from the M5 design, which is sound:

- **Core 0:** `NetWorker` (WiFi + Spotify) and the audio chain.
- **Core 1:** render loop.
- `AppState` is snapshotted under a mutex; **the lock is never held during I/O.**
- The hardware watchdog covers the render loop only. The net task gets a
  heartbeat counter and is restarted if it genuinely wedges — the M5 project
  learned this the hard way (subscribing the net task to the WDT produced a
  30-second reboot loop).
- Every timer is elapsed-based (`Deadline`) so nothing inverts at the 49.7-day
  `millis()` wrap.

The render loop takes `dt` and a seed as parameters rather than reading a clock,
so headless replay is bit-exact.

## 9. Input

| Gesture | Action |
|---|---|
| Rotate | Volume. Haptic click per detent. Ring becomes a volume ring with a centred percentage, reverting after ~1.2 s |
| Tap centre | Play / pause |
| Swipe left / right | Previous / next track |
| Long press | Like (save to library) |
| Swipe up | Cycle view |

- **Encoder** on the **PCNT** peripheral — hardware quadrature decode, so no
  software debounce jitter and no missed steps during a fast spin.
- **Touch** CST816 over I2C with its interrupt line.
- **Haptics** DRV2605 over I2C, one click per detent, distinct waveform on
  like/skip.
- `input/ButtonLogic.h` ports directly: its tap / long-press / hold-repeat pure
  state machine applies to the touch surface unchanged, along with its host
  tests.
- Volume commands coalesce through `CommandQueue::pushCoalesced` during a spin so
  only the final value is sent — already implemented in the ported code.
- Optimistic UI with settle windows (`AppState::settle_volume` et al.) is
  retained; it is what makes a knob feel instant despite a ~200 ms round trip.

## 10. Showcase view — "Cover Light"

Album art as the light source. Everything is driven by `Modulation`, so the whole
view is one tunable file.

- **Cover quad.** The album art as a perspective-projected textured quad, slow
  orbit and tilt, bilinear-sampled from a PSRAM-resident RGB565 decode of the
  cover.
- **Reflection.** The same quad mirrored below, vertically faded, with a
  bass-modulated ripple.
- **Particles.** ~2,000, coloured from the cover's extracted 16-entry palette,
  spawned in a shell around the quad. Kick onset triggers a radial burst; treble
  drives sparkle and jitter. Additive blending, persistence trails.
- **Bloom.** Intensity tracks `loudness`.
- **Backdrop.** Radial gradient in the cover's dominant hue; radius follows mid
  energy.

Because the palette comes from the artwork, the view art-directs itself and looks
different on every track for free — the same principle as the M5's tint
sampling, taken further.

## 11. Shell (present on every view)

- **Progress ring** at r~172, art-tinted, with a comet head. Direct descendant of
  `StatusStrip`'s tinted progress bar.
- **Title / artist** beneath the cover. Fonts must cover Latin-1 at minimum —
  the M5 project notes that ASCII-only faces render "Bjork" wrong, which is
  unacceptable for a real library.
- **Radial indicators** for liked state, battery, and link status.
- **Toasts** in the lower arc.
- `liked_known` and `volume_pct == -1` must render as *unknown*, never as a
  confident default. Both `PlaybackState` comments call this out and the rule
  carries over.

## 12. Testing

**Host unit tests** (`pio test -e test`) — port the M5's 26 (merge policy,
deadline wrap-safety, button logic, text wrapping) and add:

- gesture recognition from synthetic touch traces
- onset detection and tempo tracking against fixed synthetic audio
- palette extraction determinism
- modulation-bus procedural fallback and crossfade

**Visual tests** — render N frames headless at a fixed seed with fixed fake audio,
then assert pixel properties rather than comparing golden images. This is the M5
project's approach and its stated justification holds: *"Every display bug found
in this project was invisible to logic tests and obvious in a screenshot."*

**Bit-exact replay** is a hard requirement of the render loop design (`dt` and
seed as parameters, no clock reads inside effects), because it is what makes
visual assertions stable.

## 13. Milestones

| | Deliverable | Hardware |
|---|---|---|
| M1 | Renderer core: framebuffer, blend ops, bloom, dither, circle mask, particle system, SDL present, driven by `FakeSource` | no |
| M2 | Audio chain from WAV to modulation bus, with unit tests | no |
| M3 | "Cover Light" fully tuned on desktop | no |
| M4 | Bring-up: confirm pins, panel QSPI, **PSRAM bandwidth benchmark**, touch, encoder, haptics, mic | yes |
| M5 | Port `core/ net/ spotify/ config/ art/` — real Spotify, setup portal | yes |
| M6 | Radial shell, gestures, additional views | yes |

M4 may run in parallel with M1-M3. Its bandwidth measurement must land before M3
locks its per-frame budget.

**Planning scope.** This spec covers more ground than one implementation plan
should carry. It is decomposed as:

- **Plan 1 — M1-M3 (desktop):** renderer core, audio chain, showcase view. Fully
  hardware-free, independently verifiable, and delivers the thing this project is
  actually for. This is the plan to write now.
- **Plan 2 — M4-M5 (hardware + player):** bring-up, pin confirmation, bandwidth
  measurement, then the port of the network and Spotify layers.
- **Plan 3 — M6 (shell + views):** radial shell, gesture wiring, additional
  views.

Each gets its own plan and its own implementation cycle. Plan 2 may begin as soon
as someone wants to touch hardware; only its bandwidth result gates Plan 1's
final tuning.

## 14. Risks and unknowns

- **Pin assignments are unverified.** Waveshare's product page and wiki both
  returned HTTP 403, so the values below came from community ESPHome and Tasmota
  configurations and **must be confirmed on hardware as the first task of M4**:

  | Function | Candidate pins |
  |---|---|
  | Display QSPI | SCK 13, CS 14, D0-D3 15/16/17/18, RST 21, backlight 47 |
  | Touch (CST816, I2C) | SDA 11, SCL 12, INT 9, RST 10 |
  | Encoder | A 8, B 7 |

- **PSRAM bandwidth** — if materially below ~80 MB/s, the hot compositing passes
  move to internal-SRAM bands. Measured in M4.
- **ST77916 QSPI init sequence** — community reports note a required ~120 ms
  delay after sleep-exit and before display-enable, and that 40 MHz works where
  50 MHz does not on ESP32. Needs hardware confirmation.
- **Mic noise floor and AGC** tuning is genuinely fiddly on real hardware and
  cannot be fully settled from a WAV.
- **Second encoder** is wired to the board's secondary ESP32-U4WDH, which this
  project does not program. Treated as absent.
- **Spotify account constraints** carry over unchanged: Development Mode allows 5
  users, the app owner needs active Premium, post-February-2026 apps must use
  `/me/library` rather than `/me/tracks`, and `/audio-features` is 403 at this
  tier — which is precisely why reactivity comes from the microphone instead.

## 15. Deferred

- Audio output through the PCM5100A DAC (the board can play sound; nothing here
  needs it yet).
- LVGL-based settings UI.
- Views beyond the first showcase one (cheap to add once M1-M3 exist).
- Battery-life optimisation and sleep behaviour, beyond porting the M5's
  dim/sleep timers.
