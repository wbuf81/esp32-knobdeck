#pragma once

// Which of Spotify's known devices to wake.
//
// Header-only and free of Arduino, so it lands in the host test build even
// though spotify/ is excluded from it by source filter. The split is the same
// one the rest of this project makes: the JUDGEMENT gets tests, the HTTP
// plumbing around it does not.
//
// The judgement matters because it is the difference between the feature working
// and appearing to do nothing. Spotify's /me/player/devices lists everything it
// remembers, including devices that will refuse the command you are about to
// send.

#include <cstring>

#include "core/PlaybackState.h"  // setStr, and the buffer-not-string convention

namespace spotify {

struct DeviceInfo {
  char id[48] = {};
  char name[48] = {};
  // "Computer", "Smartphone", "Speaker", "TV", ... Spotify's own vocabulary.
  char type[24] = {};
  bool is_active = false;
  // Spotify's flag for a device that does not accept Web API commands. A
  // transfer to one of these fails, so it is not a candidate however well it
  // matches otherwise.
  bool is_restricted = false;
};

// Index of the device to wake, or -1 if none is usable.
//
// Preference order:
//   1. an unrestricted device whose name matches `preferred_name`
//   2. an already-active Computer  - accepts a resume with no transfer at all
//   3. any Computer                - "Spotify is open on my computer"
//   4. the first unrestricted device of any kind
//
// Rule 1 is the two-Macs fix. A live probe of this account found TWO devices of
// type Computer, and when the idle screen is showing neither is active - so
// rules 2 to 4 fall through to Spotify's list order and pick the wrong machine
// about half the time. The Mac link's heartbeat says which machine the knob is
// actually sitting next to, which is the only authority that knows.
//
// A name match does NOT override is_restricted: that flag means the device
// refuses Web API commands, so preferring it would guarantee failure.
//
// -1 rather than 0 when nothing qualifies. Returning an index into a list whose
// only entries refuse commands would send a request guaranteed to fail and then
// report success, where "no devices found" is both true and actionable.
inline int pickDevice(const DeviceInfo *devices, int count,
                      const char *preferred_name = nullptr) {
  if (devices == nullptr || count <= 0) return -1;

  int named = -1;
  int computer = -1;
  int active_computer = -1;
  int any = -1;

  const bool want_name = preferred_name != nullptr && preferred_name[0] != '\0';

  for (int i = 0; i < count; ++i) {
    if (devices[i].is_restricted) continue;
    if (want_name && named < 0 &&
        std::strcmp(devices[i].name, preferred_name) == 0) {
      named = i;
    }
    const bool is_computer = std::strcmp(devices[i].type, "Computer") == 0;
    if (is_computer && devices[i].is_active && active_computer < 0) {
      active_computer = i;
    }
    if (is_computer && computer < 0) computer = i;
    if (any < 0) any = i;
  }

  if (named >= 0) return named;
  if (active_computer >= 0) return active_computer;
  if (computer >= 0) return computer;
  return any;
}

}  // namespace spotify
