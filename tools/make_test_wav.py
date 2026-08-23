#!/usr/bin/env python3
"""Generate the synthetic audio fixtures the analysis tests run against.

Dependency-free by design: struct and math, no numpy or scipy for four short
mono files. All 16-bit PCM, mono, 16 kHz - matching audio::SAMPLE_RATE, because
the analysis chain assumes it and resampling in the fixture generator would just
hide a mismatch.

  python3 tools/make_test_wav.py assets/audio
"""

import math
import os
import random
import struct
import sys

RATE = 16000


def write_wav(path, samples):
    n = len(samples)
    data = b"".join(struct.pack("<h", max(-32768, min(32767, int(s * 32767))))
                    for s in samples)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    open(path, "wb").write(hdr + data)
    print("  %-16s %6.2fs" % (os.path.basename(path), n / RATE))


def silence(secs):
    return [0.0] * int(RATE * secs)


def sine(secs, hz, amp=0.5):
    return [amp * math.sin(2 * math.pi * hz * i / RATE)
            for i in range(int(RATE * secs))]


def clicks(secs, bpm, amp=0.9, burst_ms=5):
    """Noise bursts on the beat: broadband transients, which is what onset
    detection is actually looking for. A click train of pure tones would test
    almost nothing, since spectral flux keys on broadband change."""
    rnd = random.Random(1234)
    n = int(RATE * secs)
    out = [0.0] * n
    period = int(RATE * 60.0 / bpm)
    burst = int(RATE * burst_ms / 1000.0)
    for start in range(0, n, period):
        for i in range(burst):
            if start + i < n:
                env = 1.0 - i / burst
                out[start + i] = amp * env * (rnd.random() * 2 - 1)
    return out


def sweep(secs, lo=40.0, hi=8000.0, amp=0.5):
    n = int(RATE * secs)
    out = []
    phase = 0.0
    for i in range(n):
        t = i / n
        f = lo * (hi / lo) ** t
        phase += 2 * math.pi * f / RATE
        out.append(amp * math.sin(phase))
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "assets/audio"
    os.makedirs(outdir, exist_ok=True)
    print("writing fixtures to %s" % outdir)
    write_wav(os.path.join(outdir, "silence.wav"), silence(3.0))
    write_wav(os.path.join(outdir, "sine440.wav"), sine(3.0, 440.0))
    write_wav(os.path.join(outdir, "clicks120.wav"), clicks(8.0, 120.0))
    write_wav(os.path.join(outdir, "sweep.wav"), sweep(4.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
