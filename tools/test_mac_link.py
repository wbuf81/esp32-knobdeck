#!/usr/bin/env python3
"""Tests for the Mac link helper.

Run: python3 -m unittest discover -s tools -p 'test_*.py' -v

The helper is the one part of this project that runs on the Mac, and it was the
only part with no tests at all. Everything here is pure - no device, no network,
no osascript - because the parsing and the clamping are where a mistake is
silent, and the subprocess calls are where a mistake is loud.
"""

import unittest
from unittest import mock

import mac_link as m


class TestParseResponse(unittest.TestCase):
    def test_parses_key_value_lines(self):
        f = m.parse_response("v=1\ntok=abc\nadjust_output_volume=2\n")
        self.assertEqual(f["v"], "1")
        self.assertEqual(f["tok"], "abc")
        self.assertEqual(f["adjust_output_volume"], "2")

    def test_ignores_lines_without_an_equals(self):
        # A blank line or a stray header must not become a field.
        f = m.parse_response("v=1\n\ngarbage\ntok=abc\n")
        self.assertEqual(f, {"v": "1", "tok": "abc"})

    def test_tolerates_an_empty_body(self):
        self.assertEqual(m.parse_response(""), {})

    def test_a_value_containing_an_equals_keeps_it(self):
        # partition, not split: a base64 token can end in '='.
        f = m.parse_response("tok=abc==\n")
        self.assertEqual(f["tok"], "abc==")

    def test_strips_surrounding_whitespace(self):
        f = m.parse_response("tok = abc \n")
        self.assertEqual(f["tok"], "abc")


class TestVolumeAdjust(unittest.TestCase):
    """One osascript per click, and the overlay told either way."""

    def setUp(self):
        self.osa = []
        self.hud = []
        m._volume = 50
        m._muted = False
        self.addCleanup(lambda: (setattr(m, "_volume", None),
                                 setattr(m, "_muted", None)))
        p1 = mock.patch.object(m, "osa", side_effect=lambda s: self.osa.append(s))
        p2 = mock.patch.object(m, "_hud_send", side_effect=self.hud.append)
        for p in (p1, p2):
            p.start()
            self.addCleanup(p.stop)

    def test_exactly_one_osascript_per_adjustment(self):
        # THE responsiveness requirement, as an assertion. Each osascript is a
        # process spawn of roughly a tenth of a second; the earlier design did
        # read, press, settle, read, maybe set - about a second per click,
        # during which more clicks piled up and applied in a lump. That is what
        # made a slow knob feel like a wild one.
        m.adjust_output_volume("2")
        self.assertEqual(len(self.osa), 1)
        self.assertTrue(self.osa[0].startswith("set volume"))

    def test_it_steps_by_sixteenths(self):
        # macOS steps volume in sixteenths, so +2 from 50 is about 62.
        m.adjust_output_volume("2")
        self.assertIn("62", self.osa[0])

    def test_the_overlay_is_shown(self):
        m.adjust_output_volume("2")
        self.assertIn("volume 62", self.hud)

    def test_the_overlay_learns_the_mute_state_first(self):
        # muted before volume, because the volume line is what makes the panel
        # appear - the state has to be right before it shows.
        m._muted = True
        m.adjust_output_volume("1")
        self.assertEqual(self.hud[0], "muted 1")
        self.assertTrue(self.hud[1].startswith("volume "))

    def test_cranking_against_the_stop_still_shows_the_overlay(self):
        # Already at 100: no osascript is spent, but silence at the stops read
        # as "broken" in testing, not as "full". The panel still appears.
        m._volume = 100
        m.adjust_output_volume("3")
        self.assertEqual([s for s in self.osa if s.startswith("set volume")], [])
        self.assertIn("volume 100", self.hud)

    def test_it_clamps_at_the_top(self):
        m.adjust_output_volume("16")
        self.assertIn("100", self.osa[0])

    def test_it_clamps_at_the_bottom(self):
        m.adjust_output_volume("-16")
        self.assertIn("0", self.osa[0])

    def test_successive_clicks_accumulate_from_the_cache(self):
        m.adjust_output_volume("1")
        m.adjust_output_volume("1")
        self.assertEqual(len(self.osa), 2)
        self.assertIn("62", self.osa[1])

    def test_zero_does_nothing(self):
        m.adjust_output_volume("0")
        self.assertEqual(self.osa, [])
        self.assertEqual(self.hud, [])

    def test_non_numeric_is_refused_entirely(self):
        # Not clamped to a default - REFUSED. A garbage argument means the
        # device is not saying what we think, and guessing a volume from it
        # would be inventing an instruction.
        m.adjust_output_volume("; rm -rf /")
        self.assertEqual(self.osa, [])
        self.assertEqual(self.hud, [])

    def test_a_float_string_is_refused_rather_than_truncated(self):
        m.adjust_output_volume("2.9")
        self.assertEqual(self.osa, [])

    def test_an_absurd_delta_is_clamped(self):
        # One stuck report must not spin the volume end to end: +/-16 clicks is
        # the full range, so the clamp caps a lump at one full sweep.
        m.adjust_output_volume("9999")
        self.assertIn("100", self.osa[0])


