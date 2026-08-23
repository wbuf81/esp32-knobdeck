#include "Fft.h"

#include <cmath>

namespace audio {

Fft::Fft() {
  for (int i = 0; i < N / 2; ++i) {
    const double a = -2.0 * M_PI * i / N;
    cos_[i] = static_cast<float>(std::cos(a));
    sin_[i] = static_cast<float>(std::sin(a));
  }
  // Hann. Without a window, a tone that does not land exactly on a bin centre
  // smears across the whole spectrum, which makes every band read as loud and
  // the bands stop distinguishing anything.
  for (int i = 0; i < N; ++i)
    win_[i] =
        static_cast<float>(0.5 * (1.0 - std::cos(2.0 * M_PI * i / (N - 1))));

  int bits = 0;
  while ((1 << bits) < N) ++bits;
  for (int i = 0; i < N; ++i) {
    unsigned r = 0;
    for (int b = 0; b < bits; ++b)
      if (i & (1 << b)) r |= 1u << (bits - 1 - b);
    rev_[i] = static_cast<uint16_t>(r);
  }
}

void Fft::magnitudes(const float *in, float *out_mag) {
  for (int i = 0; i < N; ++i) {
    re_[rev_[i]] = in[i] * win_[i];
    im_[rev_[i]] = 0.0f;
  }
  for (int len = 2; len <= N; len <<= 1) {
    const int half = len >> 1;
    const int step = N / len;
    for (int i = 0; i < N; i += len) {
      for (int j = 0; j < half; ++j) {
        const int t = j * step;
        const float wr = cos_[t], wi = sin_[t];
        const int p = i + j, q = p + half;
        const float xr = re_[q] * wr - im_[q] * wi;
        const float xi = re_[q] * wi + im_[q] * wr;
        re_[q] = re_[p] - xr;
        im_[q] = im_[p] - xi;
        re_[p] += xr;
        im_[p] += xi;
      }
    }
  }
  for (int i = 0; i < BINS; ++i)
    out_mag[i] = std::sqrt(re_[i] * re_[i] + im_[i] * im_[i]);
}

}  // namespace audio
