# Teams Mode — Design

**Status:** approved interactively; the Teams local-API spike is the one open gate.

**Scope:** sub-project 4 of the Mac link (the one the original spec deferred
pending a spike). The spike question is now answered in principle — Teams ships
a local hardware-control API — and awaits confirmation against the real client.

## What it is

When a Teams call starts, the knob's screen becomes a meeting controller: two
giant tappable halves, mic left and camera right, each drawn in its live state
from Teams' own truth. Tap a half to toggle it. Call ends, the screen returns to
whatever it was showing. Teams merely being open changes nothing — the user
works all day with Teams running, and a mode that hijacked the music display on
app-launch would be furniture.

Decided with the user directly:

- **Two halves, not four buttons and not gesture-chords.** The panel is a real
  touchscreen (CST816, single-touch, continuous x/y) that the UI has been using
  as a gesture pad; fingertip targets want ~90px+ on a 360px disc, and two
  half-disc buttons are unmissable mid-meeting. The state display IS the button.
- **Call-triggered only** (`isInMeeting`), not app-triggered.
- **Pending, then confirmed.** A tap gives instant pending feedback (dimmed
  half) but the displayed state only changes when Teams echoes the new state
  back. The screen must never claim the mic is muted while it is hot — that is
  the whole reason app-level truth was required and system-mic hacks were
  rejected in the original spec.
- **Unknown renders as unknown.** Link drops mid-call: grey question-mark
  halves, never a confident stale state.

## Why the Teams local API

Teams (new client included) ships a third-party device API — the Stream Deck
mechanism: a WebSocket on `localhost:8124` that pushes meeting state
(`isMuted`, `isCameraOn`, `isInMeeting`, ...) on every change and accepts
`toggle-mute` / `toggle-video` actions. Off by default; enabled per-user in
Teams Settings → Privacy → Third-party app API. First connect triggers an
in-Teams pairing prompt and yields a token, stored beside the knob token.

This kills every fragile alternative dead: no Accessibility keystrokes (the
TCC-grant-dies-on-rebuild trap), no window-title scraping, no guessing Teams
mute from system mic state (provably different things). State is pushed, not
polled, and actions are confirmed by the same channel.

**Spike, still owed against the real machine:** the port is closed until the
user flips the setting. Before the helper's client is finalized: capture the
real pairing flow, the real token-refresh message, and the real meetingUpdate
payload shape. Protocol details in any plan are provisional until then — this
session's core lesson is measure, don't recall.

## Architecture

One new lane on existing rails:

```
Teams ── ws://localhost:8124 ──► helper (mac_link.py)
                                   │  beat fields: teams_in_call, teams_muted,
                                   │               teams_camera (all tri-state)
                                   ▼
                                 knob ── MacLink → Screen::Teams (auto enter/exit)
                                   │
                                   ▲  commands: teams_toggle_mute,
                                   │            teams_toggle_camera
                                 tap on a half
```

- **Helper:** a minimal stdlib WebSocket client (~100 lines; no pip — the
  LaunchAgent stays boring). Runs on a thread; failures degrade to
  "teams fields absent", never to a dead helper. Token persisted at
  `~/.config/knob-spotify/teams_token`.
- **Wire:** three new beat fields, tri-state (-1/0/1) like `out_muted`; absent
  means unknown. Two new response commands on the fixed allowlist — typed,
  no arguments needed, never a string the Mac evaluates.
- **Device:** `MacState` carries the three fields. `Screen::Teams` enters when
  `teams_in_call` becomes 1 and the state is fresh; exits (restoring the prior
  screen) when it leaves 1 or goes stale. Tap routing by half needs the touch
  DOWN position, which `GestureRecognizer` already records and merely does not
  expose — a `tapX()` accessor and a positional-tap route, host-tested like the
  player routing.

## Out of scope, recorded

- Raise-hand, leave-call, blur: the API offers them; the two-half layout does
  not. Add only with a screen design that earns them.
- Teams presence/status outside calls.
- Multi-touch anything: the CST816 is single-touch.

## Testing

The established split: every decision host-tested (tap→half routing, tri-state
parsing, enter/exit/restore rules, pending-vs-confirmed rendering, staleness to
unknown), transport verified live (WS pairing, real payloads, end-to-end tap →
Teams mute flip observed in the client). The helper's WS frame codec gets pure
tests with recorded real frames once the spike captures them.