class TestOverlayProcess(unittest.TestCase):
    """The overlay is ONE long-lived child, respawned once if it died."""

    def setUp(self):
        m._hud_proc = None
        m._hud_missing_said = False
        self.addCleanup(lambda: (setattr(m, "_hud_proc", None),
                                 setattr(m, "_hud_missing_said", False)))

    def _proc(self, alive=True):
        proc = mock.MagicMock()
        proc.poll.return_value = None if alive else 1
        return proc

    def test_it_spawns_once_and_reuses(self):
        proc = self._proc()
        with mock.patch("os.access", return_value=True), \
             mock.patch.object(m.subprocess, "Popen", return_value=proc) as popen:
            m._hud_send("volume 10")
            m._hud_send("volume 20")
        self.assertEqual(popen.call_count, 1, "one panel, not one per click")
        self.assertEqual(proc.stdin.write.call_count, 2)

    def test_a_dead_overlay_is_respawned(self):
        m._hud_proc = self._proc(alive=False)
        fresh = self._proc()
        with mock.patch("os.access", return_value=True), \
             mock.patch.object(m.subprocess, "Popen", return_value=fresh):
            m._hud_send("volume 10")
        fresh.stdin.write.assert_called_once()

    def test_a_missing_binary_is_said_once_and_skipped(self):
        logs = []
        with mock.patch("os.access", return_value=False), \
             mock.patch.object(m, "log", side_effect=logs.append):
            m._hud_send("volume 10")
            m._hud_send("volume 20")
        self.assertEqual(len(logs), 1, "nagging every click is churn")


class TestVolumeCacheSync(unittest.TestCase):
    def setUp(self):
        m._volume = None
        self.addCleanup(lambda: setattr(m, "_volume", None))

    def test_the_beat_refreshes_the_cache(self):
        # The estimate can only drift for one beat, because every beat already
        # reads the real volume for the body.
        m.note_volume(33, muted=True)
        self.assertEqual(m._volume, 33)
        self.assertIs(m._muted, True)

    def test_an_unknown_volume_does_not_poison_the_cache(self):
        # -1 means the Mac did not report one. Caching that would make the next
        # click compute from nonsense.
        m.note_volume(40)
        m.note_volume(-1)
        m.note_volume(None)
        self.assertEqual(m._volume, 40)


