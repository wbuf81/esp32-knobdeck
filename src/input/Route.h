#pragma once

// What a gesture on the player screen means, as a pure function.
//
// This exists because the rules got subtle and lived somewhere no test could
// reach. main_esp32.cpp is device-only - excluded from both the native and the
// test filters - so every rule in it was verified by flashing and squinting at
// a 360-pixel disc. Two of those rules are now load-bearing and non-obvious:
//
//   - Transport feedback is suppressed when nothing is listening, so a glyph
//     never promises a player that is not there. Gated on has_device rather
//     than on the idle view, because Spotify open with nothing loaded answers
//     /me/player 200 with a null item - has_track false, has_device true - and
//     a play command in that state genuinely can resume.
//
//   - Swipe DOWN is swallowed with no track, but swipe UP is NOT. Up is the
//     only route to Playlists, and Playlists is the only way to start playback
//     or reach THEMES. Making the two symmetric would leave the device unable
//     to start music, precisely when nothing is playing.
//
// That second rule was a comment in a switch. Here it is a test.
//
// Scoped to the player screen deliberately. The list screens keep their inline
// handling; extracting them is worth doing when a fourth screen appears, not
// as a prerequisite for testing the rules that already turned out to be tricky.

#include <cstdint>

#include "core/CommandQueue.h"
#include "core/PlaybackState.h"
#include "input/Gesture.h"
#include "shell/Glyphs.h"
#include "views/DaisyIdle.h"

namespace input {

// Order matches the enum this replaced in main_esp32.cpp, so the 39 existing
// `Screen::X` sites there keep their meaning via a using-declaration.
enum class Screen : uint8_t { Player, Playlists, Tracks, Confirm, Themes, Teams };

// Everything the caller has to do, as data. No side effects here: the whole
// point is that the decision can be made in a test and applied on a device.
struct Action {
  Screen screen = Screen::Player;           // same as current = stay put
  CommandType command = CommandType::None;  // None = send nothing
  shell::Glyph glyph = shell::Glyph::Play;
  bool show_glyph = false;
  views::DaisyIdle::Reaction dog = views::DaisyIdle::Reaction::Touch;
  bool poke_dog = false;
  // Optimistic local edits the caller must apply BEFORE queueing the command,
  // because runCommand picks /play vs /pause and PUT vs DELETE by reading the
  // already-flipped field. Getting that order wrong sent /play while playing
  // and Spotify answered 403 "restriction violated", so tapping did nothing at
  // all and the only evidence was a 403 in a log nobody could read.
  bool flip_playing = false;
  bool flip_liked = false;
  // True means the heavier bump; false with any real gesture means the click.
  bool haptic_bump = false;
  char toast[32] = {};
};

// `pb` carries both has_track and has_device, and the transport rules read both.
Action routePlayer(Gesture g, const PlaybackState &pb);

// The Teams screen: two half-disc buttons, split at the centreline. What the
// caller must do, as data - toggles go to MacLink, not the Spotify queue.
struct TeamsAction {
  bool toggle_mic = false;
  bool toggle_cam = false;
  bool exit_screen = false;  // the manual escape hatch back to the player
};

// tap_x is GestureRecognizer::tapX() - the touch DOWN position, because a
// gesture that slides off a button still belongs to the button it began on.
// Everything except tap-left, tap-right and swipe-down deliberately does
// nothing: mid-meeting is the worst place for a surprise action.
TeamsAction routeTeams(Gesture g, int tap_x);

}  // namespace input
