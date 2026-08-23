#pragma once

// One serial line at a time.
//
// This exists because of a real and thoroughly confusing failure. The network
// task runs on core 0 and the renderer on core 1, and both logged with several
// printf calls per message. Neither the calls nor the writes are atomic, so the
// two streams interleaved character by character:
//
//     [net]kno b pnins: Ae=1 B=1t
//
// which is a shredded "[net] ..." and "knob pins: A=1 B=1" on top of each other.
// It cost a debugging session, because the log is the only instrument the
// hardware has and a corrupted log does not look corrupted - it looks like the
// message was never printed at all, so grep finds nothing and the obvious
// conclusion is that the code did not run.
//
// Every line is formatted into a buffer first and written once, under a lock.

#include <cstdarg>

namespace corelog {

// Formats and writes one line atomically. A trailing newline is added.
void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vline(const char *fmt, va_list ap);

}  // namespace corelog

#define LOGF(...) ::corelog::line(__VA_ARGS__)