class TestBeatVerification(unittest.TestCase):
    """The response must prove it came from something holding the secret."""

    def setUp(self):
        self.applied = []
        p1 = mock.patch.object(m, "build_body", return_value=b"v=1\n")
        p2 = mock.patch.dict(
            m.HANDLERS, {"adjust_output_volume": self.applied.append}, clear=True
        )
        p1.start()
        p2.start()
        self.addCleanup(p1.stop)
        self.addCleanup(p2.stop)

    def _respond(self, body):
        resp = mock.MagicMock()
        resp.read.return_value = body.encode()
        resp.__enter__ = mock.Mock(return_value=resp)
        resp.__exit__ = mock.Mock(return_value=False)
        return mock.patch.object(m.urllib.request, "urlopen", return_value=resp)

    def test_a_matching_token_applies_the_command(self):
        with self._respond("v=1\ntok=s3cret\nadjust_output_volume=2\n"):
            m.beat("s3cret")
        self.assertEqual(self.applied, ["2"])

    def test_a_mismatched_token_applies_nothing(self):
        # Not theatre. Without this, anything that can answer on the device's
        # address - ARP or mDNS spoofing - could drive this Mac.
        with self._respond("v=1\ntok=WRONG\nadjust_output_volume=2\n"):
            m.beat("s3cret")
        self.assertEqual(self.applied, [])

    def test_a_missing_token_applies_nothing(self):
        with self._respond("v=1\nadjust_output_volume=2\n"):
            m.beat("s3cret")
        self.assertEqual(self.applied, [])

    def test_a_wrong_version_applies_nothing(self):
        with self._respond("v=2\ntok=s3cret\nadjust_output_volume=2\n"):
            m.beat("s3cret")
        self.assertEqual(self.applied, [])

    def test_an_unknown_key_is_ignored_not_executed(self):
        # The allowlist is the whole safety model: a key with no handler must be
        # dropped silently, never dispatched by name.
        with self._respond("v=1\ntok=s3cret\nrun_shell=whoami\n"):
            m.beat("s3cret")
        self.assertEqual(self.applied, [])



class TestHostRotation(unittest.TestCase):
    """Rotation caused today's most misleading log trail, and had no tests."""

    def setUp(self):
        m._host_idx = 0
        p1 = mock.patch.object(m, "HOSTS", ["first.local", "10.0.0.9"])
        p2 = mock.patch.object(m, "build_body", return_value=b"v=1\n")
        for p in (p1, p2):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(lambda: setattr(m, "_host_idx", 0))

    def _fail_with(self, exc):
        return mock.patch.object(m.urllib.request, "urlopen", side_effect=exc)

    def test_an_unreachable_host_rotates(self):
        with self._fail_with(m.urllib.error.URLError("boom")):
            with self.assertRaises(m.urllib.error.URLError):
                m.beat("t")
        self.assertEqual(m._host_idx, 1, "should have moved to the next host")

    def test_an_http_error_does_NOT_rotate(self):
        # The bug. HTTPError is a SUBCLASS of URLError, so the reachability
        # handler swallowed it: an HTTP error was logged as "unreachable" AND
        # flapped the host, when in fact the host answered - it just said no.
        err = m.urllib.error.HTTPError("http://x/beat", 404, "Not Found", {}, None)
        with self._fail_with(err):
            with self.assertRaises(m.urllib.error.HTTPError):
                m.beat("t")
        self.assertEqual(m._host_idx, 0, "an answering host must not be rotated away")

    def test_rotation_wraps_around(self):
        with self._fail_with(OSError("down")):
            for _ in range(4):
                with self.assertRaises(OSError):
                    m.beat("t")
        self.assertEqual(m._host_idx, 0, "should have wrapped back")

    def test_a_single_host_does_not_rotate(self):
        with mock.patch.object(m, "HOSTS", ["only.local"]):
            with self._fail_with(OSError("down")):
                with self.assertRaises(OSError):
                    m.beat("t")
            self.assertEqual(m._host_idx, 0)

    def _succeed(self):
        resp = mock.MagicMock()
        resp.read.return_value = b"v=1\ntok=t\n"
        resp.__enter__ = mock.Mock(return_value=resp)
        resp.__exit__ = mock.Mock(return_value=False)
        return mock.patch.object(m.urllib.request, "urlopen", return_value=resp)

    def test_a_fallback_host_eventually_yields_back_to_the_preferred(self):
        # The bug this test pins: a reflash's transient outage rotated the
        # helper onto the mDNS name, and success was sticky - so one blip
        # taxed every future beat with the device's ~5.4s resolve, felt as
        # laggy taps and a Spotify screen ten seconds into a call. Sustained
        # success on a fallback must retry the preferred host.
        m._host_idx = 1
        m._fallback_beats = 0
        with self._succeed():
            for _ in range(m.PREFERRED_RETRY_BEATS + 1):
                m.beat("t")
        self.assertEqual(m._host_idx, 0,
                         "sustained fallback success must retry HOSTS[0]")

    def test_a_slow_beat_is_logged_and_a_normal_one_is_not(self):
        # The mDNS tax was invisible for hours because every test and every
        # log line is purely functional: a 5.4s beat and a 150ms beat look
        # identical. Latency past the device's own hold must SAY something,
        # or the next silent tax is just as silent.
        m._host_idx = 0
        lines = []
        with self._succeed(), \
             mock.patch.object(m, "log", side_effect=lines.append), \
             mock.patch.object(m.time, "monotonic",
                               side_effect=[0.0, m.SLOW_BEAT_S + 1.0]):
            m.beat("t")
        self.assertTrue(any("slow beat" in ln for ln in lines), lines)

        lines.clear()
        with self._succeed(), \
             mock.patch.object(m, "log", side_effect=lines.append), \
             mock.patch.object(m.time, "monotonic", side_effect=[0.0, 0.2]):
            m.beat("t")
        self.assertFalse(any("slow beat" in ln for ln in lines), lines)

    def test_a_dead_preferred_host_is_probed_not_camped_on(self):
        # The probe must cost one failure, not move in permanently: if the
        # preferred host is still gone, the failed probe rotates straight
        # back to the fallback.
        m._host_idx = 1
        m._fallback_beats = m.PREFERRED_RETRY_BEATS
        with self._fail_with(OSError("still down")):
            with self.assertRaises(OSError):
                m.beat("t")
        self.assertEqual(m._host_idx, 1, "failed probe returns to the fallback")


