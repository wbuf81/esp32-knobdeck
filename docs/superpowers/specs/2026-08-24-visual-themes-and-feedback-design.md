# Visual themes, gesture feedback, and an idle dog

Status: approved 2026-08-24. Split into three sub-projects; each gets its own plan.

## Why

Three requests that turn out to share one seam.

1. **Every gesture should acknowledge itself.** A long-press test on 2026-08-24
   produced no visible response twice: once because nothing was playing so the
   command was correctly dropped, and once because the toggle went from saved to
   not-saved and the heart quietly disappeared. Both read as "the device ignored
   me". Silence is the bug.
2. **More than one visualisation**, chosen by hand or shuffled per track.
3. **No particle field when nothing is playing** — show a sleeping dog instead.

(2) and (3) are the same mechanism: something decides which visual is on screen.
The dog is chosen by playback state, a theme is chosen by the user or by
shuffle. One abstraction, two callers.

## Constraints, measured

These are the numbers the design has to live inside. All from this device.

| Fact | Value | Consequence |
|---|---|---|
| Player frame | ~19 fps | No new full-frame read-modify-write pass. |
| Backdrop pass | 11.3 ms | A theme backdrop must REPLACE it, not add to it. |
| Cover pass | 12.5 ms | Unchanged; the cover stays in every theme. |
| Internal SRAM | 56% used | ~144 KB free. |
| `fx::Particles` | ~38 KB | Themes cannot each own a particle pool. |
| Flash | 38.6% used | ~1.9 MB free; the dog's ~90 KB is affordable. |
| Knob press | **unreadable** | `BOARD_REFERENCE.md:61`. Touch only. |

The particle-pool number is the one that shapes the architecture. Six themes
each owning a `Particles` would be 228 KB and would not fit.

## Sub-project 1: the skeleton

### Gesture feedback

A new `shell::GestureFlash`: a glyph drawn centre-disc, held briefly and faded
out. `prepare()`/`render()` split like everything else in `shell/`.

Glyphs are **plotted, not typeset** — the same reasoning as `ConfirmRing`. The
fonts stop at 0xFF, so play/pause/heart/arrows are not available as characters.
Each is a handful of filled rects or triangles.

| Gesture | Flash |
|---|---|
| Tap → playing | play triangle |
| Tap → paused | two pause bars |
| Swipe left / right | skip arrows |
| Long-press → saved | filled heart |
| Long-press → unsaved | outline heart |
| Long-press, nothing playing | struck-through heart + "nothing playing" |
| Up / down between screens | chevron |

The last row is the bug this fixes: a refused command must say it was refused.

### The visual seam

```
class Visual {                      // one instance live at a time
  virtual void update(const Modulation&, float dt, Rng&);
  virtual void renderBand(Surface&);
  virtual void endFrame();
  virtual uint16_t tint() const;    // the shell's ring colour
};
```

Two implementations in this sub-project:

- **`CoverLight`** — what exists now, gaining a swappable strategy (below).
- **`DaisyIdle`** — the sleeping dog. No particles, no bloom, no cover.

### Themes inside CoverLight

A theme is NOT a `Visual`. It is a small strategy object that `CoverLight` owns
one pointer to, because `CoverLight` owns the expensive parts — the particle
pool, the gradient table, the bloom accumulator, the cover quad — and those are
shared across every theme.

```
struct Theme {
  virtual void configure(fx::SpawnParams*);        // once on selection
  virtual void emit(fx::Particles*, const Modulation&, float dt, Rng&);
  virtual void backdrop(uint16_t* grad, int n, const Modulation&);  // optional
  virtual void drawBand(Surface&) {}               // optional, for Tetris etc.
};
```

Six themes at roughly the RAM cost of one.

### Selection

`Screen::Themes`, reached by **swipe up from Playlists** — Player → Playlists →
Themes. Down goes home from anywhere, so the rule becomes "up = further out,
down = home". No existing gesture changes meaning.

Rendered with the existing `ListView`, whose `current` parameter already tints
the active row. Row 0 is "Shuffle"; the rest are themes. Tap locks one in; tap
Shuffle to pick a new theme on every track change.

The choice persists across reboots via the existing `DeviceConfig`.

### The dog

`DaisyAssets.h` is copied from the ancestor project unchanged — it is generated
from the same GIFs by `tools/daisy_convert.py`, so regenerating would produce
identical bytes and needs Pillow, which is not installed here. Sleep is 3 frames
at 330 ms, drowsy 2 at 400 ms, yawn 4 at 450 ms; 55x41 cells, 8-bit indexed,
17-colour RGB565 palettes, `const` so it lands in flash.

The ancestor's `DaisySprite` draws with `fillRect` against a panel. It is ported
to this project's banded `Surface`: for each band, walk only the cell rows that
intersect it. Index 0 is transparent.

Shown when playback reports no track. Idles on `sleep`, occasionally `yawn`.

### Testing

- `GestureFlash`: each glyph draws something; the flash expires; banded render
  is byte-identical to a full frame.
- `DaisyIdle`: banded render byte-identical to a full frame; transparent cells
  untouched; the frame advances on the animation's own clock.
- Theme selection: shuffle picks a different theme across track changes; a
  locked theme survives a track change; the choice round-trips through config.
- The `KNOB_BANDS=1` assertion holds for every new draw path.

## Sub-project 2: the cheap themes

`Heartbeat`, `Rain`, `Tetris`. All are emission strategies or small block draws,
none needs the backdrop budget.

Tetris: seven tetrominoes x four rotations is a lookup table; pieces fall and
rotate and are drawn as filled cells, so it costs what the blocks cover.

## Sub-project 3: the expensive themes

`Outrun` and `Matrix`. Both replace the radial backdrop and must come in at or
under its 11.3 ms. Neither ships until measured against that.

## Open

- The unexplained 6-second queue fetch. Measured at 747 ms; size, memory and
  stale-connection retry are ruled out. Command submit-to-execute latency is now
  instrumented; it has not reproduced.
