# Mac Link — Design

**Status:** approved design, ready for an implementation plan.

**Scope:** sub-project 1 of 4, plus the slice of sub-project 2 needed to prove the
channel end to end.

## Why

`HostLink` is half a subsystem. The device side has existed since early on: an
HTTP listener on port 80 with mDNS, waiting for a Mac to report whether it is
awake or locked. The Mac side was never written, and the boot log has said
`host: awake (heartbeat never seen)` every single boot.

Two features now want that missing half, and a third and fourth are coming:

- **Which Mac to wake.** `22ea6b4` added a tap on the idle screen that asks
  Spotify to hand playback back to a deregistered device. The live probe found
  two devices of type `Computer` on this account — `SERIAL-1234ABCD-M` and
  `Wes's MacBook Pro` — and when the idle screen is showing, neither is
  active, so `pickDevice` falls through to list order. Roughly half the time it
  would resume music on the wrong machine.
- **The knob controlling system volume**, not Spotify's.
- Later: system media keys, and a meeting mode showing mic and camera state.

Building the channel once, deliberately, is cheaper than bolting a private
mechanism onto each. This spec covers the channel and exactly enough of the
audio work to prove it.

## Explicitly not in this spec

- **Meeting mode**, and mic/camera state. There is a real unresolved question
  there: system mic mute and Teams mute are different things. Muting macOS input
  volume leaves Teams showing you as unmuted while it hears silence, so a mic
  indicator driven off system input would confidently display the wrong thing.
  Reading app-level state generally needs Accessibility permission and is
  fragile across app updates, and camera-in-use detection is similarly awkward.
  That needs a spike against the actual macOS version, not a design written from
  memory.
- **System media keys.** Small, and it follows trivially once the command
  allowlist exists.
- Any command that runs a script, a Shortcut, or a named app. See Security.

## Architecture

The Mac pushes; the device listens. That direction is inherited and it is not
arbitrary — `HostLink`'s header records the reason: the alternative is an
unauthenticated listening port on a laptop, where this way the port lives on a
dedicated appliance that does one thing. Commands travel back as the *response*
to the Mac's own request, so the Mac never listens for anything.

```
  Mac helper (LaunchAgent)                 knob (HTTP server, port 80)
  ─────────────────────────                ───────────────────────────
  POST /beat  ──── state up ────────────▶  parse, validate token
     (held open, up to 1s)                 hold if no command pending
  ◀─────────── commands down ────────────  respond on command or timeout
  apply via osascript
  reconnect immediately
```

### Transport: a small `key=value` body, both ways

Not query parameters. The field that fixes the two-Macs problem is the computer
name, which here is `Wes's MacBook Pro` — spaces and an apostrophe. Query
parameters would mean percent-encoding it and writing a decoder on the device.

Not JSON either, on the server path. `HostLinkEsp32.cpp` hand-parses a raw
`WiFiServer` rather than using `WebServer`, and its comment gives the reason:
internal SRAM is the binding constraint on this board. A `key=value\n` body
needs a bounded read and a trivial split, and adds no parser.

**Request** (`POST /beat`):

```
v=1
tok=<shared secret>
locked=0
host=Wes's MacBook Pro
sp_device=Wes's MacBook Pro
out_vol=63
```

`host` is `scutil --get ComputerName`. `sp_device` is the name Spotify Connect
advertises for this machine, which on macOS is the computer name — sent as its
own field anyway, so the two can diverge without a firmware change.

**Response**, when a command is pending:

```
v=1
tok=<shared secret>
set_output_volume=42
```

and when nothing is pending, after the hold expires:

```
v=1
tok=<shared secret>
```

### Latency: the device holds the request

The helper's request is held open by the device for up to `HOLD_MS` (1000) and
answered the instant a command appears. Idle cost is one request per second with
the helper's process **asleep on a socket read** — zero CPU, no timer wakeups.

This was chosen over a fixed 500 ms heartbeat specifically because it is
*lighter* on the Mac, not merely faster: a blocked read costs nothing, whereas
waking a Python interpreter twice a second forever is what stops a laptop
reaching deep idle. The complexity lands entirely in firmware.

Consequence for `HostLink`: it must keep a `WiFiClient` across `poll()` calls
instead of answering within one. At 126 fps `poll()` runs often enough that the
response goes out within a frame of the command appearing.

**Bounded, because this runs in the render loop.** The existing 40 ms read
deadline and 96-byte line cap exist because an unbounded read would let one slow
or hostile client stall rendering. The held client keeps that property: reading
is still bounded per frame, the hold is a deadline check rather than a wait, and
exactly one client is held at a time. A second client arriving while one is held
is answered immediately with no command.

### Security

Today anything on the LAN can `GET /locked` and darken the screen. Once the
response carries commands the Mac executes, the blast radius changes, so:

