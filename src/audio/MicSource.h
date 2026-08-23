#pragma once

// The one seam between the analysis chain and where audio comes from.
//
// Desktop reads a WAV; the device reads I2S. Neither the analyzer nor any effect
// knows which - which is what let the entire FFT, band, onset and tempo chain be
// built and tested against fixed synthetic audio before any microphone existed.

#include <cstdint>

namespace audio {

class MicSource {
 public:
  virtual ~MicSource() = default;

  // Fills up to `n` samples in -1..1 and returns how many were written.
  // Returning fewer than asked is normal and means "no more audio right now";
  // it must never block, because it is called from the render loop.
  virtual int read(float *out, int n) = 0;

  virtual void restart() {}
};

}  // namespace audio