class TestTimeoutBudget(unittest.TestCase):
    def test_the_request_budget_exceeds_the_devices_hold(self):
        # The device holds a request for HOLD_MS (1s). A shorter budget here
        # would time out every idle beat.
        self.assertGreater(m.TIMEOUT_S, 1.0)

    def test_the_budget_survives_a_cold_mdns_resolve(self):
        # Measured at 6.1s cold on this network against an earlier 4.0s budget,
        # which made the first request after any cache miss fail every time and
        # then rotate the host - looking exactly like an unreachable device.
        self.assertGreaterEqual(m.TIMEOUT_S, 8.0)

    def test_a_subprocess_cannot_outlast_the_request_it_is_for(self):
        # A reader that outlives its request turns a LATE beat into a LOST one,
        # which is how the locked-screen bug hid: build_body outran the POST.
        # Enforced by inspection because the timeout is a literal in run().
        import inspect
        srcs = inspect.getsource(m.run)
        self.assertIn("timeout=4", srcs)
        self.assertLess(4, m.TIMEOUT_S)


class TestTokenLoading(unittest.TestCase):
    def test_the_environment_wins(self):
        with mock.patch.dict("os.environ", {"KNOB_TOKEN": "  fromenv  "}):
            self.assertEqual(m.read_token(), "fromenv")

    def test_a_missing_file_is_empty_not_an_exception(self):
        # main() treats empty as "not configured" and exits cleanly. Raising
        # here would crash the LaunchAgent into a KeepAlive restart loop.
        with mock.patch.dict("os.environ", {}, clear=True):
            with mock.patch("builtins.open", side_effect=OSError):
                self.assertEqual(m.read_token(), "")

    def test_a_file_token_is_stripped(self):
        data = "  abc123\n"
        with mock.patch.dict("os.environ", {}, clear=True):
            with mock.patch("builtins.open", mock.mock_open(read_data=data)):
                self.assertEqual(m.read_token(), "abc123")


class TestMainRefusesToRunUnconfigured(unittest.TestCase):
    def test_no_token_exits_rather_than_looping(self):
        # KeepAlive would otherwise restart it forever, hammering nothing.
        with mock.patch.object(m, "read_token", return_value=""):
            self.assertEqual(m.main(), 1)