- **A shared token, verified in both directions.** The device rejects a request
  whose `tok` does not match, and the helper ignores a response whose `tok` does
  not match. The second half is not theatre: without it, anything that can
  answer on the device's address — ARP or mDNS spoofing — could drive the Mac.
- **The token resolves NVS-first with compiled secrets as the per-field
  fallback**, exactly as credentials already do in `DeviceConfig`. New field:
  `mac_token`, with `MAC_LINK_TOKEN` in `secrets.h`.
- **An empty token disables the command channel entirely** and leaves
  `/awake` and `/locked` working. An unconfigured device must not ship a
  command channel with a blank password.
- **Commands are a fixed allowlist with typed, range-checked arguments.** Never
  a free-form string the Mac evaluates. This sub-project defines exactly one:
  `set_output_volume`, an integer clamped to 0..100. The helper re-validates the
  range rather than trusting the device, because the device is the less trusted
  end of a channel that ends in `osascript`.
- This constraint is what lets "more Mac stuff" grow safely. Adding a command
  means adding an allowlist entry with a typed argument, not widening a hole.

### Forward and backward compatibility

The helper runs at login; firmware changes when it is flashed. They will not
move in lockstep, so both ends must degrade rather than break:

- `v=1` in both directions. A future `v=2` is additive.
- **Unknown keys are ignored on both sides.** New firmware sending a field an
  old helper does not know is harmless, and vice versa.
- `GET /awake` and `GET /locked` keep working, unchanged and unauthenticated,
  so a partially-updated setup still reports sleep state.
- A helper that gets a 401 logs it once and keeps sending sleep state, rather
  than dying.

## Components

### `src/net/MacLink.{h,cpp}` — new, host-testable

The state machine and the parse, with no Arduino in sight, so it gets tests the
way `Backlight`, `HostLink` and `CrashPolicy` do.

```cpp
namespace net {

struct MacState {
  bool  locked = false;
  bool  valid = false;         // a good beat has been seen
  char  host[64] = {};
  char  sp_device[64] = {};
  int   out_vol = -1;          // -1 = unknown, and renders as unknown
};

class MacLink {
 public:
  static constexpr uint32_t HOLD_MS = 1000;
  static constexpr int PROTOCOL_V = 1;

  void setToken(const char *tok);
  bool commandChannelEnabled() const;   // false when the token is empty

  // Parses one request body. Returns false on a bad version or a bad token,
  // and then nothing is applied - a rejected beat must not move any state.
  bool applyBeat(const char *body, uint32_t now_ms);

  // The device wants the Mac's output volume set. Replaces any pending value:
  // only the final position of a spin matters, the same coalescing the Spotify
  // volume command already does.
  void requestOutputVolume(int pct);
  bool hasPending() const;

  // Builds the response body. Clears the pending command once written.
  int buildResponse(char *out, size_t cap);

  const MacState &state() const;
  bool stale(uint32_t now_ms) const;    // wrap-safe, like HostLink::hostAsleep
};

}  // namespace net
```

### `src/net/HostLink.{h,cpp}` and `HostLinkEsp32.cpp` — extended

Gains `POST /beat`, a bounded body read using `Content-Length`, and the held
client. Delegates all parsing and all policy to `MacLink`; keeps the existing
routes and the existing fail-open behaviour.

### `src/spotify/DevicePick.h` — extended

`pickDevice` takes an optional preferred name. Preference order becomes:

1. an unrestricted device whose name matches `sp_device` from the Mac link
2. an already-active `Computer`
3. any `Computer`
4. the first unrestricted device

Rule 1 is the fix for two Macs: the knob prefers the machine it is sitting next
to, because that machine told it its own name.

### `tools/mac_link.py` — new

The daemon. Standard library only — no pip install for something that runs at
login. Reads `~/.config/knob-spotify/token`, or `KNOB_TOKEN` from the
environment.

- Resolves the device by mDNS name with an IP fallback.
- `POST /beat` in a loop, blocking on the read.
- Applies `set_output_volume` with
  `osascript -e "set volume output volume <n>"`, after re-clamping to 0..100.
- Reads state to send: `scutil --get ComputerName`, lock state, and current
  output volume from `osascript -e "output volume of (get volume settings)"`.
- **Backs off on failure** — 1 s, doubling to 30 s — so an unreachable knob
  never becomes a spinning process on the laptop.
- Logs to stderr; the LaunchAgent captures it to a file.

### `tools/com.knobspotify.maclink.plist` — new

A LaunchAgent with `RunAtLoad` and `KeepAlive`. Installation and removal are one
`launchctl` command each, documented in the plan. Nothing about the Mac's
behaviour depends on it running: kill it and the knob loses Mac control, not the
Mac.

## Data flow

**Knob turn, system-volume mode:**

```
encoder detent
  → MacLink::requestOutputVolume(pct)      coalesced, replaces any pending
  → held helper request answered this frame
  → helper: osascript set volume output volume
  → next beat reports out_vol, UI resyncs
```

**Wake the right Mac:**

