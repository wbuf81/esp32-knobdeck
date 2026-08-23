#pragma once

// 512-point radix-2 complex FFT, used on real input with the imaginary part
// zeroed.
//
// A real-input transform packing two samples per complex bin would halve the
// work, and is deliberately not used: this measures about 2 ms on the target,
// and an FFT that is subtly wrong produces a visualiser that is subtly
// unconvincing - which is close to impossible to debug by eye. Correctness here
// is worth a millisecond.
//
// Sample rate is fixed at 16 kHz, so the 257 bins span 0-8 kHz at 31.25 Hz
// each: plenty for three bands and cheap to capture.

#include <cstdint>

namespace audio {

constexpr int SAMPLE_RATE = 16000;

class Fft {
 public:
  static constexpr int N = 512;
  static constexpr int BINS = N / 2 + 1;
  static constexpr float BIN_HZ = static_cast<float>(SAMPLE_RATE) / N;

  Fft();  // precomputes twiddles, the Hann window and the bit-reversal table

  // Applies the Hann window to `in` (N samples, nominally -1..1) and writes
  // BINS magnitudes to `out_mag`.
  void magnitudes(const float *in, float *out_mag);

 private:
  float cos_[N / 2], sin_[N / 2], win_[N];
  uint16_t rev_[N];
  float re_[N], im_[N];
};

}  // namespace audio