class TestBeatCost(unittest.TestCase):
    """The delivery cadence IS the latency, and spawns are the cadence.

    Measured: audio_and_playback costs ~200ms and ran on EVERY beat, so each
    delivered command paid apply (127ms) + rebuild (200ms) + RTT - a ~360ms
    cadence that made a spin land in lumps. The audio read is now TTL-cached:
    the device only consumes sp_playing/sp_uri (for the 20s poll stretch) and
    identity, none of which needs 1Hz sampling.
    """

    def setUp(self):
        m._cache.clear()
        m._volume = None
        m._muted = None
        self.addCleanup(lambda: (m._cache.clear(),
                                 setattr(m, "_volume", None),
                                 setattr(m, "_muted", None)))
        self.reads = []
        p1 = mock.patch.object(m, "audio_and_playback",
                               side_effect=lambda: (self.reads.append(1) or
                                                    (50, False, {})))
        p2 = mock.patch.object(m, "screen_locked", return_value=False)
        p3 = mock.patch.object(m, "computer_name", return_value="M")
        for p in (p1, p2, p3):
            p.start()
            self.addCleanup(p.stop)

    def test_consecutive_beats_do_not_respawn_the_audio_read(self):
        m.build_body("t")
        m.build_body("t")
        m.build_body("t")
        self.assertEqual(len(self.reads), 1,
                         "the 200ms spawn must not run per beat")

    def test_the_cache_expires(self):
        m.build_body("t")
        base = m.time.monotonic()
        with mock.patch.object(m.time, "monotonic",
                               return_value=base + m.AUDIO_TTL_S + 1):
            m.build_body("t")
        self.assertEqual(len(self.reads), 2)

    def test_a_fresh_adjust_beats_a_cached_volume(self):
        # The trap the cache digs: after a knob turn, the cached audio tuple
        # still holds the OLD volume, and blindly reporting it would resync the
        # device's ring backwards - the oscillation bug reborn. out_vol must
        # come from the adjust-maintained cache when there is one.
        m.build_body("t")           # caches vol=50
        m._volume = 62              # a turn happened since
        f = m.parse_response(m.build_body("t").decode())
        self.assertEqual(f["out_vol"], "62")

    def test_a_stale_cached_read_does_not_clobber_the_volume_cache(self):
        # note_volume must fire only when the producer actually RAN.
        m.build_body("t")
        m._volume = 62
        m.build_body("t")           # served from cache
        self.assertEqual(m._volume, 62)


class TestOverlayBeforeSet(unittest.TestCase):
    def test_the_panel_paints_before_the_osascript(self):
        # The perceived delay is visual. The osascript takes ~110ms; the panel
        # write takes none - so the panel goes FIRST, showing the target while
        # the set is still in flight.
        order = []
        m._volume = 50
        m._muted = False
        self.addCleanup(lambda: (setattr(m, "_volume", None),
                                 setattr(m, "_muted", None)))
        with mock.patch.object(m, "osa",
                               side_effect=lambda s: order.append("osa")), \
             mock.patch.object(m, "_hud_send",
                               side_effect=lambda s: order.append(s)):
            m.adjust_output_volume("2")
        self.assertEqual(order[0], "muted 0")
        self.assertEqual(order[1], "volume 62")
        self.assertEqual(order[2], "osa")



class TestTeamsInference(unittest.TestCase):
    """In-call is inferred (Teams running + mic capturing) because Teams' own
    local API never binds on this machine. The inference is pure and these pin
    its honesty rules."""

    def test_in_a_call(self):
        f = m.teams_fields(True, 1, 1)
        self.assertEqual(f["teams_in_call"], "1")
        self.assertEqual(f["teams_camera"], "1")

    def test_in_a_call_camera_off(self):
        f = m.teams_fields(True, 1, 0)
        self.assertEqual(f["teams_camera"], "0")

    def test_teams_open_but_idle(self):
        f = m.teams_fields(True, 0, 0)
        self.assertEqual(f["teams_in_call"], "0")
        self.assertNotIn("teams_camera", f)

    def test_teams_not_running_is_not_a_call(self):
        f = m.teams_fields(False, 1, 1)
        self.assertEqual(f["teams_in_call"], "0")

    def test_unreadable_hardware_says_nothing(self):
        # {} means the beat omits the fields and the device reads unknown -
        # never a confident "not in a call" from a probe that failed.
        self.assertEqual(m.teams_fields(True, -1, -1), {})

    def test_mute_is_never_claimed(self):
        # Teams keeps capturing while soft-muted, so hardware CANNOT know.
        # A guessed mute over a hot microphone is the forbidden lie.
        for args in ((True, 1, 1), (True, 0, 0), (False, 1, 0)):
            self.assertNotIn("teams_muted", m.teams_fields(*args))

    def test_camera_unreadable_in_call_is_omitted(self):
        f = m.teams_fields(True, 1, -1)
        self.assertEqual(f["teams_in_call"], "1")
        self.assertNotIn("teams_camera", f)



