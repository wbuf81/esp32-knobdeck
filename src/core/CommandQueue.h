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
};

struct Command {
  CommandType type = CommandType::None;
  int arg = 0;
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
  void pushCoalesced(Command c) {
    for (int i = tail_; i != head_; i = (i + 1) % CAPACITY) {
      if (buf_[i].type == c.type) {
        buf_[i] = c;
        return;
      }
    }
    push(c);
  }

  bool empty() const { return head_ == tail_; }

 private:
  Command buf_[CAPACITY];
  int head_ = 0;
  int tail_ = 0;
};
