#include "WavMic.h"

#include <cstdio>
#include <cstring>

namespace desktop {
namespace {

uint32_t rd32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t rd16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

bool WavMic::open(const char *path) {
  samples_.clear();
  pos_ = 0;
  avail_ = 0.0f;
  if (!path) return false;

  FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (len < 44) {
    std::fclose(f);
    return false;
  }
  std::vector<uint8_t> d(static_cast<size_t>(len));
  const size_t got = std::fread(d.data(), 1, d.size(), f);
  std::fclose(f);
  if (got != d.size()) return false;

  if (std::memcmp(d.data(), "RIFF", 4) != 0 ||
      std::memcmp(d.data() + 8, "WAVE", 4) != 0)
    return false;

  int channels = 1, bits = 16;
  size_t off = 12;
  const uint8_t *data = nullptr;
  size_t data_len = 0;
  while (off + 8 <= d.size()) {
    const char *id = reinterpret_cast<const char *>(d.data() + off);
    const uint32_t sz = rd32(d.data() + off + 4);
    const size_t body = off + 8;
    if (body + sz > d.size()) break;
    if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
      channels = rd16(d.data() + body + 2);
      rate_ = static_cast<int>(rd32(d.data() + body + 4));
      bits = rd16(d.data() + body + 14);
    } else if (std::memcmp(id, "data", 4) == 0) {
      data = d.data() + body;
      data_len = sz;
    }
    off = body + sz + (sz & 1);  // chunks are word-aligned
  }
  if (!data || bits != 16 || channels < 1) return false;

  const size_t frames = data_len / (2u * static_cast<size_t>(channels));
  samples_.reserve(frames);
  for (size_t i = 0; i < frames; ++i) {
    // Mono-mix, so a stereo fixture still works rather than playing at half
    // speed with alternating channels.
    int acc = 0;
    for (int c = 0; c < channels; ++c) {
      const size_t k = (i * channels + c) * 2;
      acc += static_cast<int16_t>(rd16(data + k));
    }
    samples_.push_back(static_cast<float>(acc) /
                       (32768.0f * static_cast<float>(channels)));
  }
  return !samples_.empty();
}

void WavMic::advance(float dt) {
  avail_ += dt * static_cast<float>(rate_);
}

int WavMic::read(float *out, int n) {
  if (samples_.empty() || !out || n <= 0) return 0;
  if (avail_ < static_cast<float>(n)) return 0;
  for (int i = 0; i < n; ++i) {
    out[i] = samples_[pos_];
    // Loops, so a short fixture can drive a long run.
    if (++pos_ >= samples_.size()) pos_ = 0;
  }
  avail_ -= static_cast<float>(n);
  return n;
}

}  // namespace desktop