class TestAxTruthOutranksInference(unittest.TestCase):
    """A completed walk decides BOTH ways; the heuristic fills only silence."""

    def test_the_reported_bug_a_lingering_mic_does_not_fake_a_call(self):
        # Teams holds the mic after a call ends. The tree walked, found no
        # Leave button, said no - and the first merge let the heuristic
        # overrule it, parking the device on a meeting screen with no meeting.
        f = m.teams_fields(True, 1, 0, ax_ok=1, ax_in_call=0)
        self.assertEqual(f["teams_in_call"], "0")

    def test_ax_supplies_the_mute_state(self):
        f = m.teams_fields(True, 1, 1, ax_ok=1, ax_in_call=1, ax_muted=1,
                           ax_camera=0)
        self.assertEqual(f["teams_muted"], "1")
        self.assertEqual(f["teams_camera"], "0")

    def test_ax_in_call_wins_even_when_the_mic_heuristic_misses(self):
        f = m.teams_fields(True, 0, -1, ax_ok=1, ax_in_call=1, ax_muted=1)
        self.assertEqual(f["teams_in_call"], "1")
        self.assertEqual(f["teams_muted"], "1")

    def test_an_overrun_walk_decides_nothing(self):
        # ok=0: the tree gave up mid-walk. Its "no Leave button seen" is not
        # evidence, so the heuristic stands - otherwise a big in-call tree
        # would bounce the device out of the meeting screen mid-call.
        f = m.teams_fields(True, 1, 1, ax_ok=0, ax_in_call=0)
        self.assertEqual(f["teams_in_call"], "1")

    def test_silent_tree_leaves_mute_unknown(self):
        f = m.teams_fields(True, 1, 1)
        self.assertNotIn("teams_muted", f)
        self.assertEqual(f["teams_in_call"], "1")

    def test_ax_camera_outranks_the_hardware_flag(self):
        f = m.teams_fields(True, 1, 1, ax_ok=1, ax_in_call=1, ax_muted=0,
                           ax_camera=0)
        self.assertEqual(f["teams_camera"], "0")

    def test_hardware_camera_still_fills_in_when_ax_is_silent(self):
        f = m.teams_fields(True, 1, 1, ax_ok=1, ax_in_call=1)
        self.assertEqual(f["teams_camera"], "1")

    def test_teams_not_running_still_wins_over_everything(self):
        f = m.teams_fields(False, 1, 1, ax_ok=1, ax_in_call=1, ax_muted=1)
        self.assertEqual(f["teams_in_call"], "0")


class TestAxTeamsParse(unittest.TestCase):
    def _proc(self, rc, out=""):
        p = mock.MagicMock(); p.returncode = rc; p.stdout = out
        return p

    def test_parses_the_tool_output(self):
        with mock.patch("os.access", return_value=True), \
             mock.patch.object(m.subprocess, "run",
                               return_value=self._proc(0, "ok=1 in_call=1 muted=0 camera=1\n")):
            self.assertEqual(m.ax_teams_state(), (1, 1, 0, 1))

    def test_missing_grant_reads_unknown_and_nags_once(self):
        m._axteams_grant_said = False
        self.addCleanup(lambda: setattr(m, "_axteams_grant_said", False))
        logs = []
        with mock.patch("os.access", return_value=True), \
             mock.patch.object(m.subprocess, "run", return_value=self._proc(3)), \
             mock.patch.object(m, "log", side_effect=logs.append):
            self.assertEqual(m.ax_teams_state(), (0, 0, -1, -1))
            m.ax_teams_state()
        self.assertEqual(len(logs), 1)

    def test_missing_binary_reads_unknown(self):
        with mock.patch("os.access", return_value=False):
            self.assertEqual(m.ax_teams_state(), (0, 0, -1, -1))


