#!/usr/bin/env python3
"""The Mac half of the knob's HostLink.

HostLink's device side has existed since early on; this is the sender that was
never written, which is why every boot log said "heartbeat never seen".

Posts host state to the knob and applies any command that comes back. The
request is HELD OPEN by the device for up to a second, so this process spends
essentially all its life asleep on a socket read - zero CPU and no timer
wakeups, which is lighter on the laptop than a fast heartbeat would be.

Standard library only: this runs at login and must not need a pip install.
Everything it reads is verified available on this machine rather than assumed -
see the notes on each reader.

  KNOB_HOST   override the device address (default knobspotify.local)
  KNOB_TOKEN  the shared secret; otherwise read from
              ~/.config/knob-spotify/token
"""

import os
import plistlib
import subprocess
import sys
import time
import urllib.error
import urllib.request

PROTOCOL_V = 1
# A comma-separated list, tried in order, with the one that worked remembered.
#
# mDNS is the right primary: it survives a DHCP lease moving, which a pinned IP
# does not. But it is not always instantly available - the device starts its
# responder lazily once WiFi is up, so for the first few seconds after a flash
# the name does not resolve and macOS caches that miss. An IP fallback covers
# exactly that window without needing a static lease on the router.
HOSTS = [h.strip() for h in
         os.environ.get("KNOB_HOST", "knobspotify.local").split(",")
         if h.strip()]
# Longer than the device's HOLD_MS so a held request is answered rather than
# timed out here - and longer than a COLD mDNS resolve, which measured 6.1s on
# this network against an earlier 4.0s budget. That mismatch made the very first
# request after a cache miss fail every time, which then rotated the host and
# looked like the device being unreachable.
TIMEOUT_S = 10.0
BACKOFF_START_S = 1.0
BACKOFF_MAX_S = 30.0
# Anything the device sends that is not in HANDLERS is ignored, so a newer
# firmware talking to an older helper degrades instead of breaking.


def log(msg):
    # Milliseconds since midnight - enough to measure hops, cheap to read.
    t = time.time()
    ms = int(t * 1000) % 86400000
    print(f"[maclink {ms//3600000:02d}:{ms//60000%60:02d}:{ms//1000%60:02d}"
          f".{ms%1000:03d}] {msg}", file=sys.stderr, flush=True)


def read_token():
    tok = os.environ.get("KNOB_TOKEN", "")
    if tok:
        return tok.strip()
    path = os.path.expanduser("~/.config/knob-spotify/token")
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def run(args, stdin_text=None):
    """Run a command and return stdout, or None. Never raises."""
    try:
        # 4s: comfortably inside the 10s request budget, so a slow reader makes
        # a beat LATE rather than LOST - which is how the locked-screen bug hid,
        # when build_body outran the POST it was for.
        #
        # Not 2s. Observed in the wild while the machine was being used
        # remotely: Spotify does not answer Apple events promptly in that state
        # either, and 2s turned a recoverable slow read into a timeout on every
        # single beat. Remote access is a third case beyond locked and unlocked,
        # and it does not always report as locked.
        out = subprocess.run(
            args, capture_output=True, text=True, timeout=4, input=stdin_text
        )
        if out.returncode != 0:
            log(f"{args[0]} failed: {out.stderr.strip()[:120]}")
            return None
        return out.stdout
    except (OSError, subprocess.SubprocessError) as e:
        log(f"{args[0]} error: {e}")
        return None


def osa(script):
    out = run(["osascript", "-e", script])
    return out.strip() if out is not None else None


# Cheap TTL caches, because the first measured version cost 1.6% CPU - not from
# waiting on the socket, which is free, but from four or five PROCESS SPAWNS a
# second building the body. The socket was never the expensive part.
_cache = {}


def cached(key, ttl_s, producer):
    now = time.monotonic()
    hit = _cache.get(key)
    if hit is not None and now - hit[0] < ttl_s:
        return hit[1]
    val = producer()
    _cache[key] = (now, val)
    return val


def computer_name():
    # Effectively constant. Re-read once a minute so a rename is still noticed.
    def read():
        out = run(["scutil", "--get", "ComputerName"])
        return out.strip() if out else ""

    return cached("host", 60.0, read)


