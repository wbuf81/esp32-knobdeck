#include "Route.h"

#include <cstdio>

#include "shell/GestureFlash.h"

namespace input {

Action routePlayer(Gesture g, const PlaybackState &pb) {
  Action a;
  const bool transport =
      shell::transportFeedbackVisible(pb.has_track, pb.has_device);
  // Inside the player screen, "no track" is exactly "the dog is on screen".
  const bool dog = !pb.has_track;

  switch (g) {
    case Gesture::Tap:
      // She lifts her head. With nothing listening this is the whole response:
      // no glyph promising a transport that is not there, and no request that
      // could only come back 404.
      if (dog) {
        a.poke_dog = true;
        a.dog = views::DaisyIdle::Reaction::Touch;
      }
      if (!transport) return a;
      a.flip_playing = true;
      a.command = CommandType::PlayPause;
      // The glyph shows the state you are NOW IN, not the button you pressed -
      // the same convention as every transport control. `pb` is the pre-flip
      // snapshot, so playing means we are about to pause.
      a.glyph = pb.is_playing ? shell::Glyph::Pause : shell::Glyph::Play;
      a.show_glyph = true;
      return a;

    case Gesture::SwipeLeft:
    case Gesture::SwipeRight:
      if (dog) {
        a.poke_dog = true;
        a.dog = views::DaisyIdle::Reaction::Swipe;
      }
      a.haptic_bump = true;
      if (!transport) return a;
      a.command =
          g == Gesture::SwipeLeft ? CommandType::Previous : CommandType::Next;
      a.glyph = g == Gesture::SwipeLeft ? shell::Glyph::Previous
                                       : shell::Glyph::Next;
      a.show_glyph = true;
      return a;

    case Gesture::LongPress:
      a.haptic_bump = true;
      if (!pb.has_track) {
        // The refusal is still owed: a refusal the user cannot see is the same
        // as a bug, and that is the whole reason this branch exists. But a
        // crossed-out heart only refuses something SPECIFIC - with nothing
        // listening there is no track to have refused, so the zoomies are the
        // honest answer and the glyph would be noise.
        if (dog) {
          a.poke_dog = true;
          a.dog = views::DaisyIdle::Reaction::Hold;
        }
        if (transport) {
          a.glyph = shell::Glyph::HeartSlash;
          a.show_glyph = true;
        }
        std::snprintf(a.toast, sizeof(a.toast), "Nothing playing");
        return a;
      }
      a.flip_liked = true;
      a.command = CommandType::ToggleLike;
      a.glyph =
          pb.liked ? shell::Glyph::HeartOutline : shell::Glyph::HeartFilled;
      a.show_glyph = true;
      return a;

    case Gesture::SwipeDown:
      a.haptic_bump = true;
      if (!pb.has_track) {
        // The queue is what comes up AFTER something. With no current track
        // there is nothing for it to come after, so this used to jump to a list
        // that was always empty and shove the dog off screen to do it.
        a.poke_dog = true;
        a.dog = views::DaisyIdle::Reaction::Swipe;
        return a;
      }
      // Straight to what is coming up, without going through the chooser.
      a.screen = Screen::Tracks;
      a.command = CommandType::FetchQueue;
      a.glyph = shell::Glyph::ChevronDown;
      a.show_glyph = true;
      return a;

    case Gesture::SwipeUp:
      // NOT gated on a track, deliberately. See the header.
      a.haptic_bump = true;
      a.screen = Screen::Playlists;
      a.command = CommandType::FetchPlaylists;
      a.glyph = shell::Glyph::ChevronUp;
      a.show_glyph = true;
      return a;

    case Gesture::None:
    default:
      return a;
  }
}

}  // namespace input
