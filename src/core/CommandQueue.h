#pragma once

// Commands travel from the UI task to the net task.
//
// On hardware this becomes a FreeRTOS queue; the emulator drains it inline on a
// single thread. The interface is the same either way, so the threading change
// does not ripple into callers. Fixed-capacity ring buffer, no allocation.

#include <cstdint>

enum class CommandType : uint8_t {
  None,
  PlayPause,
  Next,
  Previous,
  SetVolume,   // arg = 0..100
  ToggleLike,
  // Browser. These are requests from the UI to the net task; results land in
  // spotify::Library and are picked up through its generation counter.
  FetchPlaylists,
  FetchTracks,      // uri = playlist uri, text = its name
  FetchQueue,       // what is playing and what is coming up
  PlayFromContext,  // uri = playlist uri, arg = track offset within it
  // Jump to an entry in UP NEXT. uri = the track, arg = how many rows below
  // the one playing it sits. Spotify has no play-queue-index call, so the net
  // task needs both: the uri for the one-shot context jump, and the row count
  // for the skip-forward fallback when that is refused.
  PlayQueueItem,
  // Ask Spotify to hand playback back to a device it has quietly deregistered.
  // The common case is a computer with Spotify open but idle: /me/player then
  // answers 204, every transport command 404s NO_ACTIVE_DEVICE, and the only
  // way back is to transfer playback onto it explicitly.
  WakeDevice,
};

struct Command {
  CommandType type = CommandType::None;
  int arg = 0;
  // When the UI handed this over, for measuring how long it then waited.
  //
  // A queue fetch was once seen to take about six seconds against a measured
  // 750ms request, and there was no way to tell which half was slow: the wait
  // for the net task to pick the command up, or the request itself. Stamped on
  // submit and reported on execution when the gap is large enough to matter.
  uint32_t submitted_ms = 0;
  // A URI and a display name, for the browser commands.
  //
  // Fixed buffers rather than std::string: this struct crosses a FreeRTOS queue
  // between two tasks, and the ancestor's rule about avoiding per-command
  // allocation on a fragmenting heap applies here more than anywhere.
  char uri[52] = {};
  char text[52] = {};
};

template <int CAPACITY = 8>
class CommandQueue {
 public:
  bool push(Command c) {
    const int next = (head_ + 1) % CAPACITY;
    if (next == tail_) return false;  // full: drop rather than block
    buf_[head_] = c;
    head_ = next;
    return true;
  }

  bool pop(Command *out) {
    if (tail_ == head_) return false;
    *out = buf_[tail_];
    tail_ = (tail_ + 1) % CAPACITY;
    return true;
  }

  // Replaces any pending command of this type. Used to coalesce volume changes
  // during a long-press so only the final value is sent.
  //
  // Returns false only when there was nothing to coalesce with AND the ring was
  // full. This used to return void and throw away push()'s answer, which made
  // it the quietest drop in the codebase: a volume command arriving with
  // nothing of its type pending and no room left simply vanished. Same bug as
  // submit()'s void signature, one level further down.
  bool pushCoalesced(Command c) {
    for (int i = tail_; i != head_; i = (i + 1) % CAPACITY) {
      if (buf_[i].type == c.type) {
        buf_[i] = c;
        return true;
      }
    }
    return push(c);
  }

  bool empty() const { return head_ == tail_; }

 private:
  Command buf_[CAPACITY];
  int head_ = 0;
  int tail_ = 0;
};