def audio_and_playback():
    """System volume, mute state, AND Spotify's now-playing, in ONE osascript.

    Returns (out_vol, muted, playback_dict). -1 / None / {} when unreadable -
    the device renders unknown as unknown, and a confident zero would draw an
    empty slider for a value nobody measured.

    The Spotify read is wrapped in a try INSIDE the AppleScript. Spotify running
    with no current track - stopped state, just launched - errors with -1728
    "Can't get current track", and before the try that error killed the WHOLE
    read: the volume went with it, the log gained two lines per beat, and a
    second osascript was spawned to recover what the first had already known.
    """
    script = (
        "set vs to (get volume settings)\n"
        "set ov to output volume of vs\n"
        "set om to output muted of vs\n"
        'if application "Spotify" is not running then '
        'return (ov as string) & "\\n" & (om as string) & "\\n" & "NORUN"\n'
        'tell application "Spotify"\n'
        "  try\n"
        "    set s to player state as string\n"
        "    set t to name of current track\n"
        "    set a to artist of current track\n"
        "    set u to spotify url of current track\n"
        "    set p to player position\n"
        "    set d to duration of current track\n"
        '    return (ov as string) & "\\n" & (om as string) & "\\n" & s & '
        '"\\n" & t & "\\n" & a & "\\n" & u & "\\n" & (p as string) & '
        '"\\n" & (d as string)\n'
        "  on error\n"
        '    return (ov as string) & "\\n" & (om as string) & "\\n" & "NORUN"\n'
        "  end try\n"
        "end tell"
    )
    out = osa(script)
    if not out:
        return -1, None, {}
    parts = out.split("\n")
    try:
        vol = max(0, min(100, int(float(parts[0]))))
    except (ValueError, IndexError):
        vol = -1
    muted = None
    if len(parts) > 1:
        if parts[1] == "true":
            muted = True
        elif parts[1] == "false":
            muted = False
    if len(parts) < 8 or parts[2] == "NORUN":
        return vol, muted, {}
    state, track, artist, uri, pos, dur = parts[2:8]
    try:
        # player position is seconds as a float; duration is already ms.
        pos_ms = int(float(pos) * 1000)
        dur_ms = int(float(dur))
    except ValueError:
        pos_ms, dur_ms = -1, -1
    return vol, muted, {
        "sp_playing": "1" if state == "playing" else "0",
        # A newline in a track name would break the line-per-field wire format.
        # Flattened rather than escaped: not worth a parser on the device.
        "sp_track": track.replace("\n", " ")[:120],
        "sp_artist": artist.replace("\n", " ")[:120],
        "sp_uri": uri[:60],
        "sp_pos_ms": str(pos_ms),
        "sp_dur_ms": str(dur_ms),
    }


_last_locked = None


def screen_locked():
    """True when the screen is locked.

    Checks TWO keys and takes either as locked, because I could not verify which
    one actually flips without locking the machine:

      CGSSessionScreenIsLocked - absent entirely while unlocked, which is why a
        grep for it found nothing and I wrongly concluded it was unavailable.
      IOConsoleLocked          - present and False while unlocked. Whether it
        tracks SCREEN lock or console access lock is unclear, so it is treated
        as a second opinion rather than the answer.

    Quartz's CGSessionCopyCurrentDictionary would settle it, but the system
    python is 3.9 with no Quartz module and a pip dependency in something that
    runs at login is not worth it.

    Reports UNLOCKED when it cannot tell, matching HostLink's fail-open stance:
    its header records that getting this backwards means a dead helper
    permanently bricks the display.
    """
    global _last_locked
    # TTL'd: this dumps and parses a sizeable plist, and it was the single most
    # expensive thing in the beat. Lock state does not change fast enough to
    # need reading every second.
    out = cached("ioreg", 3.0, lambda: run(["ioreg", "-n", "Root", "-d1", "-a"]))
    if not out:
        return False
    try:
        d = plistlib.loads(out.encode("utf-8"))
    except Exception:
        return False

    found = {"screen": None, "console": None}

    def walk(o):
        if isinstance(o, dict):
            if "CGSSessionScreenIsLocked" in o and found["screen"] is None:
                found["screen"] = o["CGSSessionScreenIsLocked"]
            if "IOConsoleLocked" in o and found["console"] is None:
                found["console"] = o["IOConsoleLocked"]
            for v in o.values():
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(d)
    locked = bool(found["screen"]) or bool(found["console"])
    if locked != _last_locked:
        _last_locked = locked
        log(f"screen {'LOCKED' if locked else 'unlocked'} "
            f"(screenIsLocked={found['screen']!r} consoleLocked={found['console']!r})")
    return locked


# The knob's own overlay, built from tools/knobhud.m. Drawing a window needs no
# permission, which is the whole reason it replaced volhud: posting synthetic
# media keys needed an Accessibility grant that BINDS TO THE BINARY'S SIGNATURE,
# and every rebuild mints a new ad-hoc signature - so rebuilding the tool
# silently voided its own grant. It worked exactly once, right after the user
# granted it, and never after the next rebuild.
KNOBHUD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "knobhud")

