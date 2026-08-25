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
    """The device is the less trusted end of a channel ending in a keypress."""

    def setUp(self):
        self.runs = []
        self.osa = []
        p1 = mock.patch.object(m, "run", side_effect=lambda a, **k: self.runs.append(a))
        p2 = mock.patch.object(m, "osa", side_effect=lambda s: self.osa.append(s))
        # volhud present and executable: the HUD path.
        p3 = mock.patch("os.access", return_value=True)
        for p in (p1, p2, p3):
            p.start()
            self.addCleanup(p.stop)

    def test_one_detent_is_one_keypress(self):
        m.adjust_output_volume("1")
        self.assertEqual(len(self.runs), 1)
        self.assertEqual(self.runs[0][1], "up")

    def test_three_detents_are_three_keypresses(self):
        # A delta is a count of clicks, and the HUD steps once per press.
        m.adjust_output_volume("3")
        self.assertEqual(len(self.runs), 3)
        self.assertTrue(all(r[1] == "up" for r in self.runs))

    def test_a_negative_delta_presses_down(self):
        m.adjust_output_volume("-2")
        self.assertEqual([r[1] for r in self.runs], ["down", "down"])

    def test_zero_does_nothing(self):
        m.adjust_output_volume("0")
        self.assertEqual(self.runs, [])
        self.assertEqual(self.osa, [])

    def test_non_numeric_is_refused_entirely(self):
        # Not clamped to a default - REFUSED. A garbage argument means the device
        # is not saying what we think, and guessing a volume from it would be
        # inventing an instruction.
        m.adjust_output_volume("; rm -rf /")
        self.assertEqual(self.runs, [])
        self.assertEqual(self.osa, [])

    def test_a_float_string_is_refused_rather_than_truncated(self):
        m.adjust_output_volume("2.9")
        self.assertEqual(self.runs, [])

    def test_an_absurd_delta_is_clamped(self):
        # One stuck report must not be able to spin the volume end to end.
        m.adjust_output_volume("9999")
        self.assertEqual(len(self.runs), 16)

    def test_the_hud_binary_is_preferred_over_applescript(self):
        m.adjust_output_volume("1")
        self.assertEqual(self.osa, [], "must not fall back while volhud exists")


class TestVolumeAdjustFallback(unittest.TestCase):
    """No volhud: change it silently rather than not at all."""

    def setUp(self):
        self.osa = []

        def fake_osa(script):
            self.osa.append(script)
            if "get volume settings" in script:
                return "50"
            return ""

        p1 = mock.patch.object(m, "osa", side_effect=fake_osa)
        p2 = mock.patch.object(m, "run", side_effect=AssertionError("no binary"))
        p3 = mock.patch("os.access", return_value=False)
        for p in (p1, p2, p3):
            p.start()
            self.addCleanup(p.stop)

    def test_falls_back_to_an_absolute_set(self):
        # One step is 100/16 = 6.25 points, so +2 from 50 lands on about 62.
        m.adjust_output_volume("2")
        sets = [s for s in self.osa if s.startswith("set volume")]
        self.assertEqual(len(sets), 1)
        self.assertIn("62", sets[0])

    def test_the_fallback_clamps_at_the_top(self):
        m.adjust_output_volume("16")
        sets = [s for s in self.osa if s.startswith("set volume")]
        self.assertIn("100", sets[0])

    def test_the_fallback_clamps_at_the_bottom(self):
        m.adjust_output_volume("-16")
        sets = [s for s in self.osa if s.startswith("set volume")]
        self.assertIn("0", sets[0])


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


class TestBuildBody(unittest.TestCase):
    def setUp(self):
        self.locked = False
        self.reads = 0

        def fake_audio():
            self.reads += 1
            return 63, {"sp_playing": "1", "sp_track": "hazy concentration"}

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
            "25\nplaying\nhazy concentration\nKAESUL\n"
            "spotify:track:366l6ir20fMXe2KhBxCbb0\n113.206\n235690"
        )
        with self._with_osa(out):
            vol, pb = m.audio_and_playback()
        self.assertEqual(vol, 25)
        self.assertEqual(pb["sp_playing"], "1")
        self.assertEqual(pb["sp_artist"], "KAESUL")
        self.assertEqual(pb["sp_pos_ms"], "113206")
        self.assertEqual(pb["sp_dur_ms"], "235690")

    def test_spotify_not_running_still_yields_the_volume(self):
        # The failure coupling that combining the two reads introduced: a
        # missing Spotify must not cost the volume, which needs no permission.
        with self._with_osa("19\nNORUN"):
            vol, pb = m.audio_and_playback()
        self.assertEqual(vol, 19)
        self.assertEqual(pb, {})

    def test_paused_is_reported_as_not_playing(self):
        out = "10\npaused\nt\na\nspotify:track:x\n1.0\n1000"
        with self._with_osa(out):
            _, pb = m.audio_and_playback()
        self.assertEqual(pb["sp_playing"], "0")

    def test_a_newline_in_a_track_name_is_flattened(self):
        # Would otherwise forge a field on the wire.
        out = "10\nplaying\nbad\nname\na\nspotify:track:x\n1.0\n1000"
        with self._with_osa(out):
            _, pb = m.audio_and_playback()
        self.assertNotIn("\n", pb.get("sp_track", ""))

    def test_a_total_failure_reads_as_unknown_not_zero(self):
        # -1, never 0: the device renders unknown as unknown, and a confident
        # zero would draw an empty slider for a value nobody measured.
        with mock.patch.object(m, "osa", return_value=None):
            vol, pb = m.audio_and_playback()
        self.assertEqual(vol, -1)
        self.assertEqual(pb, {})


if __name__ == "__main__":
    unittest.main()
