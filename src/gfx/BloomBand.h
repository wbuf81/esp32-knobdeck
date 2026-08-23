#pragma once

// Bloom for a band renderer, one frame late.
//
// Bloom fundamentally wants the whole frame: its blur reads neighbours, and the
// glow must be added on top of what produced it. A band renderer never holds a
// whole frame, and holding one in PSRAM to make it possible costs about 15 ms
// per frame in read-modify-write on this board, measured - half the budget for
// one effect.
//
// So the glow is one frame late. Each band contributes its bright-pass to a
// resident accumulator as it is composited, and the glow a band receives comes
// from that accumulator blurred at the END of the previous frame. At 30 fps that
// is 33 ms of latency on a soft halo: not perceptible, and it makes it fit.
//
// This class does NOT own an add pass. An earlier version did and it cost 84 ms
// of a 115 ms frame, because a separate read-modify-write over every pixel is
// the most expensive thing this renderer can do. Instead it hands out an
// expanded glow row and the caller folds it into a write it was already making.
//
// Three rules came out of measuring this file, all of them the hard way:
//   * never add a pass that reads and writes every pixel;
//   * never do per-pixel work that could be done per row, or per fourth row;
//   * one aligned 32-bit access beats three byte accesses, every time.
//
// A 40-row band is exactly 10 rows of the 90x90, and 360/4 is exactly 90
// columns, which is why the band height is 40: an accumulator straddling a band
// boundary would need its own seam handling.

#include <cstdint>

#include "Surface.h"

namespace gfx {

class BloomBand {
 public:
  static constexpr int SW = 90;
  static constexpr int SH = 90;
  static constexpr int SCALE = W / SW;  // 4
  static constexpr int BAND_H = 40;
  static constexpr int PASSES = 3;
  static constexpr int N = SW * SH;

  void begin();

  void setThreshold(uint8_t t) { threshold_ = t; }
  // 0 disables the glow entirely, which the harness uses to prove it is really
  // contributing rather than being drawn over.
  void setStrength(uint8_t s) { strength_ = s; }
  uint8_t strength() const { return strength_; }

  // A full-width row of glow, packed RGB565, strength already applied - so the
  // caller adds it with one saturating add and never unpacks anything.
  //
  // Cached per SMALL row, so it is rebuilt 90 times a frame rather than 360.
  const uint16_t *glowRow(int screen_y);

  // Read-only bright-pass contribution for next frame. Subsampled every other
  // pixel in both axes - a quarter of the reads - because this feeds a blur at
  // a sixteenth of the resolution and cannot tell the difference.
  void accumulateBand(const Surface &s);

  // Blur the accumulator into what the next frame reads, then clear it.
  void endFrame();

 private:
  void blurPass();
  void buildGlowRow(int small_y);

  // One pixel per uint32, 0x00BBGGRR, deliberately NOT three packed bytes.
  //
  // The blur is the reason. As 3-byte pixels it needed nine byte loads, three
  // byte stores and a multiply per index, and measured 15.8 ms per frame. A
  // 4-byte stride makes every access an aligned 32-bit load and lets the blur
  // run as SWAR across all three channels at once. The wasted byte per pixel is
  // 8 KB; the alternative was a third of the frame budget.
  uint32_t glow_[N] = {};   // blurred, from last frame
  uint32_t accum_[N] = {};  // building, from this frame's bands; doubles as the
                            // blur's scratch, since it is cleared afterwards
                            // anyway - which is what pays for the wider stride
  uint16_t glow565_[N] = {};  // packed once per frame, for row expansion
  uint32_t acc_[SW] = {};     // one small row, full precision, same layout
  uint16_t glowrow_[W] = {};  // expanded and packed, cached
  int glowrow_sy_ = -1;
  uint8_t threshold_ = 100;
  uint8_t strength_ = 150;
};

}  // namespace gfx