# How stale the Spotify/audio read may be. The device consumes sp_playing and
# sp_uri (for the 20s poll stretch) and identity - none of it needs 1Hz
# sampling, and the read is the single most expensive spawn in the beat:
# measured at ~200ms warm, ~550ms cold, ON EVERY BEAT. That spawn WAS the
# volume delay - each delivered command paid apply+rebuild+RTT, a ~360ms
# cadence that made a spin land in lumps.
AUDIO_TTL_S = 2.0

# macOS moves output volume in sixteenths, so one knob detent is 6.25 points.
_STEP = 100.0 / 16.0


def _read_volume():
    v = osa("output volume of (get volume settings)")
    try:
        return int(float(v))
    except (TypeError, ValueError):
        return None


# What we believe the Mac's volume and mute state are.
#
# Cached rather than read per click, because reading was the delay: each
# osascript is a process spawn of roughly a tenth of a second, and reading
# before every adjustment made a slow knob feel like a wild one. The cache
# self-corrects for free - every beat reads the real values for the body and
# refreshes it, so an estimate can only drift for one beat.
_volume = None
_muted = None

_hud_proc = None
_hud_missing_said = False


def note_volume(actual, muted=None):
    """Called from the beat with the freshly read state."""
    global _volume, _muted
    if actual is not None and actual >= 0:
        _volume = actual
    if muted is not None:
        _muted = muted


def _hud_send(line):
    """Feed the overlay, keeping ONE long-lived process.

    A process per click would stack overlapping windows during a fast spin;
    a persistent child updates one panel in place. Respawned once if it died;
    absent entirely (not built) it is skipped and said once.
    """
    global _hud_proc, _hud_missing_said
    if not os.access(KNOBHUD, os.X_OK):
        if not _hud_missing_said:
            _hud_missing_said = True
            log("knobhud not built; volume works but nothing shows on screen "
                "(clang -framework Cocoa -o tools/knobhud tools/knobhud.m)")
        return
    for _ in range(2):
        try:
            if _hud_proc is None or _hud_proc.poll() is not None:
                _hud_proc = subprocess.Popen([KNOBHUD], stdin=subprocess.PIPE)
            _hud_proc.stdin.write((line + "\n").encode("utf-8"))
            _hud_proc.stdin.flush()
            return
        except (OSError, ValueError) as e:
            # Say WHICH attempt died. Both failing used to be silent, and a
            # silent overlay failure looks exactly like a painted one from the
            # sending side - the same invisible-failure trap as everything else
            # today.
            log(f"overlay write failed ({e}); respawning")
            try:
                _hud_proc.kill()
            except Exception:
                pass
            _hud_proc = None
    log("overlay unreachable after respawn; volume still applied")


def adjust_output_volume(delta):
    """Move the output volume by `delta` clicks.

    A delta, not a target, because a knob produces discrete detents and macOS
    steps volume in sixteenths - one maps to the other, and there is no absolute
    value for the two ends to disagree about.

    ONE osascript per adjustment, computed from the cache. The overlay is
    notified either way, so cranking against the 0 or 100 stop still shows the
    panel - silence at the stops read as "broken" in testing, not as "full".

    Re-clamped and re-typed here rather than trusted: the device is the less
    trusted end of a channel that terminates in a shell-out, and an allowlist
    is only as good as its argument checking.
    """
    global _volume
    try:
        delta = int(delta)
    except (TypeError, ValueError):
        log("bad volume delta, ignored")
        return
    if delta == 0:
        return
    delta = max(-16, min(16, delta))

    if _volume is None:
        _volume = _read_volume()
        if _volume is None:
            log("volume unreadable; nothing done")
            return

    target = max(0, min(100, int(round(_volume + delta * _STEP))))
    # Panel FIRST. The osascript takes ~110ms and the panel write takes none,
    # so the screen shows the target while the set is still in flight - the
    # perceived delay was mostly this ordering.
    if _muted is not None:
        _hud_send(f"muted {1 if _muted else 0}")
    _hud_send(f"volume {target}")
    if target != _volume:
        _volume = target
        osa(f"set volume output volume {target}")
        log(f"volume -> {target}")


# Only these keys are ever acted on. Adding a capability means adding an entry
# here with a typed handler, not widening a hole.
HANDLERS = {
    "adjust_output_volume": adjust_output_volume,
}


