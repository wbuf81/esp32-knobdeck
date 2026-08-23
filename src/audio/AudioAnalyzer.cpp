#include "AudioAnalyzer.h"

#include <cmath>
#include <cstring>

namespace audio {

void AudioAnalyzer::begin(MicSource *mic) {
  mic_ = mic;
  filled_ = 0;
  std::memset(ring_, 0, sizeof(ring_));
  bands_.reset();
  onset_.reset();
  tempo_.reset();
  mix_ = 0.0f;
  quiet_s_ = QUIET_HOLD_S;
  rms_ = 0.0f;
  rms_peak_ = 0.0f;
}

void AudioAnalyzer::setTrack(uint32_t track_seed) {
  seed_ = track_seed;
  fallback_.reseed(track_seed);
}

void AudioAnalyzer::runHop() {
  fft_.magnitudes(ring_, mag_);
  // dt here is the hop period, not the frame period: the band smoothing is
  // defined per second and the hops arrive at their own fixed rate.
  const float hop_dt = static_cast<float>(HOP) / static_cast<float>(SAMPLE_RATE);
  bands_.process(mag_, Fft::BINS, hop_dt, &live_bass_, &live_mid_, &live_treble_,
                 &live_loud_);
  if (onset_.process(mag_, Fft::BINS)) live_onset_ = true;
}

void AudioAnalyzer::update(Modulation *m, float dt) {
  if (!m) return;

  live_onset_ = false;
  int hops = 0;

  if (mic_) {
    while (hops < MAX_HOPS_PER_FRAME) {
      const int got = mic_->read(scratch_, HOP);
      if (got < HOP) break;  // not a whole hop yet

      // Shift the window down by one hop and append.
      std::memmove(ring_, ring_ + HOP, (Fft::N - HOP) * sizeof(float));
      std::memcpy(ring_ + Fft::N - HOP, scratch_, HOP * sizeof(float));
      if (filled_ < Fft::N) filled_ += HOP;

      double sq = 0.0;
      for (int i = 0; i < HOP; ++i) sq += scratch_[i] * scratch_[i];
      rms_ = static_cast<float>(std::sqrt(sq / HOP));
      if (rms_ > rms_peak_) rms_peak_ = rms_;  // rises instantly

      if (filled_ >= Fft::N) runHop();
      ++hops;
    }
  }

  // Decide whether the room is worth listening to, from the decaying peak
  // rather than from this instant.
  rms_peak_ *= std::exp(-dt / PEAK_DECAY_S);
  if (rms_peak_ < SILENCE_RMS) {
    quiet_s_ += dt;
  } else {
    quiet_s_ = 0.0f;
  }
  const float target = quiet_s_ >= QUIET_HOLD_S ? 0.0f : 1.0f;
  const float step = dt / CROSSFADE_S;
  if (mix_ < target) {
    mix_ += step;
    if (mix_ > target) mix_ = target;
  } else if (mix_ > target) {
    mix_ -= step;
    if (mix_ < target) mix_ = target;
  }

  // The procedural side always runs, so its oscillators are already in phase
  // whenever the crossfade reaches for them. Starting them at handover would
  // make every fade-in begin from a standstill.
  Modulation proc = *m;
  fallback_.fill(&proc, dt);

  m->bass = proc.bass + (live_bass_ - proc.bass) * mix_;
  m->mid = proc.mid + (live_mid_ - proc.mid) * mix_;
  m->treble = proc.treble + (live_treble_ - proc.treble) * mix_;
  m->loudness = proc.loudness + (live_loud_ - proc.loudness) * mix_;

  // Onset is a boolean, so it is switched at the midpoint rather than blended:
  // a half-beat is not a meaningful thing to report, and blending it would just
  // produce a beat at every frame near the crossover.
  const bool use_live = mix_ > 0.5f;
  m->onset = use_live ? live_onset_ : proc.onset;

  if (use_live) {
    tempo_.process(live_onset_, dt);
    m->beat_phase = tempo_.beatPhase();
  } else {
    m->beat_phase = proc.beat_phase;
  }
  m->live = use_live;
}

}  // namespace audio