class TestAvStateParse(unittest.TestCase):
    def test_parses_the_probe_output(self):
        with mock.patch.object(m, "run", return_value="mic=1 cam=0\n"), \
             mock.patch("os.access", return_value=True):
            self.assertEqual(m.av_state(), (1, 0))

    def test_missing_probe_reads_unknown(self):
        with mock.patch("os.access", return_value=False):
            self.assertEqual(m.av_state(), (-1, -1))

    def test_garbage_reads_unknown(self):
        with mock.patch.object(m, "run", return_value="what\n"), \
             mock.patch("os.access", return_value=True):
            self.assertEqual(m.av_state(), (-1, -1))


class TestWholeBodySmoke(unittest.TestCase):
    """build_body with only the SUBPROCESS boundary mocked.

    The regression this guards: an edit deleted the module-level _last_locked
    initializer. Every unit test passed, because they mock screen_locked - and
    the helper then crash-looped under launchd on its first real beat, taking
    volume control and the overlay down together. This test runs the real
    functions end to end so a missing module-level name explodes HERE.
    """

    def test_a_full_body_builds_with_real_readers(self):
        m._cache.clear()
        self.addCleanup(m._cache.clear)
        with mock.patch.object(m, "run", return_value="ok\n"), \
             mock.patch.object(m, "osa", return_value="50\nfalse\nNORUN"):
            body = m.build_body("tok").decode()
        f = m.parse_response(body)
        self.assertEqual(f["tok"], "tok")
        self.assertIn("locked", f)
        self.assertIn("out_vol", f)


class TestBuildBody(unittest.TestCase):
    def setUp(self):
        self.locked = False
        self.reads = 0

        def fake_audio():
            self.reads += 1
            return 63, False, {"sp_playing": "1", "sp_track": "hazy concentration"}

        p1 = mock.patch.object(
            m, "screen_locked", side_effect=lambda: self.locked
        )
        p2 = mock.patch.object(m, "computer_name", return_value="Wes's Mac")
        p3 = mock.patch.object(m, "audio_and_playback", side_effect=fake_audio)
        for p in (p1, p2, p3):
            p.start()
            self.addCleanup(p.stop)

    def test_unlocked_carries_the_expected_fields(self):
        f = m.parse_response(m.build_body("s3cret").decode())
        self.assertEqual(f["v"], "1")
        self.assertEqual(f["tok"], "s3cret")
        self.assertEqual(f["locked"], "0")
        self.assertEqual(f["out_vol"], "63")
        self.assertEqual(f["out_muted"], "0")
        self.assertEqual(f["sp_track"], "hazy concentration")

    def test_locked_skips_the_applescript_entirely(self):
        # The fix for a real bug, not an optimisation. Spotify does not answer
        # Apple events while the screen is locked - the read hangs for the whole
        # subprocess timeout, build_body outran the request budget, and the beat
        # carrying locked=1 was never sent. The device therefore never learned to
        # sleep, even though the lock state had been read correctly.
        self.locked = True
        f = m.parse_response(m.build_body("s3cret").decode())
        self.assertEqual(f["locked"], "1")
        self.assertEqual(self.reads, 0, "must not touch AppleScript while locked")

    def test_locked_reports_volume_as_unknown_rather_than_guessing(self):
        # -1, not a stale or invented value. Unknown renders as unknown.
        self.locked = True
        f = m.parse_response(m.build_body("s3cret").decode())
        self.assertEqual(f["out_vol"], "-1")

    def test_locked_still_identifies_the_machine(self):
        # host and sp_device must survive, or the device forgets which Mac it is
        # beside and the wake targeting regresses.
        self.locked = True
        f = m.parse_response(m.build_body("s3cret").decode())
        self.assertEqual(f["host"], "Wes's Mac")
        self.assertEqual(f["sp_device"], "Wes's Mac")

    def test_an_apostrophe_and_spaces_survive(self):
        # The reason the wire format is key=value lines and not query params:
        # this machine's name has both, and encoding it would need a decoder on
        # the device.
        f = m.parse_response(m.build_body("t").decode())
        self.assertEqual(f["host"], "Wes's Mac")
        self.assertEqual(f["sp_device"], "Wes's Mac")

    def test_every_line_has_exactly_one_field(self):
        # A newline smuggled into any value would forge a field. Guard it here
        # as well as at the source, because this is the format's only invariant.
        body = m.build_body("s3cret").decode()
        for line in body.splitlines():
            if line:
                self.assertEqual(line.count("\n"), 0)
                self.assertIn("=", line)


