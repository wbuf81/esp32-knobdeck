#pragma once

// FNV-1a, the project's one string hash.
//
// Everything that wants a stable per-track choice hashes the track id through
// this. Ported unchanged from the ancestor project so a given song lands on
// comparable choices in both.

#include <cstdint>

inline uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  while (s && *s) {
    h ^= static_cast<uint8_t>(*s++);
    h *= 16777619u;
  }
  return h;
}
