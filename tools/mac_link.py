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
# Comfortably longer than the device's HOLD_MS, so a held request is answered by
# the device rather than timed out here.
TIMEOUT_S = 4.0
BACKOFF_START_S = 1.0
BACKOFF_MAX_S = 30.0
# Anything the device sends that is not in HANDLERS is ignored, so a newer
# firmware talking to an older helper degrades instead of breaking.


def log(msg):
    print(f"[maclink] {msg}", file=sys.stderr, flush=True)


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
        out = subprocess.run(
            args, capture_output=True, text=True, timeout=5, input=stdin_text
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
    """System volume AND Spotify's now-playing, in ONE osascript call.

    Two separate calls meant two process spawns a second for data that arrives
    from the same interpreter. Both are AppleScript, so both fit in one script.

    Returns (out_vol, playback_dict). out_vol is -1 when unreadable - never 0,
    because the device renders unknown as unknown and a confident zero would
    draw an empty slider for a value nobody measured.
    """
    script = (
        "set ov to output volume of (get volume settings)\n"
        'if application "Spotify" is not running then return (ov as string) & "\n" & "NORUN"\n'
        'tell application "Spotify"\n'
        "  set s to player state as string\n"
        "  set t to name of current track\n"
        "  set a to artist of current track\n"
        "  set u to spotify url of current track\n"
        "  set p to player position\n"
        "  set d to duration of current track\n"
        '  return (ov as string) & "\n" & s & "\n" & t & "\n" & a & "\n" & u '
        '& "\n" & p & "\n" & d\n'
        "end tell"
    )
    out = osa(script)
    if not out:
        # The combined script failed - and folding both reads into one call for
        # speed also FOLDED THEIR FAILURES together. The volume half needs no
        # Automation permission (get volume settings is a system call, not an
        # Apple event to an app), so losing it because Spotify is not authorised
        # is the optimisation costing more than it saved.
        #
        # Fall back to volume alone. One extra spawn, only when the fast path
        # has already failed.
        v = osa("output volume of (get volume settings)")
        try:
            return max(0, min(100, int(float(v)))), {}
        except (TypeError, ValueError):
            return -1, {}
    parts = out.split("\n")
    try:
        vol = max(0, min(100, int(float(parts[0]))))
    except (ValueError, IndexError):
        vol = -1
    if len(parts) < 7 or parts[1] == "NORUN":
        return vol, {}
    state, track, artist, uri, pos, dur = parts[1:7]
    try:
        # player position is seconds as a float; duration is already ms.
        pos_ms = int(float(pos) * 1000)
        dur_ms = int(float(dur))
    except ValueError:
        pos_ms, dur_ms = -1, -1
    return vol, {
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


def set_output_volume(pct):
    """The one command this helper will act on.

    Re-clamped here rather than trusted. The device is the less trusted end of a
    channel that terminates in osascript, and an allowlist is only as good as
    its argument checking.
    """
    try:
        pct = int(pct)
    except (TypeError, ValueError):
        log("bad volume argument, ignored")
        return
    pct = max(0, min(100, pct))
    osa(f"set volume output volume {pct}")
    log(f"output volume -> {pct}")


# Only these keys are ever acted on. Adding a capability means adding an entry
# here with a typed handler, not widening a hole.
HANDLERS = {
    "set_output_volume": set_output_volume,
}


def build_body(token):
    name = computer_name()
    vol, playback = audio_and_playback()
    fields = [
        f"v={PROTOCOL_V}",
        f"tok={token}",
        f"locked={1 if screen_locked() else 0}",
        f"host={name}",
        f"sp_device={name}",
        f"out_vol={vol}",
    ]
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
    except (urllib.error.URLError, OSError, TimeoutError):
        # Rotate to the next candidate before propagating, so the caller's
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

    for key, value in fields.items():
        handler = HANDLERS.get(key)
        if handler:
            handler(value)


def main():
    token = read_token()
    if not token:
        log("no token; set KNOB_TOKEN or write ~/.config/knob-spotify/token")
        return 1
    log(f"starting, hosts={','.join(HOSTS)}")
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