def build_body(token):
    name = computer_name()

    # Lock state FIRST, and when locked the AppleScript reads are skipped
    # entirely.
    #
    # This is not an optimisation, it is the fix for a real bug. Spotify does not
    # answer Apple events while the screen is locked - the read hangs for the
    # whole subprocess timeout. build_body then took longer than the request's
    # own budget, so the beat carrying locked=1 never got sent, and the device
    # never learned to sleep. The lock state was being read correctly and then
    # thrown away.
    #
    # Skipping is also just correct: a screen that is about to go dark has no
    # use for now-playing, and out_vol is reported as unknown rather than
    # guessed.
    locked = screen_locked()
    if locked:
        fields = [
            f"v={PROTOCOL_V}",
            f"tok={token}",
            "locked=1",
            f"host={name}",
            f"sp_device={name}",
            "out_vol=-1",
        ]
        return ("\n".join(fields) + "\n").encode("utf-8")

    def _read_audio():
        # note_volume fires ONLY on a real read. Calling it with a cached tuple
        # would clobber the fresher value adjust_output_volume maintains, and
        # reporting that stale number would resync the device's ring backwards -
        # the oscillation bug reborn through the cache.
        v, mu, pb = audio_and_playback()
        note_volume(v, mu)
        return v, mu, pb

    vol, muted, playback = cached("audio", AUDIO_TTL_S, _read_audio)
    out_vol = _volume if _volume is not None else vol
    out_muted = _muted if _muted is not None else muted
    fields = [
        f"v={PROTOCOL_V}",
        f"tok={token}",
        "locked=0",
        f"host={name}",
        f"sp_device={name}",
        f"out_vol={out_vol}",
    ]
    if out_muted is not None:
        fields.append(f"out_muted={1 if out_muted else 0}")
    for k, v in playback.items():
        fields.append(f"{k}={v}")
    return ("\n".join(fields) + "\n").encode("utf-8")


def parse_response(text):
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, _, v = line.partition("=")
            out[k.strip()] = v.strip()
    return out


_host_idx = 0
_announced = False


def beat(token):
    global _host_idx
    host = HOSTS[_host_idx]
    req = urllib.request.Request(
        f"http://{host}/beat",
        data=build_body(token),
        method="POST",
        headers={"Content-Type": "text/plain"},
    )
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT_S) as r:
            body = r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        # HTTPError is a SUBCLASS of URLError, so the handler below used to
        # swallow it: an HTTP error was logged as "unreachable" AND rotated the
        # host, which is exactly wrong - the host answered, it just said no.
        # That double-logging is why a 404 looked like two separate failures.
        #
        # The body is logged because without it a 404 is unattributable, and
        # this device answers 404 with a line saying what it wants.
        try:
            detail = e.read()[:120].decode("utf-8", "replace").strip()
        except Exception:
            detail = ""
        log(f"{host} answered HTTP {e.code}: {detail!r}")
        raise
    except (urllib.error.URLError, OSError, TimeoutError):
        # Genuinely unreachable. Rotate before propagating, so the caller's
        # backoff applies once per full pass rather than once per address.
        if len(HOSTS) > 1:
            _host_idx = (_host_idx + 1) % len(HOSTS)
            log(f"{host} unreachable; next try via {HOSTS[_host_idx]}")
        raise
    fields = parse_response(body)

    # The token must come back. Without this, anything that can answer on the
    # device's address - ARP or mDNS spoofing - could drive this Mac.
    if fields.get("tok") != token:
        log("response token mismatch, ignoring")
        return
    if fields.get("v") != str(PROTOCOL_V):
        log(f"response version {fields.get('v')!r}, ignoring")
        return

    global _announced
    if not _announced:
        _announced = True
        log(f"beating via {host}")

    for key, value in fields.items():
        handler = HANDLERS.get(key)
        if handler:
            # The receipt timestamp splits the pipeline: delay BEFORE this line
            # lives on the device/network/hold side, delay after it is apply.
            log(f"cmd {key}={value}")
            handler(value)


def main():
    token = read_token()
    if not token:
        log("no token; set KNOB_TOKEN or write ~/.config/knob-spotify/token")
        return 1
    log(f"starting, hosts={','.join(HOSTS)}")
    # Warm the overlay now, not on the first click: a Cocoa app takes ~half a
    # second to launch, and lazily paying that on the first turn showed up as
    # "very delayed to show up on first instance". "muted 0" initialises state
    # without showing the panel.
    _hud_send("muted 0")
    backoff = BACKOFF_START_S
    while True:
        try:
            beat(token)
            backoff = BACKOFF_START_S
        except urllib.error.HTTPError as e:
            if e.code == 401:
                log("401: the device rejected the token")
            else:
                log(f"HTTP {e.code}")
            time.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX_S)
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            # Back off rather than spin. A knob that is unplugged must never
            # become a busy process on this laptop.
            log(f"unreachable ({e}); retrying in {backoff:.0f}s")
            time.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX_S)


if __name__ == "__main__":
    sys.exit(main())