```
beat carries sp_device="Wes's MacBook Pro"
  → stored in MacState
  → tap on the idle screen → WakeDevice
  → pickDevice(devices, n, preferred="Wes's MacBook Pro")
  → transfer targets that machine
```

## Error handling

| Case | Behaviour |
|---|---|
| No helper ever seen | Exactly as today: fails open, nothing darkens, no Mac features offered |
| Helper stops | `stale()` after `TIMEOUT_MS`; Mac-dependent UI reverts to unknown, never to a stale value |
| Bad token | Request rejected, no state applied, logged once per minute rather than per beat |
| Bad version | Same as bad token |
| Body over cap | Rejected; the read stays bounded |
| Two helpers | One held at a time; the second gets an immediate no-command response |
| Knob spun fast | Coalesced to the final value, like Spotify volume |
| `osascript` fails | Helper logs and carries on; the device is not told, because it cannot do anything useful with the news |

## Testing

Host tests, on `MacLink` and `DevicePick`:

- A good beat applies; a bad token applies **nothing** (not partial state)
- A bad version is rejected
- Unknown keys are ignored, and do not prevent known keys from applying
- An apostrophe and spaces in `host` survive the round trip
- A missing `out_vol` reads as -1, not 0 — unknown renders as unknown
- An empty token disables the command channel
- `requestOutputVolume` coalesces; out-of-range values are clamped
- `buildResponse` clears the pending command, so it is delivered once
- `stale()` is correct across the `millis` wrap
- `pickDevice` prefers the named device over an active `Computer`, and still
  falls back correctly when the name matches nothing

Not host-testable, verified by flashing and by reading the helper's log: the
held-client mechanics, the real `osascript` calls, and the LaunchAgent.

**A number to report rather than promise:** the helper's actual CPU while
running, measured, because "it will not affect your Mac" is a claim and not
evidence.


## Revision: the Mac can answer most of this locally

Added after verifying Spotify's AppleScript interface against the actual machine,
prompted by the right question — "can it just get everything from my Mac?"

**Measured on this Mac, read-only except one no-op write:**

| Property | Value read |
|---|---|
| `player state` | `playing` |
| `name/artist/album of current track` | `hazy concentration` / `KAESUL` / `Schedule I (Original Soundtrack)` |
| `player position` / `duration` | 113.2 s / 235690 ms |
| `spotify url of current track` | `spotify:track:366l6ir20fMXe2KhBxCbb0` |
| `artwork url of current track` | `i.scdn.co/image/ab67616d0000b273...` (640px variant) |
| `sound volume` | 100, and `set sound volume` verified working |
| `shuffling` / `repeating` | `true` / `false` |
| `output volume of (get volume settings)` | 13 |

That is the entire hot path, locally, with no quota. The `GET /me/player` poll that
exhausted the quota today — 1800 requests an hour at `POLL_PLAYING_MS=2000` — is
exactly the call this replaces for free. Artwork was never an API call; it is a
plain CDN fetch either way.

**What AppleScript cannot give, so the Web API does not go away:**

- Playlist enumeration. It can *play* a URI but not list what you have.
- The queue / UP NEXT.
- Liked/saved state — the heart would return to unknown, which is the correct
  rendering for something we do not know.
- Device transfer, so the wake feature still needs the API.
- Anything playing off this Mac. Phone-only or speaker-only playback is invisible
  to AppleScript.

Every remaining API need is **user-initiated and occasional** rather than a
polling loop, which is the shape that does not exhaust a quota.

### Consequence for this spec

The channel is unchanged — Tasks 1 through 3 of the plan stand exactly as
written. What rides on it grows, in three deliberate steps rather than one:

1. **Carry the data.** `MacState` gains playback fields and the helper populates
   them. Additive, testable, and changes no behaviour.
2. **Stretch the poll while the Mac link is fresh and agrees.** A cheap quota win
   that does not touch who owns the truth: if a fresh beat reports the same track
   playing, the device can poll Spotify far less often because the local data
   covers the gap.
3. **Invert the authority** — the Mac becomes the primary source of playback
   truth, the API the fallback. This is the big one and its own sub-project,
   because it needs an answer to a question this spec does not have one for:
   what happens when the two disagree. The Mac saying "playing" while the API
   reports a different active device is a real state, not an error, and picking
   the wrong winner would make the screen lie about where the music is.

Step 3 is explicitly NOT in this spec. Steps 1 and 2 are folded into the plan.

### Automation permission

The queries above succeeded from a shell, so this Mac already permits it there.
The LaunchAgent is a different executable and will likely prompt once on first
run. Expected, not a fault — but worth knowing before it appears.

## Open question, deferred

Whether the knob should *display* Mac state (locked, volume) anywhere in this
sub-project, or stay silent until meeting mode gives it a home. Leaning silent:
a status element nobody asked for is furniture, and this project already learned
that lesson with the toast that rendered nowhere.
