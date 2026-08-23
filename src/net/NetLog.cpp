#include "NetLog.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace netlog {

void line(const char *fmt, ...) {
  // Prefix and body are combined BEFORE anything is written, so a net line
  // cannot be split around another task's output.
  char buf[480];
  std::strcpy(buf, "[net] ");
  const size_t off = std::strlen(buf);

  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf + off, sizeof(buf) - off, fmt, ap);
  va_end(ap);

  corelog::line("%s", buf);
}

}  // namespace netlog