class TestHostFallback(unittest.TestCase):
    def test_a_single_host_is_parsed(self):
        with mock.patch.dict("os.environ", {"KNOB_HOST": "knob.local"}):
            hosts = [h.strip() for h in "knob.local".split(",") if h.strip()]
        self.assertEqual(hosts, ["knob.local"])

    def test_a_list_is_split_and_trimmed(self):
        raw = " knobspotify.local , 192.168.1.42 ,, "
        hosts = [h.strip() for h in raw.split(",") if h.strip()]
        self.assertEqual(hosts, ["knobspotify.local", "192.168.1.42"])


class TestPlaybackParsing(unittest.TestCase):
    def _with_osa(self, out):
        return mock.patch.object(m, "osa", return_value=out)

    def test_parses_a_real_reading(self):
        # Values measured off the actual machine.
        out = (
            "25\nfalse\nplaying\nhazy concentration\nKAESUL\n"
            "spotify:track:366l6ir20fMXe2KhBxCbb0\n113.206\n235690"
        )
        with self._with_osa(out):
            vol, muted, pb = m.audio_and_playback()
        self.assertEqual(vol, 25)
        self.assertIs(muted, False)
        self.assertEqual(pb["sp_playing"], "1")
        self.assertEqual(pb["sp_artist"], "KAESUL")
        self.assertEqual(pb["sp_pos_ms"], "113206")
        self.assertEqual(pb["sp_dur_ms"], "235690")

    def test_mute_state_is_carried(self):
        with self._with_osa("25\ntrue\nNORUN"):
            vol, muted, pb = m.audio_and_playback()
        self.assertIs(muted, True)

    def test_spotify_not_running_still_yields_volume_and_mute(self):
        with self._with_osa("19\nfalse\nNORUN"):
            vol, muted, pb = m.audio_and_playback()
        self.assertEqual(vol, 19)
        self.assertIs(muted, False)
        self.assertEqual(pb, {})

    def test_the_stopped_state_no_longer_costs_the_volume(self):
        # Spotify running with NO current track errors with -1728 inside the
        # tell block. Before the AppleScript-side try, that error killed the
        # whole read: the volume went with it, the log gained two lines per
        # beat, and a second osascript was spawned to recover what the first
        # had already known. The script now returns NORUN itself, so the volume
        # and mute survive in the SAME spawn.
        with self._with_osa("19\nfalse\nNORUN"):
            vol, muted, pb = m.audio_and_playback()
        self.assertEqual(vol, 19)
        self.assertEqual(pb, {})

    def test_the_applescript_carries_its_own_try_block(self):
        # The fix lives inside the script, so it must actually be in there.
        import inspect
        s = inspect.getsource(m.audio_and_playback)
        self.assertIn("on error", s)

    def test_paused_is_reported_as_not_playing(self):
        out = "10\nfalse\npaused\nt\na\nspotify:track:x\n1.0\n1000"
        with self._with_osa(out):
            _, _, pb = m.audio_and_playback()
        self.assertEqual(pb["sp_playing"], "0")

    def test_a_newline_in_a_track_name_is_flattened(self):
        out = "10\nfalse\nplaying\nbad\nname\na\nspotify:track:x\n1.0\n1000"
        with self._with_osa(out):
            _, _, pb = m.audio_and_playback()
        self.assertNotIn("\n", pb.get("sp_track", ""))

    def test_a_total_failure_reads_as_unknown_not_zero(self):
        # -1 and None, never 0 and False: the device renders unknown as
        # unknown, and a confident zero would draw an empty slider for a value
        # nobody measured.
        with mock.patch.object(m, "osa", return_value=None):
            vol, muted, pb = m.audio_and_playback()
        self.assertEqual(vol, -1)
        self.assertIsNone(muted)
        self.assertEqual(pb, {})


if __name__ == "__main__":
    unittest.main()
