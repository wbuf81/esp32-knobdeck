#pragma once

// A MicSource backed by a WAV file, so the analysis chain can be developed and
// regression-tested against fixed audio rather than by making noises at
// hardware.

#include <cstdint>
#include <string>
#include <vector>

#include "audio/MicSource.h"

namespace desktop {

class WavMic : public audio::MicSource {
 public:
  // 16-bit PCM mono. Chunks are walked rather than assumed at fixed offsets,
  // because files from different tools carry different chunk layouts and a
  // hard-coded offset works until it silently does not.
  bool open(const char *path);

  // Paces playback so a fixed number of samples per second is handed out
  // regardless of frame rate, which is what keeps a headless run deterministic.
  void advance(float dt);

  int read(float *out, int n) override;
  void restart() override { pos_ = 0; avail_ = 0; }

  bool loaded() const { return !samples_.empty(); }
  int sampleRate() const { return rate_; }

 private:
  std::vector<float> samples_;
  size_t pos_ = 0;
  int rate_ = 16000;
  float avail_ = 0.0f;  // fractional samples owed to the caller
};

}  // namespace desktop
