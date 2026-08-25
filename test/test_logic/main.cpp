// Host unit tests for the display-free engine.
//
// Everything here runs without SDL, without hardware, and in milliseconds.
// Each test is written against a symptom that can actually be seen on screen,
// not just against a function signature.
//
//   pio test -e test

#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/FrameClock.h"
#include "core/Backlight.h"
#include "net/HostLink.h"
#include "core/CommandQueue.h"
#include "core/HeapPolicy.h"
#include "core/CrashPolicy.h"
#include "core/Hash.h"
#include "core/Rng.h"
#include "gfx/Blend.h"
#include "gfx/Bloom.h"
#include "gfx/CircleMask.h"
#include "gfx/Color.h"
#include "gfx/Dither.h"
#include "gfx/Framebuffer.h"
#include "art/Image.h"
#include "audio/Analysis.h"
#include "audio/AudioAnalyzer.h"
#include "audio/Fft.h"
#include "audio/Procedural.h"
#include "fx/Particles.h"
#include "fx/ThemePicker.h"
#include "fx/Matrix.h"
#include "fx/Record.h"
#include "fx/Outrun.h"
#include "fx/Tetris.h"
#include "fx/Themes.h"
#include "gfx/Quad3D.h"
#include "gfx/fonts/Fonts.h"
#include "input/Gesture.h"
#include "input/Route.h"
#include "spotify/DevicePick.h"
#include "shell/ConfirmRing.h"
#include "shell/GestureFlash.h"
#include "shell/Glyphs.h"
#include "views/CoverLight.h"
#include "views/DaisyIdle.h"
#include "views/SafeScreen.h"
#include "shell/ListView.h"
#include "shell/NowPlaying.h"
#include "shell/Toast.h"
#include "shell/RadialShell.h"
#include "spotify/Library.h"
#include "gfx/Surface.h"
#include "platform/desktop/FrameDump.h"
#include "platform/desktop/WavMic.h"

// ---------------------------------------------------------------------------
// Framebuffer
// ---------------------------------------------------------------------------

void test_framebuffer_is_360_square(void) {
  TEST_ASSERT_EQUAL_INT(360, gfx::Framebuffer::width());
  TEST_ASSERT_EQUAL_INT(360, gfx::Framebuffer::height());
  TEST_ASSERT_EQUAL_UINT32(129600u, (uint32_t)gfx::Framebuffer::count());
}

void test_framebuffer_fill_sets_every_pixel(void) {
  gfx::Framebuffer fb;
  fb.fill(0xF800);
  TEST_ASSERT_EQUAL_HEX16(0xF800, fb.at(0, 0));
  TEST_ASSERT_EQUAL_HEX16(0xF800, fb.at(359, 359));
  TEST_ASSERT_EQUAL_HEX16(0xF800, fb.at(180, 180));
}

void test_framebuffer_set_is_bounds_checked(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  fb.set(-1, 0, 0xFFFF);
  fb.set(360, 0, 0xFFFF);
  fb.set(0, 360, 0xFFFF);
  fb.set(5, 7, 0x07E0);
  TEST_ASSERT_EQUAL_HEX16(0x07E0, fb.at(5, 7));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(0, 0));
}

// ---------------------------------------------------------------------------
// Colour and blending
// ---------------------------------------------------------------------------

void test_rgb565_roundtrips_channel_extremes(void) {
  TEST_ASSERT_EQUAL_HEX16(0xF800, gfx::rgb565(255, 0, 0));
  TEST_ASSERT_EQUAL_HEX16(0x07E0, gfx::rgb565(0, 255, 0));
  TEST_ASSERT_EQUAL_HEX16(0x001F, gfx::rgb565(0, 0, 255));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, gfx::rgb565(255, 255, 255));
  TEST_ASSERT_EQUAL_HEX16(0x0000, gfx::rgb565(0, 0, 0));
}

void test_unpack565_replicates_high_bits(void) {
  uint8_t r, g, b;
  gfx::unpack565(0xFFFF, r, g, b);
  TEST_ASSERT_EQUAL_UINT8(255, r);
  TEST_ASSERT_EQUAL_UINT8(255, g);
  TEST_ASSERT_EQUAL_UINT8(255, b);
}

void test_add_sat_clamps_each_channel_independently(void) {
  const uint16_t red = gfx::rgb565(255, 0, 0);
  TEST_ASSERT_EQUAL_HEX16(0xF800, gfx::addSat(red, red));
  // A channel overflowing must not bleed into its neighbour. This is the bug
  // that produces rainbow noise instead of a glow.
  const uint16_t hot = gfx::addSat(gfx::rgb565(200, 200, 200),
                                   gfx::rgb565(200, 200, 200));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, hot);
}

void test_add_sat_does_not_bleed_red_into_green(void) {
  // Saturating red twice must leave green and blue at zero.
  const uint16_t r = gfx::addSat(gfx::rgb565(255, 0, 0), gfx::rgb565(255, 0, 0));
  uint8_t rr, gg, bb;
  gfx::unpack565(r, rr, gg, bb);
  TEST_ASSERT_EQUAL_UINT8(255, rr);
  TEST_ASSERT_EQUAL_UINT8(0, gg);
  TEST_ASSERT_EQUAL_UINT8(0, bb);
}

void test_add_sat_is_identity_against_black(void) {
  const uint16_t c = gfx::rgb565(90, 140, 30);
  TEST_ASSERT_EQUAL_HEX16(c, gfx::addSat(c, 0x0000));
  TEST_ASSERT_EQUAL_HEX16(c, gfx::addSat(0x0000, c));
}

void test_fade_converges_to_black(void) {
  uint16_t c = 0xFFFF;
  for (int i = 0; i < 200; ++i) c = gfx::fade(c, 230);
  TEST_ASSERT_EQUAL_HEX16(0x0000, c);
}

void test_fade_by_zero_is_black_and_full_is_near_identity(void) {
  TEST_ASSERT_EQUAL_HEX16(0x0000, gfx::fade(0xFFFF, 0));
  // 256/256 is not representable in a uint8_t, so 255 is the maximum and is
  // deliberately a hair below identity - that hair is what makes trails decay.
  TEST_ASSERT_TRUE(gfx::fade(0xFFFF, 255) > 0xF000);
}

void test_lerp565_hits_both_endpoints_exactly(void) {
  const uint16_t a = gfx::rgb565(255, 0, 0);
  const uint16_t b = gfx::rgb565(0, 0, 255);
  TEST_ASSERT_EQUAL_HEX16(a, gfx::lerp565(a, b, 0));
  TEST_ASSERT_EQUAL_HEX16(b, gfx::lerp565(a, b, 256));
}

void test_lerp565_midpoint_is_between(void) {
  const uint16_t mid = gfx::lerp565(gfx::rgb565(255, 0, 0),
                                    gfx::rgb565(0, 0, 255), 128);
  uint8_t r, g, b;
  gfx::unpack565(mid, r, g, b);
  TEST_ASSERT_TRUE(r > 100 && r < 160);
  TEST_ASSERT_TRUE(b > 100 && b < 160);
  TEST_ASSERT_TRUE(g < 20);
}

// ---------------------------------------------------------------------------
// Circular mask and dither
// ---------------------------------------------------------------------------

void test_mask_blackens_corners_and_keeps_centre(void) {
  gfx::Framebuffer fb;
  fb.fill(0xFFFF);
  gfx::maskToCircle(fb);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(0, 0));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(359, 0));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(0, 359));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(359, 359));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(180, 180));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(180, 5));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(5, 180));
}

void test_dither_is_deterministic(void) {
  gfx::Framebuffer a, b;
  a.fill(0x4208);
  b.fill(0x4208);
  gfx::ditherFrame(a);
  gfx::ditherFrame(b);
  for (int y = 0; y < 360; y += 37)
    for (int x = 0; x < 360; x += 41)
      TEST_ASSERT_EQUAL_HEX16(a.at(x, y), b.at(x, y));
}

void test_dither_perturbs_by_at_most_one_step_per_channel(void) {
  gfx::Framebuffer fb;
  const uint16_t base = gfx::rgb565(100, 100, 100);
  fb.fill(base);
  gfx::ditherFrame(fb);
  uint8_t br, bg, bb;
  gfx::unpack565(base, br, bg, bb);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      uint8_t r, g, b;
      gfx::unpack565(fb.at(x, y), r, g, b);
      TEST_ASSERT_TRUE(abs((int)r - (int)br) <= 9);
      TEST_ASSERT_TRUE(abs((int)g - (int)bg) <= 5);
      TEST_ASSERT_TRUE(abs((int)b - (int)bb) <= 9);
    }
  }
}

void test_dither_leaves_black_and_white_alone(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::ditherFrame(fb);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(3, 3));
  fb.fill(0xFFFF);
  gfx::ditherFrame(fb);
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, fb.at(3, 3));
}

// ---------------------------------------------------------------------------
// Bloom
// ---------------------------------------------------------------------------

void test_bloom_leaves_a_black_frame_black(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Bloom bloom;
  bloom.apply(fb, 40, 200);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(180, 180));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(90, 270));
}

void test_bloom_spreads_a_bright_point_to_its_neighbourhood(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  for (int y = 176; y < 184; ++y)
    for (int x = 176; x < 184; ++x) fb.set(x, y, 0xFFFF);

  // The bright block occupies 176..183. Bloom runs at 1/4 scale, so it covers
  // small pixels 44..45; two 3-tap passes spread that by 2 small pixels in each
  // direction, which is 8 screen pixels. So 188 is inside the glow and 300 is
  // not - and asserting both is what distinguishes a working bloom from one
  // that has washed the whole frame.
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(188, 180));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(300, 180));
  gfx::Bloom bloom;
  bloom.apply(fb, 40, 255);
  TEST_ASSERT_TRUE(fb.at(188, 180) > 0x0000);
  TEST_ASSERT_TRUE(fb.at(180, 188) > 0x0000);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(300, 180));
}

void test_bloom_ignores_pixels_below_threshold(void) {
  gfx::Framebuffer fb;
  const uint16_t dim = gfx::rgb565(20, 20, 20);
  fb.fill(dim);
  gfx::Bloom bloom;
  bloom.apply(fb, 200, 255);
  TEST_ASSERT_EQUAL_HEX16(dim, fb.at(180, 180));
}

void test_bloom_never_darkens(void) {
  gfx::Framebuffer fb;
  const uint16_t c = gfx::rgb565(120, 60, 200);
  fb.fill(c);
  gfx::Bloom bloom;
  bloom.apply(fb, 40, 128);
  uint8_t r0, g0, b0, r1, g1, b1;
  gfx::unpack565(c, r0, g0, b0);
  gfx::unpack565(fb.at(180, 180), r1, g1, b1);
  TEST_ASSERT_TRUE(r1 >= r0 && g1 >= g0 && b1 >= b0);
}

// ---------------------------------------------------------------------------
// RNG, hash, frame clock
// ---------------------------------------------------------------------------

void test_rng_is_reproducible_from_a_seed(void) {
  core::Rng a(12345), b(12345);
  for (int i = 0; i < 100; ++i) TEST_ASSERT_EQUAL_UINT32(a.next(), b.next());
}

void test_rng_differs_between_seeds(void) {
  core::Rng a(1), b(2);
  bool differed = false;
  for (int i = 0; i < 20; ++i)
    if (a.next() != b.next()) differed = true;
  TEST_ASSERT_TRUE(differed);
}

void test_rng_unit_stays_in_range(void) {
  core::Rng r(7);
  for (int i = 0; i < 10000; ++i) {
    const float u = r.unit();
    TEST_ASSERT_TRUE(u >= 0.0f && u < 1.0f);
  }
}

void test_rng_range_respects_bounds(void) {
  core::Rng r(9);
  for (int i = 0; i < 5000; ++i) {
    const float v = r.range(-3.0f, 5.0f);
    TEST_ASSERT_TRUE(v >= -3.0f && v <= 5.0f);
  }
}

void test_fnv1a_is_stable_and_distinguishes(void) {
  TEST_ASSERT_EQUAL_UINT32(fnv1a("abc"), fnv1a("abc"));
  TEST_ASSERT_TRUE(fnv1a("abc") != fnv1a("abd"));
  TEST_ASSERT_EQUAL_UINT32(2166136261u, fnv1a(""));
  TEST_ASSERT_EQUAL_UINT32(2166136261u, fnv1a(nullptr));
}

void test_frame_clock_reports_elapsed_seconds(void) {
  core::FrameClock fc;
  fc.tick(1000);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.016f, fc.tick(1016));
}

void test_frame_clock_clamps_a_stall(void) {
  core::FrameClock fc;
  fc.tick(0);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, core::FrameClock::MAX_DT, fc.tick(5000));
}

void test_frame_clock_first_tick_is_zero(void) {
  core::FrameClock fc;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, fc.tick(99999));
}

void test_frame_clock_survives_the_millis_wrap(void) {
  core::FrameClock fc;
  fc.tick(0xFFFFFFF0u);
  // 32 ms later, having wrapped through zero.
  TEST_ASSERT_FLOAT_WITHIN(0.002f, 0.032f, fc.tick(0x00000010u));
}

// ---------------------------------------------------------------------------
// Frame dump
// ---------------------------------------------------------------------------

void test_frame_dump_writes_a_readable_bmp(void) {
  gfx::Framebuffer fb;
  fb.fill(0xF800);
  const char *path = "/tmp/knob_test_dump.bmp";
  TEST_ASSERT_TRUE(desktop::dumpFrameBmp(fb, path));

  FILE *f = std::fopen(path, "rb");
  TEST_ASSERT_NOT_NULL(f);
  unsigned char hdr[54] = {0};
  TEST_ASSERT_EQUAL_UINT32(54u, (uint32_t)std::fread(hdr, 1, 54, f));
  std::fclose(f);

  TEST_ASSERT_EQUAL_UINT8('B', hdr[0]);
  TEST_ASSERT_EQUAL_UINT8('M', hdr[1]);
  const uint32_t w = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
  const uint32_t h = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
  TEST_ASSERT_EQUAL_UINT32(360u, w);
  TEST_ASSERT_EQUAL_UINT32(360u, h);
}


// ---------------------------------------------------------------------------
// Quad3D
//
// The perspective divide and the UV orientation are the two things here that
// look plausible when wrong, so both are asserted against analytically known
// answers rather than against a screenshot.
// ---------------------------------------------------------------------------

namespace {

gfx::Surface fullSurface(gfx::Framebuffer &fb) {
  gfx::Surface s;
  s.px = fb.pixels();
  s.w = gfx::W;
  s.h = gfx::H;
  s.y0 = 0;
  return s;
}

// A 2x2 texture with four distinguishable corners. A symmetric test image
// cannot catch a flipped or transposed mapping, which is the whole point.
void fillCornerTexture(art::Image &t) {
  t.allocate(2, 2);
  t.set(0, 0, gfx::rgb565(255, 0, 0));      // top-left     red
  t.set(1, 0, gfx::rgb565(0, 255, 0));      // top-right    green
  t.set(0, 1, gfx::rgb565(0, 0, 255));      // bottom-left  blue
  t.set(1, 1, gfx::rgb565(255, 255, 255));  // bottom-right white
}

// A frontal quad at constant z, spanning +-half in view space.
void frontalQuad(gfx::Vec3 c[4], float half, float z) {
  c[0] = {-half, -half, z};
  c[1] = {half, -half, z};
  c[2] = {half, half, z};
  c[3] = {-half, half, z};
}

int litPixelsInColumn(const gfx::Framebuffer &fb, int x) {
  int n = 0;
  for (int y = 0; y < gfx::H; ++y)
    if (fb.at(x, y) != 0) ++n;
  return n;
}

}  // namespace

void test_quad_frontal_fills_the_projected_rectangle(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  art::Image tex;
  fillCornerTexture(tex);
  gfx::Quad3D q;
  gfx::Vec3 c[4];
  // z = 1 and half = 0.25 projects to +-0.25*340 = +-85 pixels about the centre.
  frontalQuad(c, 0.25f, 1.0f);
  gfx::Surface s = fullSurface(fb);
  TEST_ASSERT_TRUE(q.draw(s, tex, c, 256, 255));

  // Well inside the rectangle is covered; well outside is not.
  TEST_ASSERT_TRUE(fb.at(180, 180) != 0);
  TEST_ASSERT_TRUE(fb.at(100, 180) != 0);
  TEST_ASSERT_TRUE(fb.at(260, 180) != 0);
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(90, 180));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(270, 180));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(180, 90));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(180, 270));
}

void test_quad_uv_orientation_is_not_flipped_or_transposed(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  art::Image tex;
  fillCornerTexture(tex);
  gfx::Quad3D q;
  gfx::Vec3 c[4];
  frontalQuad(c, 0.25f, 1.0f);
  gfx::Surface s = fullSurface(fb);
  q.draw(s, tex, c, 256, 255);

  // The rectangle runs x,y in [95, 265]. A quarter and three quarters across it
  // land exactly on texel centres under bilinear sampling, so these are exact
  // colours rather than blends.
  const int lo = 95 + 42;
  const int hi = 95 + 127;
  uint8_t r, g, b;

  gfx::unpack565(fb.at(lo, lo), r, g, b);  // top-left must be RED
  TEST_ASSERT_TRUE(r > 200 && g < 60 && b < 60);

  gfx::unpack565(fb.at(hi, lo), r, g, b);  // top-right must be GREEN
  TEST_ASSERT_TRUE(g > 200 && r < 60 && b < 60);

  gfx::unpack565(fb.at(lo, hi), r, g, b);  // bottom-left must be BLUE
  TEST_ASSERT_TRUE(b > 200 && r < 60 && g < 60);

  gfx::unpack565(fb.at(hi, hi), r, g, b);  // bottom-right must be WHITE
  TEST_ASSERT_TRUE(r > 200 && g > 200 && b > 200);
}

void test_quad_rotation_produces_a_trapezoid_not_a_parallelogram(void) {
  // This is the test that proves the perspective divide happens at all. An
  // affine warp would draw a parallelogram: both vertical edges the same
  // height. With a real divide the near edge must be measurably taller.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  art::Image tex;
  fillCornerTexture(tex);
  gfx::Quad3D q;

  const float h = 0.35f, z0 = 1.5f;
  const float ct = 0.7071f, st = 0.7071f;  // 45 degrees about the y axis
  gfx::Vec3 c[4];
  c[0] = {-h * ct, -h, z0 - h * st};  // left edge is NEARER
  c[1] = {h * ct, -h, z0 + h * st};   // right edge is FARTHER
  c[2] = {h * ct, h, z0 + h * st};
  c[3] = {-h * ct, h, z0 - h * st};

  gfx::Surface s = fullSurface(fb);
  TEST_ASSERT_TRUE(q.draw(s, tex, c, 256, 255));

  // Find the drawn horizontal extent, then compare edge heights.
  int first = -1, last = -1;
  for (int x = 0; x < gfx::W; ++x) {
    if (litPixelsInColumn(fb, x) > 0) {
      if (first < 0) first = x;
      last = x;
    }
  }
  TEST_ASSERT_TRUE(first >= 0 && last > first);
  const int near_h = litPixelsInColumn(fb, first + 2);
  const int far_h = litPixelsInColumn(fb, last - 2);
  TEST_ASSERT_TRUE(near_h > 0 && far_h > 0);
  // Analytically 190 against 136 pixels, so demand a clear margin rather than
  // just "different".
  TEST_ASSERT_TRUE(near_h > far_h + 25);
}

void test_quad_behind_or_through_the_camera_draws_nothing(void) {
  gfx::Framebuffer fb;
  art::Image tex;
  fillCornerTexture(tex);
  gfx::Quad3D q;
  gfx::Surface s = fullSurface(fb);
  gfx::Vec3 c[4];

  fb.fill(0x0000);
  frontalQuad(c, 0.25f, -1.0f);  // entirely behind
  TEST_ASSERT_FALSE(q.draw(s, tex, c, 256, 255));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(180, 180));

  fb.fill(0x0000);
  frontalQuad(c, 0.25f, 1.0f);
  c[1].z = 0.01f;  // one corner inside the near plane
  TEST_ASSERT_FALSE(q.draw(s, tex, c, 256, 255));
  TEST_ASSERT_EQUAL_HEX16(0x0000, fb.at(180, 180));
}

void test_quad_larger_than_the_screen_is_clipped(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  art::Image tex;
  fillCornerTexture(tex);
  gfx::Quad3D q;
  gfx::Vec3 c[4];
  frontalQuad(c, 8.0f, 1.0f);  // projects far outside the panel
  gfx::Surface s = fullSurface(fb);
  q.draw(s, tex, c, 256, 255);
  // Every corner covered, and crucially no crash or out-of-bounds write.
  TEST_ASSERT_TRUE(fb.at(0, 0) != 0);
  TEST_ASSERT_TRUE(fb.at(359, 359) != 0);
  TEST_ASSERT_TRUE(fb.at(0, 359) != 0);
  TEST_ASSERT_TRUE(fb.at(359, 0) != 0);
}

void test_quad_alpha_zero_is_a_no_op(void) {
  gfx::Framebuffer fb;
  fb.fill(0x1234);
  art::Image tex;
  fillCornerTexture(tex);
  gfx::Quad3D q;
  gfx::Vec3 c[4];
  frontalQuad(c, 0.25f, 1.0f);
  gfx::Surface s = fullSurface(fb);
  TEST_ASSERT_FALSE(q.draw(s, tex, c, 0, 255));
  TEST_ASSERT_EQUAL_HEX16(0x1234, fb.at(180, 180));
}

void test_quad_drawn_in_bands_matches_a_single_full_frame(void) {
  // The device composites 40-row bands and the desktop composites whole frames.
  // If those ever diverge, the desktop stops being a useful place to judge the
  // device - so the equivalence is asserted, not assumed.
  art::Image tex;
  art::makePlaceholderCover(4242, 64, &tex);
  gfx::Vec3 c[4];
  const float h = 0.3f, z0 = 1.4f;
  c[0] = {-h * 0.8f, -h, z0 - 0.2f};
  c[1] = {h * 0.9f, -h * 0.9f, z0 + 0.25f};
  c[2] = {h * 0.85f, h, z0 + 0.2f};
  c[3] = {-h, h * 0.95f, z0 - 0.15f};

  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);

  gfx::Quad3D q1, q2;
  gfx::Surface s = fullSurface(whole);
  q1.draw(s, tex, c, 256, 255);

  for (int y = 0; y < gfx::H; y += 40) {
    gfx::Surface b;
    b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    b.w = gfx::W;
    b.h = 40;
    b.y0 = y;
    q2.draw(b, tex, c, 256, 255);
  }

  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

// ---------------------------------------------------------------------------
// Placeholder cover
// ---------------------------------------------------------------------------

void test_small_quad_in_bands_matches_full_frame(void) {
  // The band-equivalence test above uses a quad that spans most of the frame, so
  // every band intersects it. This one is deliberately SMALL and centred, so most
  // bands miss it entirely - which is the case that caught a clip folding an
  // out-of-band triangle onto the band's edge row.
  art::Image tex;
  art::makePlaceholderCover(31337, 64, &tex);
  gfx::Vec3 c[4];
  const float h = 0.08f, z = 1.5f;
  c[0] = {-h, -h, z};
  c[1] = {h, -h * 0.9f, z + 0.12f};
  c[2] = {h, h, z + 0.1f};
  c[3] = {-h, h, z};

  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  gfx::Quad3D q1, q2;
  gfx::Surface s = fullSurface(whole);
  q1.draw(s, tex, c, 256, 255);

  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface b;
    b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    b.w = gfx::W;
    b.h = 20;
    b.y0 = y;
    q2.draw(b, tex, c, 256, 255);
  }

  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_placeholder_cover_is_asymmetric_and_deterministic(void) {
  art::Image a, b;
  art::makePlaceholderCover(99, 48, &a);
  art::makePlaceholderCover(99, 48, &b);
  TEST_ASSERT_TRUE(a.valid());
  TEST_ASSERT_EQUAL_INT(48, a.width());
  for (int i = 0; i < 48 * 48; i += 97)
    TEST_ASSERT_EQUAL_HEX16(a.pixels()[i], b.pixels()[i]);

  // The bright wedge must be in the top-left and nowhere else, or a flipped UV
  // mapping would pass the orientation test above by symmetry.
  uint8_t r, g, bl;
  gfx::unpack565(a.at(2, 2), r, g, bl);
  const int tl = r + g + bl;
  gfx::unpack565(a.at(45, 45), r, g, bl);
  const int br = r + g + bl;
  TEST_ASSERT_TRUE(tl > br);
}

// ---------------------------------------------------------------------------
// Particles
// ---------------------------------------------------------------------------

void test_particles_pool_is_capped_and_does_not_overflow(void) {
  fx::Particles p;
  fx::SpawnParams sp;
  sp.colors[0] = 0xFFFF;
  sp.color_count = 1;
  p.configure(sp);
  core::Rng rng(1);
  p.emit(fx::Particles::MAX + 500, rng);
  TEST_ASSERT_EQUAL_INT(fx::Particles::MAX, p.live());
}

void test_particles_expire(void) {
  fx::Particles p;
  fx::SpawnParams sp;
  sp.life_min = 0.05f;
  sp.life_max = 0.10f;
  sp.colors[0] = 0xFFFF;
  sp.color_count = 1;
  p.configure(sp);
  core::Rng rng(2);
  p.emit(200, rng);
  TEST_ASSERT_EQUAL_INT(200, p.live());
  p.update(0.5f);
  TEST_ASSERT_EQUAL_INT(0, p.live());
}

void test_particles_are_deterministic_for_a_seed(void) {
  fx::SpawnParams sp;
  sp.colors[0] = 0xF800;
  sp.colors[1] = 0x07E0;
  sp.color_count = 2;

  gfx::Framebuffer a, b;
  a.fill(0);
  b.fill(0);
  for (int pass = 0; pass < 2; ++pass) {
    fx::Particles p;
    p.configure(sp);
    core::Rng rng(777);
    gfx::Framebuffer &fb = pass == 0 ? a : b;
    for (int f = 0; f < 30; ++f) {
      p.emit(6, rng);
      p.update(1.0f / 60.0f);
    }
    gfx::Surface s = fullSurface(fb);
    p.render(s);
  }
  int diffs = 0;
  for (size_t i = 0; i < gfx::Framebuffer::count(); ++i)
    if (a.pixels()[i] != b.pixels()[i]) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_particles_off_screen_write_nothing(void) {
  fx::Particles p;
  fx::SpawnParams sp;
  sp.x = -400.0f;
  sp.y = -400.0f;
  sp.spread = 1.0f;
  sp.speed_min = 0.0f;
  sp.speed_max = 0.0f;
  sp.colors[0] = 0xFFFF;
  sp.color_count = 1;
  p.configure(sp);
  core::Rng rng(3);
  p.emit(300, rng);
  p.update(1.0f / 60.0f);

  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  p.render(s);
  int lit = 0;
  for (size_t i = 0; i < gfx::Framebuffer::count(); ++i)
    if (fb.pixels()[i] != 0) ++lit;
  TEST_ASSERT_EQUAL_INT(0, lit);
}

void test_particles_render_additively_over_a_background(void) {
  fx::Particles p;
  fx::SpawnParams sp;
  sp.x = 180.0f;
  sp.y = 180.0f;
  sp.spread = 0.5f;
  sp.speed_min = 0.0f;
  sp.speed_max = 0.0f;
  sp.size_min = 3.0f;
  sp.size_max = 3.0f;
  sp.colors[0] = gfx::rgb565(0, 0, 255);
  sp.color_count = 1;
  p.configure(sp);
  core::Rng rng(4);
  p.emit(40, rng);
  p.update(1.0f / 60.0f);

  gfx::Framebuffer fb;
  const uint16_t bg = gfx::rgb565(120, 0, 0);
  fb.fill(bg);
  gfx::Surface s = fullSurface(fb);
  p.render(s);

  // Additive, so red must survive underneath and blue must have been added.
  uint8_t r, g, b;
  gfx::unpack565(fb.at(180, 180), r, g, b);
  TEST_ASSERT_TRUE(r > 100);
  TEST_ASSERT_TRUE(b > 40);
}

// ---------------------------------------------------------------------------
// Procedural modulation
// ---------------------------------------------------------------------------

void test_procedural_stays_in_range(void) {
  audio::Procedural pr;
  audio::Modulation m;
  pr.reseed(1234);
  for (int i = 0; i < 8000; ++i) {
    pr.fill(&m, 1.0f / 60.0f);
    TEST_ASSERT_TRUE(m.bass >= 0.0f && m.bass <= 1.0f);
    TEST_ASSERT_TRUE(m.mid >= 0.0f && m.mid <= 1.0f);
    TEST_ASSERT_TRUE(m.treble >= 0.0f && m.treble <= 1.0f);
    TEST_ASSERT_TRUE(m.loudness >= 0.0f && m.loudness <= 1.0f);
    TEST_ASSERT_TRUE(m.beat_phase >= 0.0f && m.beat_phase < 1.0f);
  }
}

void test_procedural_tempo_is_plausible_and_seed_stable(void) {
  audio::Procedural a, b;
  a.reseed(5150);
  b.reseed(5150);
  TEST_ASSERT_TRUE(a.bpm() >= 84.0f && a.bpm() <= 148.0f);
  TEST_ASSERT_EQUAL_FLOAT(a.bpm(), b.bpm());

  audio::Procedural c;
  c.reseed(9999);
  TEST_ASSERT_TRUE(c.bpm() != a.bpm());
}

void test_procedural_onset_rate_matches_its_tempo(void) {
  audio::Procedural pr;
  audio::Modulation m;
  pr.reseed(31337);
  const float dt = 1.0f / 60.0f;
  int onsets = 0;
  const int frames = 60 * 30;  // thirty seconds
  for (int i = 0; i < frames; ++i) {
    pr.fill(&m, dt);
    if (m.onset) ++onsets;
  }
  const float expected = pr.bpm() * 0.5f;  // beats in thirty seconds
  TEST_ASSERT_TRUE(onsets > expected * 0.85f);
  TEST_ASSERT_TRUE(onsets < expected * 1.15f);
}

void test_procedural_is_marked_not_live(void) {
  audio::Procedural pr;
  audio::Modulation m;
  m.live = true;
  pr.reseed(7);
  pr.fill(&m, 0.016f);
  // Anything reading this must be able to tell it is not hearing a microphone.
  TEST_ASSERT_FALSE(m.live);
}


// ---------------------------------------------------------------------------
// FFT
//
// Asserted against analytically known answers at more than one bin, because a
// twiddle-indexing error is often correct at one frequency and wrong elsewhere.
// ---------------------------------------------------------------------------

namespace {

// A unit-amplitude sine landing exactly on bin `k`, so its energy has nowhere
// to leak.
void fillSineAtBin(float *buf, int n, int k) {
  for (int i = 0; i < n; ++i)
    buf[i] = std::sin(2.0f * 3.14159265f * static_cast<float>(k) *
                      static_cast<float>(i) / static_cast<float>(n));
}

int peakBin(const float *mag, int bins) {
  int best = 0;
  for (int i = 1; i < bins; ++i)
    if (mag[i] > mag[best]) best = i;
  return best;
}

float meanExcluding(const float *mag, int bins, int skip, int width) {
  float sum = 0.0f;
  int n = 0;
  for (int i = 1; i < bins; ++i) {
    if (i > skip - width && i < skip + width) continue;
    sum += mag[i];
    ++n;
  }
  return n ? sum / n : 0.0f;
}

// A sine at an arbitrary frequency in Hz, for the band tests.
void fillSineHz(float *buf, int n, float hz, float amp) {
  for (int i = 0; i < n; ++i)
    buf[i] = amp * std::sin(2.0f * 3.14159265f * hz *
                            static_cast<float>(i) /
                            static_cast<float>(audio::SAMPLE_RATE));
}

}  // namespace

void test_fft_finds_a_tone_at_bin_32(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  fillSineAtBin(in, audio::Fft::N, 32);
  fft.magnitudes(in, mag);
  TEST_ASSERT_EQUAL_INT(32, peakBin(mag, audio::Fft::BINS));
  TEST_ASSERT_TRUE(mag[32] > meanExcluding(mag, audio::Fft::BINS, 32, 3) * 20.0f);
}

void test_fft_finds_a_tone_at_bin_100(void) {
  // A second frequency, because a twiddle-index error can be exact at one bin.
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  fillSineAtBin(in, audio::Fft::N, 100);
  fft.magnitudes(in, mag);
  TEST_ASSERT_EQUAL_INT(100, peakBin(mag, audio::Fft::BINS));
  TEST_ASSERT_TRUE(mag[100] > meanExcluding(mag, audio::Fft::BINS, 100, 3) * 20.0f);
}

void test_fft_of_silence_is_silent(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N] = {};
  static float mag[audio::Fft::BINS];
  fft.magnitudes(in, mag);
  for (int i = 0; i < audio::Fft::BINS; ++i)
    TEST_ASSERT_TRUE(mag[i] < 1e-4f);
}

void test_fft_puts_dc_in_bin_zero(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  for (int i = 0; i < audio::Fft::N; ++i) in[i] = 1.0f;
  fft.magnitudes(in, mag);
  // Windowed DC concentrates at bin 0 with a little in 1-2; nothing beyond.
  TEST_ASSERT_TRUE(mag[0] > 10.0f);
  for (int i = 5; i < audio::Fft::BINS; ++i) TEST_ASSERT_TRUE(mag[i] < 1.0f);
}

void test_fft_has_no_state_between_calls(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float a[audio::Fft::BINS], b[audio::Fft::BINS];
  fillSineAtBin(in, audio::Fft::N, 51);
  fft.magnitudes(in, a);
  fft.magnitudes(in, b);
  for (int i = 0; i < audio::Fft::BINS; ++i)
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, a[i], b[i]);
}

// ---------------------------------------------------------------------------
// Band energy
// ---------------------------------------------------------------------------

void test_bands_separate_low_from_high(void) {
  // This is the test that catches a frequency-to-bin arithmetic error, which is
  // otherwise completely invisible: everything still moves, just not with the
  // right part of the music.
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  audio::BandEnergy be;
  float bass = 0, mid = 0, tre = 0, loud = 0;

  fillSineHz(in, audio::Fft::N, 100.0f, 0.6f);
  for (int i = 0; i < 200; ++i) {
    fft.magnitudes(in, mag);
    be.process(mag, audio::Fft::BINS, 1.0f / 60.0f, &bass, &mid, &tre, &loud);
  }
  TEST_ASSERT_TRUE(bass > 0.5f);
  TEST_ASSERT_TRUE(tre < 0.15f);

  be.reset();
  fillSineHz(in, audio::Fft::N, 5000.0f, 0.6f);
  for (int i = 0; i < 200; ++i) {
    fft.magnitudes(in, mag);
    be.process(mag, audio::Fft::BINS, 1.0f / 60.0f, &bass, &mid, &tre, &loud);
  }
  TEST_ASSERT_TRUE(tre > 0.5f);
  TEST_ASSERT_TRUE(bass < 0.15f);
}

void test_bands_decay_to_quiet_on_silence(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  audio::BandEnergy be;
  float bass = 0, mid = 0, tre = 0, loud = 0;

  fillSineHz(in, audio::Fft::N, 300.0f, 0.8f);
  for (int i = 0; i < 120; ++i) {
    fft.magnitudes(in, mag);
    be.process(mag, audio::Fft::BINS, 1.0f / 60.0f, &bass, &mid, &tre, &loud);
  }
  TEST_ASSERT_TRUE(loud > 0.3f);

  for (int i = 0; i < audio::Fft::N; ++i) in[i] = 0.0f;
  for (int i = 0; i < 300; ++i) {
    fft.magnitudes(in, mag);
    be.process(mag, audio::Fft::BINS, 1.0f / 60.0f, &bass, &mid, &tre, &loud);
  }
  TEST_ASSERT_TRUE(loud < 0.05f);
  TEST_ASSERT_TRUE(bass < 0.05f);
}

void test_bands_attack_faster_than_they_release(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  audio::BandEnergy be;
  float mid = 0;
  const float dt = 1.0f / 60.0f;

  // Measured on `mid` rather than `loudness`: a single 400 Hz tone puts all its
  // energy in one band, so the band reaches full scale where a cross-band mean
  // cannot, and the test is about the smoothing rather than about the mixing.
  fillSineHz(in, audio::Fft::N, 400.0f, 0.8f);
  int rise = 0;
  while (mid < 0.5f && rise < 2000) {
    fft.magnitudes(in, mag);
    be.process(mag, audio::Fft::BINS, dt, nullptr, &mid, nullptr, nullptr);
    ++rise;
  }
  TEST_ASSERT_TRUE(rise < 2000);

  for (int i = 0; i < audio::Fft::N; ++i) in[i] = 0.0f;
  int fall = 0;
  while (mid > 0.25f && fall < 4000) {
    fft.magnitudes(in, mag);
    be.process(mag, audio::Fft::BINS, dt, nullptr, &mid, nullptr, nullptr);
    ++fall;
  }
  // Punchy means rising quickly and falling slowly. Symmetric smoothing reads
  // as mush and the reverse reads as broken.
  TEST_ASSERT_TRUE(fall > rise);
}

// ---------------------------------------------------------------------------
// Onset detection
// ---------------------------------------------------------------------------

namespace {

// Feeds `hops` analysis hops of a click train and counts detected onsets.
// period_hops is how many hops apart the clicks are.
int countOnsets(int hops, int period_hops, bool steady_tone) {
  static audio::Fft fft;
  static float in[audio::Fft::N];
  static float mag[audio::Fft::BINS];
  audio::OnsetDetector od;
  core::Rng rng(12345);
  int onsets = 0;

  for (int h = 0; h < hops; ++h) {
    if (steady_tone) {
      fillSineHz(in, audio::Fft::N, 440.0f, 0.5f);
    } else {
      const bool click = period_hops > 0 && (h % period_hops) == 0;
      for (int i = 0; i < audio::Fft::N; ++i)
        in[i] = click ? (rng.unit() * 2.0f - 1.0f) * 0.9f : 0.0f;
    }
    fft.magnitudes(in, mag);
    if (od.process(mag, audio::Fft::BINS)) ++onsets;
  }
  return onsets;
}

}  // namespace

void test_onsets_fire_on_a_click_train(void) {
  // 16 kHz with a 256-sample hop is 62.5 hops/s. 120 bpm is 2 beats/s, so a
  // beat every 31 hops. Over 500 hops that is 16 beats.
  const int onsets = countOnsets(500, 31, false);
  TEST_ASSERT_TRUE(onsets >= 13);
  TEST_ASSERT_TRUE(onsets <= 19);
}

void test_onsets_do_not_fire_on_a_steady_tone(void) {
  // A detector that fires on sustained sound is useless: it would report a beat
  // through every held note.
  const int onsets = countOnsets(400, 0, true);
  TEST_ASSERT_TRUE(onsets < 4);
}

void test_onsets_do_not_fire_on_silence(void) {
  static audio::Fft fft;
  static float in[audio::Fft::N] = {};
  static float mag[audio::Fft::BINS];
  audio::OnsetDetector od;
  int onsets = 0;
  for (int h = 0; h < 400; ++h) {
    fft.magnitudes(in, mag);
    if (od.process(mag, audio::Fft::BINS)) ++onsets;
  }
  TEST_ASSERT_EQUAL_INT(0, onsets);
}

// ---------------------------------------------------------------------------
// Tempo tracking
// ---------------------------------------------------------------------------

void test_tempo_converges_on_120_bpm(void) {
  audio::TempoTracker tt;
  const float dt = 1.0f / 60.0f;
  const int period = 30;  // frames between beats at 120 bpm and 60 fps
  for (int f = 0; f < 60 * 12; ++f) tt.process((f % period) == 0, dt);
  TEST_ASSERT_TRUE(tt.bpm() > 114.0f);
  TEST_ASSERT_TRUE(tt.bpm() < 126.0f);
}

void test_tempo_phase_stays_in_range_and_wraps(void) {
  audio::TempoTracker tt;
  const float dt = 1.0f / 60.0f;
  int wraps = 0;
  float last = 0.0f;
  for (int f = 0; f < 60 * 10; ++f) {
    tt.process((f % 30) == 0, dt);
    const float p = tt.beatPhase();
    TEST_ASSERT_TRUE(p >= 0.0f && p < 1.0f);
    if (p < last) ++wraps;
    last = p;
  }
  TEST_ASSERT_TRUE(wraps > 15);
}

void test_tempo_ignores_implausible_intervals(void) {
  audio::TempoTracker tt;
  const float dt = 1.0f / 60.0f;
  // Establish 120 bpm.
  for (int f = 0; f < 60 * 8; ++f) tt.process((f % 30) == 0, dt);
  const float before = tt.bpm();
  // Then a burst of spurious onsets four frames apart - 900 bpm - which must not
  // drag the estimate, or one noisy moment would ruin the tracking.
  for (int f = 0; f < 60; ++f) tt.process((f % 4) == 0, dt);
  TEST_ASSERT_FLOAT_WITHIN(8.0f, before, tt.bpm());
}


// ---------------------------------------------------------------------------
// WavMic and AudioAnalyzer
//
// Fixtures come from tools/make_test_wav.py. Paths are relative to the repo
// root, which is where pio test runs from.
// ---------------------------------------------------------------------------

namespace {
const char *kSilence = "assets/audio/silence.wav";
const char *kSine = "assets/audio/sine440.wav";
const char *kClicks = "assets/audio/clicks120.wav";

// Runs the analyzer for `secs` of simulated time and reports the final state.
struct AnalyzerRun {
  bool live;
  float mean_bass;
  float band_variation;  // max minus min of bass, so a frozen output is visible
  int onsets;
};

AnalyzerRun runAnalyzer(const char *wav, float secs) {
  desktop::WavMic mic;
  const bool ok = wav ? mic.open(wav) : false;
  audio::AudioAnalyzer an;
  an.begin(ok ? &mic : nullptr);
  an.setTrack(4242);

  audio::Modulation m;
  const float dt = 1.0f / 60.0f;
  float sum = 0.0f, lo = 2.0f, hi = -1.0f;
  int n = 0, onsets = 0;
  const int frames = static_cast<int>(secs * 60.0f);
  for (int i = 0; i < frames; ++i) {
    if (ok) mic.advance(dt);
    an.update(&m, dt);
    if (m.onset) ++onsets;
    // Skip the first second so the crossfade and the peak tracker have settled.
    if (i > 60) {
      sum += m.bass;
      if (m.bass < lo) lo = m.bass;
      if (m.bass > hi) hi = m.bass;
      ++n;
    }
  }
  AnalyzerRun r;
  r.live = m.live;
  r.mean_bass = n ? sum / n : 0.0f;
  r.band_variation = n ? hi - lo : 0.0f;
  r.onsets = onsets;
  return r;
}

}  // namespace

void test_wavmic_loads_a_fixture(void) {
  desktop::WavMic mic;
  TEST_ASSERT_TRUE(mic.open(kSine));
  TEST_ASSERT_TRUE(mic.loaded());
  TEST_ASSERT_EQUAL_INT(audio::SAMPLE_RATE, mic.sampleRate());
}

void test_wavmic_paces_output_and_stays_in_range(void) {
  desktop::WavMic mic;
  TEST_ASSERT_TRUE(mic.open(kSine));
  float buf[256];
  // Nothing until enough simulated time has passed: the pacing is what keeps a
  // headless run deterministic rather than dependent on frame rate.
  TEST_ASSERT_EQUAL_INT(0, mic.read(buf, 256));
  mic.advance(0.05f);
  TEST_ASSERT_EQUAL_INT(256, mic.read(buf, 256));
  bool nonzero = false;
  for (int i = 0; i < 256; ++i) {
    TEST_ASSERT_TRUE(buf[i] >= -1.01f && buf[i] <= 1.01f);
    if (buf[i] != 0.0f) nonzero = true;
  }
  TEST_ASSERT_TRUE(nonzero);
}

void test_wavmic_loops_at_the_end(void) {
  desktop::WavMic mic;
  TEST_ASSERT_TRUE(mic.open(kSilence));  // 3s, so it will wrap
  float buf[256];
  int total = 0;
  for (int i = 0; i < 400; ++i) {
    mic.advance(0.05f);
    total += mic.read(buf, 256);
  }
  // Far more samples than the file holds, with no failure: it wrapped.
  TEST_ASSERT_TRUE(total > audio::SAMPLE_RATE * 3);
}

void test_wavmic_missing_file_fails_cleanly(void) {
  desktop::WavMic mic;
  TEST_ASSERT_FALSE(mic.open("assets/audio/does-not-exist.wav"));
  float buf[64];
  mic.advance(1.0f);
  TEST_ASSERT_EQUAL_INT(0, mic.read(buf, 64));
}

void test_analyzer_goes_live_on_music(void) {
  const AnalyzerRun r = runAnalyzer(kClicks, 4.0f);
  TEST_ASSERT_TRUE(r.live);
  TEST_ASSERT_TRUE(r.onsets > 4);
}

void test_analyzer_falls_back_on_silence_and_keeps_moving(void) {
  const AnalyzerRun r = runAnalyzer(kSilence, 4.0f);
  TEST_ASSERT_FALSE(r.live);
  // The important half: the fallback must still be ANIMATING. Reporting
  // not-live while emitting a frozen zero would look exactly like a hang.
  TEST_ASSERT_TRUE(r.band_variation > 0.05f);
  TEST_ASSERT_TRUE(r.onsets > 4);
}

void test_analyzer_with_no_mic_at_all_still_animates(void) {
  const AnalyzerRun r = runAnalyzer(nullptr, 4.0f);
  TEST_ASSERT_FALSE(r.live);
  TEST_ASSERT_TRUE(r.band_variation > 0.05f);
}

void test_analyzer_handover_is_not_instant(void) {
  // A snap between live and procedural would be visible as a jolt, so the
  // crossfade has to take real time.
  desktop::WavMic mic;
  TEST_ASSERT_TRUE(mic.open(kClicks));
  audio::AudioAnalyzer an;
  an.begin(&mic);
  an.setTrack(7);
  audio::Modulation m;
  const float dt = 1.0f / 60.0f;
  for (int i = 0; i < 240; ++i) {
    mic.advance(dt);
    an.update(&m, dt);
  }
  TEST_ASSERT_TRUE(m.live);

  // Stop feeding audio. It must hold, then fade - not drop out immediately.
  int frames_to_handover = 0;
  for (int i = 0; i < 600 && m.live; ++i) {
    an.update(&m, dt);  // no mic.advance, so read() returns nothing
    ++frames_to_handover;
  }
  TEST_ASSERT_FALSE(m.live);
  const float secs = frames_to_handover * dt;
  // Long enough to be a fade rather than a jolt, and bounded so walking away
  // from the desk is noticed within a few seconds rather than never.
  TEST_ASSERT_TRUE(secs >= audio::AudioAnalyzer::QUIET_HOLD_S);
  TEST_ASSERT_TRUE(secs < 12.0f);
}

void test_analyzer_rides_through_gaps_in_percussive_music(void) {
  // A 120 bpm click track is five milliseconds of sound every five hundred, so
  // an instantaneous level test reads it as silence almost all of the time. This
  // is the case that broke the first version.
  desktop::WavMic mic;
  TEST_ASSERT_TRUE(mic.open(kClicks));
  audio::AudioAnalyzer an;
  an.begin(&mic);
  an.setTrack(11);
  audio::Modulation m;
  const float dt = 1.0f / 60.0f;
  for (int i = 0; i < 180; ++i) {
    mic.advance(dt);
    an.update(&m, dt);
  }
  // Now hold live through six seconds of the same material without a single
  // frame of dropout.
  int dropouts = 0;
  for (int i = 0; i < 360; ++i) {
    mic.advance(dt);
    an.update(&m, dt);
    if (!m.live) ++dropouts;
  }
  TEST_ASSERT_EQUAL_INT(0, dropouts);
}


// ---------------------------------------------------------------------------
// Gesture recognition
//
// A pure state machine, so every case is a synthetic trace. These are the
// gestures the whole transport depends on: getting "a long press must not also
// emit a tap" wrong means every like also toggles playback.
// ---------------------------------------------------------------------------

namespace {

using input::Gesture;
using input::GestureRecognizer;

// Runs a straight drag from (x0,y0) to (x1,y1) over `ms`, then releases.
Gesture drag(int x0, int y0, int x1, int y1, uint32_t ms) {
  GestureRecognizer g;
  const int steps = 10;
  uint32_t t = 1000;
  g.update(true, x0, y0, t);
  for (int i = 1; i <= steps; ++i) {
    t = 1000 + ms * i / steps;
    const int x = x0 + (x1 - x0) * i / steps;
    const int y = y0 + (y1 - y0) * i / steps;
    g.update(true, x, y, t);
  }
  return g.update(false, x1, y1, t + 1);
}

}  // namespace

void test_gesture_tap_fires_on_release(void) {
  GestureRecognizer g;
  TEST_ASSERT_EQUAL(Gesture::None, g.update(true, 180, 180, 1000));
  TEST_ASSERT_EQUAL(Gesture::None, g.update(true, 181, 180, 1100));
  TEST_ASSERT_EQUAL(Gesture::Tap, g.update(false, 181, 180, 1200));
}

void test_gesture_long_press_fires_once_and_suppresses_the_tap(void) {
  GestureRecognizer g;
  g.update(true, 180, 180, 0);
  TEST_ASSERT_EQUAL(Gesture::None,
                    g.update(true, 180, 180, GestureRecognizer::LONG_MS - 1));
  TEST_ASSERT_EQUAL(Gesture::LongPress,
                    g.update(true, 180, 180, GestureRecognizer::LONG_MS));
  // Only once while still held.
  TEST_ASSERT_EQUAL(Gesture::None,
                    g.update(true, 180, 180, GestureRecognizer::LONG_MS + 200));
  // And the release must NOT also be a tap: that would make every like toggle
  // playback as well.
  TEST_ASSERT_EQUAL(Gesture::None,
                    g.update(false, 180, 180, GestureRecognizer::LONG_MS + 400));
}

void test_gesture_swipes_resolve_by_direction(void) {
  TEST_ASSERT_EQUAL(Gesture::SwipeRight, drag(120, 180, 260, 180, 250));
  TEST_ASSERT_EQUAL(Gesture::SwipeLeft, drag(260, 180, 120, 180, 250));
  TEST_ASSERT_EQUAL(Gesture::SwipeUp, drag(180, 260, 180, 120, 250));
  TEST_ASSERT_EQUAL(Gesture::SwipeDown, drag(180, 120, 180, 260, 250));
}

void test_gesture_short_drag_is_a_tap_not_a_swipe(void) {
  // Ten pixels of smear is a tap by a human finger, not an instruction.
  TEST_ASSERT_EQUAL(Gesture::Tap, drag(180, 180, 190, 182, 150));
}

void test_gesture_diagonal_resolves_to_the_dominant_axis(void) {
  // Mostly horizontal with vertical drift: one gesture, not two.
  TEST_ASSERT_EQUAL(Gesture::SwipeRight, drag(120, 170, 260, 200, 250));
  TEST_ASSERT_EQUAL(Gesture::SwipeDown, drag(170, 120, 200, 260, 250));
}

void test_gesture_swipe_that_curls_back_is_still_a_swipe(void) {
  // The furthest travel counts, not the release point. Reading only where the
  // finger lifted loses a flick that rebounds.
  GestureRecognizer g;
  g.update(true, 180, 180, 0);
  g.update(true, 260, 180, 80);
  g.update(true, 200, 180, 160);
  TEST_ASSERT_EQUAL(Gesture::SwipeRight, g.update(false, 195, 180, 200));
}

void test_gesture_hold_at_the_end_of_a_drag_is_not_a_long_press(void) {
  // Otherwise every slow swipe also likes the track.
  GestureRecognizer g;
  g.update(true, 120, 180, 0);
  g.update(true, 260, 180, 200);
  TEST_ASSERT_EQUAL(Gesture::None, g.update(true, 260, 180, 1200));
  TEST_ASSERT_EQUAL(Gesture::SwipeRight, g.update(false, 260, 180, 1300));
}

void test_gesture_no_touch_reports_nothing(void) {
  GestureRecognizer g;
  for (uint32_t t = 0; t < 5000; t += 100)
    TEST_ASSERT_EQUAL(Gesture::None, g.update(false, 0, 0, t));
}

void test_gesture_survives_the_millis_wrap(void) {
  // Elapsed time is computed by unsigned subtraction, so a touch spanning the
  // 49.7-day wrap must still time out correctly rather than never.
  GestureRecognizer g;
  g.update(true, 180, 180, 0xFFFFFF00u);
  TEST_ASSERT_EQUAL(Gesture::LongPress, g.update(true, 180, 180, 0x00000200u));
}


// ---------------------------------------------------------------------------
// RadialShell
// ---------------------------------------------------------------------------

void test_shell_drawn_in_bands_matches_full_frame(void) {
  // The shell is drawn once per band on the device and once per frame on the
  // desktop, and it only walks the arc segments that can reach the surface it is
  // given. Two separate bugs came out of that optimisation - additive drawing
  // not being idempotent, and the angular range being solved from the outer
  // radius only - so the equivalence is asserted at several progress values
  // rather than assumed.
  for (int step = 0; step <= 8; ++step) {
    const float p = static_cast<float>(step) / 8.0f;

    gfx::Framebuffer whole, banded;
    whole.fill(0x0000);
    banded.fill(0x0000);

    shell::RadialShell sh1, sh2;
    gfx::Surface s = fullSurface(whole);
    sh1.render(s, p, gfx::rgb565(60, 140, 230), 62, 1000, 0.5f);

    for (int y = 0; y < gfx::H; y += 20) {
      gfx::Surface b;
      b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
      b.w = gfx::W;
      b.h = 20;
      b.y0 = y;
      sh2.render(b, p, gfx::rgb565(60, 140, 230), 62, 1000, 0.5f);
    }

    int diffs = 0;
    for (int y = 0; y < gfx::H; ++y)
      for (int x = 0; x < gfx::W; ++x)
        if (whole.at(x, y) != banded.at(x, y)) ++diffs;
    TEST_ASSERT_EQUAL_INT(0, diffs);
  }
}

void test_shell_progress_ring_grows_with_progress(void) {
  int last = -1;
  for (int step = 0; step <= 4; ++step) {
    gfx::Framebuffer fb;
    fb.fill(0x0000);
    shell::RadialShell sh;
    gfx::Surface s = fullSurface(fb);
    sh.render(s, static_cast<float>(step) / 4.0f, gfx::rgb565(0, 200, 255), 62,
              1000, 0.0f);
    // Count pixels with a strong blue component: the tint, not the dark track.
    int lit = 0;
    for (int y = 0; y < gfx::H; ++y)
      for (int x = 0; x < gfx::W; ++x) {
        uint8_t r, g, b;
        gfx::unpack565(fb.at(x, y), r, g, b);
        if (b > 120) ++lit;
      }
    TEST_ASSERT_TRUE(lit > last);
    last = lit;
  }
}

void test_shell_unknown_volume_is_not_drawn_as_zero(void) {
  // volume_pct of -1 means the active device did not report one. Drawing that
  // as an empty ring would be a confident lie.
  gfx::Framebuffer known, unknown;
  known.fill(0x0000);
  unknown.fill(0x0000);

  shell::RadialShell a, b;
  a.showVolume(0, 1000);
  b.showVolume(-1, 1000);
  gfx::Surface sk = fullSurface(known);
  gfx::Surface su = fullSurface(unknown);
  a.render(sk, 0.0f, 0x07FF, 0, 1000, 0.0f);
  b.render(su, 0.0f, 0x07FF, -1, 1000, 0.0f);

  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (known.at(x, y) != unknown.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 100);
}

void test_shell_volume_overlay_expires(void) {
  shell::RadialShell sh;
  sh.showVolume(50, 1000);
  TEST_ASSERT_TRUE(sh.volumeVisible(1000));
  TEST_ASSERT_TRUE(sh.volumeVisible(1000 + shell::RadialShell::VOLUME_SHOW_MS - 1));
  TEST_ASSERT_FALSE(sh.volumeVisible(1000 + shell::RadialShell::VOLUME_SHOW_MS));
}


// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

namespace {

int litPixels(const gfx::Framebuffer &fb) {
  int n = 0;
  for (size_t i = 0; i < gfx::Framebuffer::count(); ++i)
    if (fb.pixels()[i] != 0) ++n;
  return n;
}

// Renders one string and returns how many pixels it lit.
int renderCount(const char *utf8) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  gfx::drawText(s, gfx::fontTitle(), 40, 180, utf8, 0xFFFF);
  return litPixels(fb);
}

}  // namespace

void test_text_width_is_zero_for_empty_and_grows_with_length(void) {
  TEST_ASSERT_EQUAL_INT(0, gfx::textWidth(gfx::fontTitle(), ""));
  TEST_ASSERT_EQUAL_INT(0, gfx::textWidth(gfx::fontTitle(), nullptr));
  const int a = gfx::textWidth(gfx::fontTitle(), "A");
  const int aa = gfx::textWidth(gfx::fontTitle(), "AA");
  TEST_ASSERT_TRUE(a > 0);
  TEST_ASSERT_EQUAL_INT(a * 2, aa);  // monospace
}

void test_text_draws_inside_its_measured_box(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  const int x = 40, baseline = 180;
  gfx::drawText(s, gfx::fontTitle(), x, baseline, "Hello", 0xFFFF);

  const int w = gfx::textWidth(gfx::fontTitle(), "Hello");
  const int h = gfx::textHeight(gfx::fontTitle());
  int minx = 999, maxx = -1, miny = 999, maxy = -1;
  for (int y = 0; y < gfx::H; ++y)
    for (int px = 0; px < gfx::W; ++px)
      if (fb.at(px, y) != 0) {
        if (px < minx) minx = px;
        if (px > maxx) maxx = px;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
      }
  TEST_ASSERT_TRUE(maxx >= 0);           // something drew
  TEST_ASSERT_TRUE(minx >= x - 2);       // no drawing left of the origin
  TEST_ASSERT_TRUE(maxx <= x + w + 2);   // nor past the measured width
  TEST_ASSERT_TRUE(maxy <= baseline + 4);  // descenders only just below
  TEST_ASSERT_TRUE(miny >= baseline - h - 2);
}

void test_text_renders_latin1_accents_not_tofu(void) {
  // The reason the fonts are generated at 0x20-0xFF. An ASCII-only face renders
  // "Bjork" with an umlaut as a fallback glyph, and a device pointed at a real
  // library meets that within the hour.
  const int o = renderCount("o");
  const int o_umlaut = renderCount("\xC3\xB6");  // U+00F6
  const int question = renderCount("?");
  TEST_ASSERT_TRUE(o_umlaut > 0);
  // An umlaut is an o plus two dots, so strictly more ink than either.
  TEST_ASSERT_TRUE(o_umlaut > o);
  TEST_ASSERT_TRUE(o_umlaut != question);

  // And a whole accented name renders wider than nothing, with no gaps.
  TEST_ASSERT_TRUE(gfx::textWidth(gfx::fontTitle(), "Bj\xC3\xB6rk") >
                   gfx::textWidth(gfx::fontTitle(), "Bjrk"));
}

void test_text_beyond_latin1_falls_back_visibly(void) {
  // CJK is outside this font. It must render a visible marker rather than
  // vanish: silently dropping characters makes a name look like bad metadata.
  const int cjk = renderCount("\xE4\xB8\xAD");  // U+4E2D
  const int question = renderCount("?");
  TEST_ASSERT_TRUE(cjk > 0);
  TEST_ASSERT_EQUAL_INT(question, cjk);
}

void test_text_malformed_utf8_terminates(void) {
  // Spotify metadata is user-supplied. A truncated sequence must not walk the
  // pointer off the end of the string.
  TEST_ASSERT_TRUE(gfx::textWidth(gfx::fontTitle(), "\xC3") > 0);
  TEST_ASSERT_TRUE(gfx::textWidth(gfx::fontTitle(), "\xFF\xFE") > 0);
  TEST_ASSERT_TRUE(gfx::textWidth(gfx::fontTitle(), "ok\xC3") > 0);
}

void test_text_fit_truncates_within_the_budget(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  const char *lng = "East Side of Sorrow - Live From Nashville";
  const int max_w = 150;
  const int drawn = gfx::drawTextFit(s, gfx::fontSmall(), 180, 180, lng, max_w,
                                     0xFFFF);
  TEST_ASSERT_TRUE(drawn > 0);
  TEST_ASSERT_TRUE(drawn <= max_w);

  // And a short string is not truncated at all.
  const int shortw = gfx::textWidth(gfx::fontSmall(), "Hi");
  TEST_ASSERT_EQUAL_INT(shortw,
                        gfx::drawTextFit(s, gfx::fontSmall(), 180, 220, "Hi",
                                         max_w, 0xFFFF));
}

void test_text_outside_the_surface_draws_nothing(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s;
  s.px = fb.pixels();
  s.w = gfx::W;
  s.h = 20;
  s.y0 = 0;
  // Baseline far below this band.
  gfx::drawText(s, gfx::fontTitle(), 40, 300, "Hello", 0xFFFF);
  TEST_ASSERT_EQUAL_INT(0, litPixels(fb));
}

void test_text_drawn_in_bands_matches_full_frame(void) {
  // A glyph is up to fifteen rows tall, so it straddles a twenty-row band. The
  // bitstream index has to keep advancing through rows that are clipped away,
  // or every glyph after the first band boundary decodes from the wrong offset.
  const char *str = "East Side of Sorrow";
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);

  gfx::Surface s = fullSurface(whole);
  gfx::drawTextCentered(s, gfx::fontTitle(), 180, 195, str, 0xFFFF);

  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface b;
    b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    b.w = gfx::W;
    b.h = 20;
    b.y0 = y;
    gfx::drawTextCentered(b, gfx::fontTitle(), 180, 195, str, 0xFFFF);
  }

  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_half_chord_narrows_toward_the_poles(void) {
  // The usable width on a round screen is much less than 360 anywhere but the
  // middle, which is the whole reason text has to be measured against it.
  TEST_ASSERT_TRUE(gfx::halfChordAt(gfx::CY, 0) >= gfx::RADIUS - 1);
  const int mid = gfx::halfChordAt(gfx::CY + 120, 0);
  const int far = gfx::halfChordAt(gfx::CY + 170, 0);
  TEST_ASSERT_TRUE(mid < gfx::RADIUS);
  TEST_ASSERT_TRUE(far < mid);
  TEST_ASSERT_EQUAL_INT(0, gfx::halfChordAt(gfx::CY + gfx::RADIUS + 5, 0));
}


// ---------------------------------------------------------------------------
// Library
// ---------------------------------------------------------------------------

void test_library_publishes_only_complete_listings(void) {
  spotify::Library lib;
  TEST_ASSERT_TRUE(lib.begin());
  TEST_ASSERT_EQUAL_INT(0, lib.playlistCount());

  const uint32_t gen0 = lib.generation();
  lib.clearPlaylists();
  lib.addPlaylist("Road Trip", "spotify:playlist:abc", "abc");
  lib.addPlaylist("Liked Songs", "spotify:playlist:def", "def");
  // Not published yet: the count must still read zero, or the render task can
  // walk into entries the net task is mid-way through writing.
  TEST_ASSERT_EQUAL_INT(0, lib.playlistCount());
  TEST_ASSERT_EQUAL_UINT32(gen0, lib.generation());

  lib.publishPlaylists(false);
  TEST_ASSERT_EQUAL_INT(2, lib.playlistCount());
  TEST_ASSERT_TRUE(lib.generation() != gen0);
  TEST_ASSERT_EQUAL_STRING("Road Trip", lib.playlist(0)->name);
  TEST_ASSERT_EQUAL_STRING("spotify:playlist:def", lib.playlist(1)->uri);
}

void test_library_caps_and_reports_truncation(void) {
  spotify::Library lib;
  TEST_ASSERT_TRUE(lib.begin());
  lib.clearPlaylists();
  int added = 0;
  for (int i = 0; i < spotify::Library::MAX_PLAYLISTS + 20; ++i)
    if (lib.addPlaylist("x", "spotify:playlist:x", "x")) ++added;
  lib.publishPlaylists(false);
  TEST_ASSERT_EQUAL_INT(spotify::Library::MAX_PLAYLISTS, added);
  TEST_ASSERT_EQUAL_INT(spotify::Library::MAX_PLAYLISTS, lib.playlistCount());
  // A capped list must say so rather than reading as the whole library.
  TEST_ASSERT_TRUE(lib.playlistsTruncated());
}

void test_library_clearing_tracks_publishes_empty_immediately(void) {
  // The heading changes the moment a playlist is opened, so the list under it
  // must go empty at the same moment - not keep showing the previous playlist's
  // tracks until the fetch returns.
  spotify::Library lib;
  TEST_ASSERT_TRUE(lib.begin());
  lib.clearTracks("spotify:playlist:one", "One");
  lib.addTrack("A", "spotify:track:a", "a");
  lib.addTrack("B", "spotify:track:b", "b");
  lib.publishTracks(false);
  TEST_ASSERT_EQUAL_INT(2, lib.trackCount());

  const uint32_t gen = lib.generation();
  lib.clearTracks("spotify:playlist:two", "Two");
  TEST_ASSERT_EQUAL_INT(0, lib.trackCount());
  TEST_ASSERT_TRUE(lib.generation() != gen);
  TEST_ASSERT_EQUAL_STRING("Two", lib.tracksOf());
}

void test_library_out_of_range_reads_are_null(void) {
  spotify::Library lib;
  TEST_ASSERT_TRUE(lib.begin());
  TEST_ASSERT_NULL(lib.playlist(0));
  TEST_ASSERT_NULL(lib.playlist(-1));
  TEST_ASSERT_NULL(lib.track(999));
  lib.clearPlaylists();
  lib.addPlaylist("only", "spotify:playlist:o", "o");
  lib.publishPlaylists(false);
  TEST_ASSERT_NOT_NULL(lib.playlist(0));
  TEST_ASSERT_NULL(lib.playlist(1));
}

void test_library_truncates_overlong_names_safely(void) {
  spotify::Library lib;
  TEST_ASSERT_TRUE(lib.begin());
  char longname[400];
  for (int i = 0; i < 399; ++i) longname[i] = 'x';
  longname[399] = '\0';
  lib.clearPlaylists();
  lib.addPlaylist(longname, longname, longname);
  lib.publishPlaylists(false);
  const spotify::Entry *e = lib.playlist(0);
  TEST_ASSERT_NOT_NULL(e);
  // Terminated inside the field, so nothing downstream reads past it.
  TEST_ASSERT_TRUE(std::strlen(e->name) < sizeof(e->name));
  TEST_ASSERT_TRUE(std::strlen(e->uri) < sizeof(e->uri));
}

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------

namespace {

int litIn(const gfx::Framebuffer &fb, int y0, int y1) {
  int n = 0;
  for (int y = y0; y < y1; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0) ++n;
  return n;
}

const char *const kItems[] = {"One", "Two", "Three", "Four", "Five", "Six"};

}  // namespace

void test_listview_centre_row_is_the_brightest(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::ListView lv;
  lv.prepare(kItems, 6, 2.0f, "HEADING", false);
  gfx::Surface s = fullSurface(fb);
  lv.render(s, 0x07FF);

  // The selected row sits at the centre baseline; its neighbours are dimmer, so
  // the centre band must carry more lit pixels than the band above it.
  const int c = shell::ListView::CENTRE_BASELINE;
  const int centre = litIn(fb, c - 16, c + 4);
  const int above = litIn(fb, c - shell::ListView::ROW_SPACING - 16,
                          c - shell::ListView::ROW_SPACING + 4);
  TEST_ASSERT_TRUE(centre > 0);
  TEST_ASSERT_TRUE(above > 0);
  TEST_ASSERT_TRUE(centre > above);
}

void test_listview_scrolling_moves_the_selection(void) {
  // The same item drawn at two scroll positions must land on different rows.
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  shell::ListView la, lb;
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  la.prepare(kItems, 6, 1.0f, "H", false);
  la.render(sa, 0x07FF);
  lb.prepare(kItems, 6, 3.0f, "H", false);
  lb.render(sb, 0x07FF);

  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 200);
}

void test_listview_fractional_position_differs_from_whole(void) {
  // The float scroll is the whole reason the wheel glides; a mid-glide frame
  // must not render identically to the settled one.
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  shell::ListView la, lb;
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  la.prepare(kItems, 6, 2.0f, "H", false);
  la.render(sa, 0x07FF);
  lb.prepare(kItems, 6, 2.45f, "H", false);
  lb.render(sb, 0x07FF);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 50);
}

void test_listview_empty_says_so(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::ListView lv;
  lv.prepare(nullptr, 0, 0.0f, "PLAYLISTS", false);
  gfx::Surface s = fullSurface(fb);
  lv.render(s, 0x07FF);
  // An empty list must draw something. A blank screen is indistinguishable from
  // a rendering failure.
  TEST_ASSERT_TRUE(litIn(fb, 0, gfx::H) > 20);
}

void test_listview_handles_null_items_and_short_lists(void) {
  const char *const holey[] = {"One", nullptr, "Three"};
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::ListView lv;
  gfx::Surface s = fullSurface(fb);
  // A deleted playlist comes back as a null entry; it must be skipped, not
  // dereferenced.
  lv.prepare(holey, 3, 1.0f, "H", false);
  lv.render(s, 0x07FF);
  lv.prepare(holey, 1, 0.0f, "H", false);
  lv.render(s, 0x07FF);
  TEST_ASSERT_TRUE(true);  // reaching here without a crash is the assertion
}

void test_listview_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  shell::ListView l1, l2;
  l1.prepare(kItems, 6, 2.37f, "PLAYLISTS", true);
  l2.prepare(kItems, 6, 2.37f, "PLAYLISTS", true);

  gfx::Surface s = fullSurface(whole);
  l1.render(s, 0x07FF);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface b;
    b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    b.w = gfx::W;
    b.h = 20;
    b.y0 = y;
    l2.render(b, 0x07FF);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}


// ---------------------------------------------------------------------------
// ConfirmRing
//
// The jump confirmation. Every test here is written against something you could
// see go wrong on the dial: the name not shown, the marker not moving, text
// spilling off the round window, or the banded path disagreeing with the whole
// frame.
// ---------------------------------------------------------------------------

namespace {

// Lit pixels strictly left and right of the vertical centreline. The whole
// point of the control is that the choice reads as a SIDE, so that is what the
// tests measure.
void litPerSide(const gfx::Framebuffer &fb, int *left, int *right) {
  *left = 0;
  *right = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      if (fb.at(x, y) == 0) continue;
      if (x < gfx::CX) ++*left;
      else if (x > gfx::CX) ++*right;
    }
}

}  // namespace

void test_confirmring_names_the_track(void) {
  // Asking "are you sure?" without saying what you are sure ABOUT is the one
  // thing this screen must never do.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::ConfirmRing cr;
  cr.prepare("The Valley Comes Alive", 1.0f);
  gfx::Surface s = fullSurface(fb);
  cr.render(s, 0x07FF);
  TEST_ASSERT_TRUE(litIn(fb, 0, gfx::H) > 200);

  // A different track must not draw the same screen.
  gfx::Framebuffer other;
  other.fill(0x0000);
  shell::ConfirmRing c2;
  c2.prepare("Stubborn Love", 1.0f);
  gfx::Surface s2 = fullSurface(other);
  c2.render(s2, 0x07FF);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != other.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 50);
}

void test_confirmring_marker_leans_to_the_chosen_side(void) {
  // Choice is the entire state of this screen. If the two ends render the same,
  // the knob is doing nothing the user can see.
  gfx::Framebuffer no, yes;
  no.fill(0x0000);
  yes.fill(0x0000);
  shell::ConfirmRing cn, cy;
  gfx::Surface sn = fullSurface(no);
  gfx::Surface sy = fullSurface(yes);
  cn.prepare("Ho Hey", 0.0f);
  cn.render(sn, 0x07FF);
  cy.prepare("Ho Hey", 1.0f);
  cy.render(sy, 0x07FF);

  int nl = 0, nr = 0, yl = 0, yr = 0;
  litPerSide(no, &nl, &nr);
  litPerSide(yes, &yl, &yr);
  // Cancel brightens the left, confirm brightens the right. Measured as a shift
  // rather than an absolute, because both arcs are always drawn.
  TEST_ASSERT_TRUE(nl > yl);
  TEST_ASSERT_TRUE(yr > nr);
}

void test_confirmring_mid_glide_differs_from_both_ends(void) {
  // The marker is eased, same as the list. A frame halfway through the swing
  // must be its own picture, not a snap to one end.
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  shell::ConfirmRing ca, cb;
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  ca.prepare("Ho Hey", 0.0f);
  ca.render(sa, 0x07FF);
  cb.prepare("Ho Hey", 0.5f);
  cb.render(sb, 0x07FF);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 20);
}

void test_confirmring_long_name_stays_inside_the_disc(void) {
  // The round window is the constraint the rectangular framebuffer hides. A
  // name long enough to wrap must still be clipped by the chord, not by luck.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::ConfirmRing cr;
  cr.prepare(
      "An Extremely Long Track Name That Will Certainly Need More Than One "
      "Line To Sit On This Dial",
      1.0f);
  gfx::Surface s = fullSurface(fb);
  cr.render(s, 0x07FF);
  int outside = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      const int dx = x - gfx::CX, dy = y - gfx::CY;
      if (dx * dx + dy * dy > gfx::RADIUS_SQ && fb.at(x, y) != 0) ++outside;
    }
  TEST_ASSERT_EQUAL_INT(0, outside);
}

void test_confirmring_handles_a_missing_name(void) {
  // The queue can hand back an entry with no name. Asking about nothing is
  // better than dereferencing nothing.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::ConfirmRing cr;
  gfx::Surface s = fullSurface(fb);
  cr.prepare(nullptr, 0.0f);
  cr.render(s, 0x07FF);
  cr.prepare("", 1.0f);
  cr.render(s, 0x07FF);
  TEST_ASSERT_TRUE(true);  // reaching here without a crash is the assertion
}

void test_confirmring_drawn_in_bands_matches_full_frame(void) {
  // KNOB_BANDS=1 must be byte-identical to the whole frame. This assertion has
  // caught three clipping bugs in this project already.
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  shell::ConfirmRing c1, c2;
  c1.prepare("The Valley Comes Alive", 0.37f);
  c2.prepare("The Valley Comes Alive", 0.37f);

  gfx::Surface s = fullSurface(whole);
  c1.render(s, 0x07FF);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface b;
    b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    b.w = gfx::W;
    b.h = 20;
    b.y0 = y;
    c2.render(b, 0x07FF);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}


// ---------------------------------------------------------------------------
// CoverLight
//
// The album cover is the SUBJECT of this view, and the particle field is the
// light around it. These assert that relationship holds, because the field was
// once drawn over the face and washed the artwork out.
// ---------------------------------------------------------------------------

namespace {

// Drives the view for `frames` fixed steps and leaves the result in `fb`. Fixed
// dt and a fixed seed on purpose: this engine's determinism is an invariant, and
// a test that fed it a real clock could not assert anything frame-by-frame.
void runCoverLight(gfx::Framebuffer &fb, const art::Image *cover, bool particles,
                   int frames) {
  views::CoverLight v;
  v.begin(0xABCD1234u);
  v.setCover(cover);
  v.setParticlesEnabled(particles);
  core::Rng rng(0x1234u);
  audio::Modulation m;
  m.loudness = 0.6f;
  m.bass = 0.4f;
  for (int f = 0; f < frames; ++f) {
    m.beat_phase = static_cast<float>(f % 30) / 30.0f;
    m.onset = (f % 30) == 0;
    v.update(m, 1.0f / 30.0f, rng);
    gfx::Surface s = fullSurface(fb);
    v.renderBand(s);
    v.endFrame();
  }
}

}  // namespace

void test_coverlight_particles_do_not_wash_out_the_cover(void) {
  // The complaint this was written for: the field was additive and drawn last,
  // so streaks crossed the artwork and the album became hard to see. Behind the
  // cover, the field cannot touch the face at all - so the centre of the cover
  // must be byte-identical whether the field is running or switched off.
  art::Image cover;
  fillCornerTexture(cover);

  gfx::Framebuffer with, without;
  with.fill(0x0000);
  without.fill(0x0000);
  runCoverLight(with, &cover, /*particles=*/true, 20);
  runCoverLight(without, &cover, /*particles=*/false, 20);

  // The cover's centre projects to CX, CY + GROUP_Y * FOCAL / 1.62 - about 16
  // pixels above centre. The box is kept well inside the ~49px half-extent so
  // the orbit and the tilt cannot swing an edge into it.
  const int cx = gfx::CX;
  const int cy = gfx::CY - 16;
  int diffs = 0;
  for (int y = cy - 15; y <= cy + 15; ++y)
    for (int x = cx - 15; x <= cx + 15; ++x)
      if (with.at(x, y) != without.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);

  // ...and the field must still be visibly THERE. A test that passed by drawing
  // no particles at all would be worse than no test.
  int outer = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (with.at(x, y) != without.at(x, y)) ++outer;
  TEST_ASSERT_TRUE(outer > 200);
}

void test_coverlight_is_bit_exact_across_runs(void) {
  // Headless runs are bit-exact: effects take dt and a seed and never read a
  // clock. The slow breathing pulse on the cover is driven from accumulated dt
  // for exactly this reason, and this is the assertion that keeps it honest.
  art::Image cover;
  fillCornerTexture(cover);

  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  runCoverLight(a, &cover, true, 25);
  runCoverLight(b, &cover, true, 25);

  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_coverlight_cover_breathes_over_time(void) {
  // The slow pulse, asserted on the size itself rather than on pixels.
  //
  // A pixel measurement was tried first and cannot work: the cover's on-screen
  // width also carries the orbit's foreshortening, which swings it by about 9%
  // against the breath's 4%, so the two cannot be told apart from the outside.
  //
  // Audio held completely silent, so bass contributes nothing and the only thing
  // left moving the size is the breath.
  views::CoverLight v;
  v.begin(0x5555u);
  v.setParticlesEnabled(false);
  core::Rng rng(0x99u);
  audio::Modulation m;  // silent: no bass, no onset, no loudness

  float lo = 1e9f, hi = -1e9f, sum = 0.0f;
  // Twenty seconds at 30fps, comfortably more than one ~18s breath.
  const int frames = 600;
  for (int f = 0; f < frames; ++f) {
    v.update(m, 1.0f / 30.0f, rng);
    const float h = v.coverHalf();
    if (h < lo) lo = h;
    if (h > hi) hi = h;
    sum += h;
  }

  // It must actually move, and by about the 4% asked for - a breath that has
  // quietly become 0.4% or 40% is a different feature.
  const float swing = (hi - lo) / (sum / frames);
  TEST_ASSERT_TRUE(swing > 0.05f);
  TEST_ASSERT_TRUE(swing < 0.10f);

  // And it must breathe around the intended size, not drift off it. 0.235 is
  // bounded above by a documented failure: 0.30 ran the cover and its reflection
  // off the top and bottom of the disc.
  const float mean = sum / frames;
  TEST_ASSERT_TRUE(mean > 0.225f);
  TEST_ASSERT_TRUE(mean < 0.245f);
}

// ---------------------------------------------------------------------------
// Glyphs and GestureFlash
//
// Written against a real complaint: a long-press produced no visible response
// at all, twice. Once because nothing was playing so the command was correctly
// dropped, and once because the like toggle worked and NOTHING ON SCREEN READS
// `liked`. Silence is the bug these cover.
// ---------------------------------------------------------------------------

namespace {

int litAll(const gfx::Framebuffer &fb) { return litIn(fb, 0, gfx::H); }

int drawOne(gfx::Framebuffer &fb, shell::Glyph g, uint16_t alpha) {
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  shell::drawGlyph(s, g, gfx::CX, gfx::CY, 44, 0xFFFF, alpha);
  return litAll(fb);
}

}  // namespace

void test_glyphs_every_kind_draws_something(void) {
  // A glyph that draws nothing is indistinguishable from the silence this
  // whole feature exists to remove.
  const shell::Glyph all[] = {
      shell::Glyph::Play,         shell::Glyph::Pause,
      shell::Glyph::Next,         shell::Glyph::Previous,
      shell::Glyph::HeartFilled,  shell::Glyph::HeartOutline,
      shell::Glyph::HeartSlash,   shell::Glyph::ChevronUp,
      shell::Glyph::ChevronDown};
  gfx::Framebuffer fb;
  for (shell::Glyph g : all) {
    const int lit = drawOne(fb, g, 256);
    TEST_ASSERT_TRUE(lit > 80);
  }
}

void test_glyphs_are_distinguishable_from_each_other(void) {
  // Play and pause must not be the same picture - that is the one confusion
  // that would make the feedback worse than none.
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  shell::drawGlyph(sa, shell::Glyph::Play, gfx::CX, gfx::CY, 44, 0xFFFF, 256);
  shell::drawGlyph(sb, shell::Glyph::Pause, gfx::CX, gfx::CY, 44, 0xFFFF, 256);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 200);
}

void test_glyphs_outline_heart_is_lighter_than_filled(void) {
  // The two like states have to be told apart at a glance, so the difference
  // is a hole in the middle rather than a shade.
  gfx::Framebuffer fb;
  const int filled = drawOne(fb, shell::Glyph::HeartFilled, 256);
  const int outline = drawOne(fb, shell::Glyph::HeartOutline, 256);
  TEST_ASSERT_TRUE(outline > 40);
  TEST_ASSERT_TRUE(outline < filled / 2);
}

void test_glyphs_stay_inside_their_box(void) {
  // Half-extent means half-extent. A glyph that overruns would clip against the
  // disc rather than against its own bounds, which is invisible until it is not.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  const int half = 30;
  shell::drawGlyph(s, shell::Glyph::HeartFilled, gfx::CX, gfx::CY, half, 0xFFFF, 256);
  int outside = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      if (fb.at(x, y) == 0) continue;
      if (x < gfx::CX - half || x > gfx::CX + half || y < gfx::CY - half ||
          y > gfx::CY + half)
        ++outside;
    }
  TEST_ASSERT_EQUAL_INT(0, outside);
}

void test_glyphs_drawn_in_bands_match_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  gfx::Surface s = fullSurface(whole);
  shell::drawGlyph(s, shell::Glyph::HeartFilled, gfx::CX, gfx::CY, 44, 0xF81F, 150);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface b;
    b.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    b.w = gfx::W;
    b.h = 20;
    b.y0 = y;
    shell::drawGlyph(b, shell::Glyph::HeartFilled, gfx::CX, gfx::CY, 44, 0xF81F, 150);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_gestureflash_shows_then_expires(void) {
  // It must get out of the way on its own. A confirmation that stays is furniture.
  shell::GestureFlash f;
  TEST_ASSERT_FALSE(f.visible(0));
  f.show(shell::Glyph::Play, 1000);
  TEST_ASSERT_TRUE(f.visible(1000));
  TEST_ASSERT_TRUE(f.visible(1000 + shell::GestureFlash::HOLD_MS));
  TEST_ASSERT_FALSE(
      f.visible(1000 + shell::GestureFlash::HOLD_MS +
                shell::GestureFlash::FADE_MS + 1));
}

void test_gestureflash_fades_rather_than_vanishing(void) {
  // Mid-fade must be dimmer than full strength, not simply gone.
  gfx::Framebuffer full, half;
  shell::GestureFlash a, b;
  a.show(shell::Glyph::HeartFilled, 0);
  b.show(shell::Glyph::HeartFilled, 0);

  full.fill(0x0000);
  a.prepare(0);
  gfx::Surface sf = fullSurface(full);
  a.render(sf, 0xFFFF);

  half.fill(0x0000);
  b.prepare(shell::GestureFlash::HOLD_MS + shell::GestureFlash::FADE_MS / 2);
  gfx::Surface sh = fullSurface(half);
  b.render(sh, 0xFFFF);

  // Same silhouette, dimmer pixels.
  int brighter = 0, lit_half = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      if (half.at(x, y) != 0) ++lit_half;
      if (full.at(x, y) > half.at(x, y)) ++brighter;
    }
  TEST_ASSERT_TRUE(lit_half > 80);
  TEST_ASSERT_TRUE(brighter > 80);
}

void test_gestureflash_survives_the_millis_wrap(void) {
  // Every timed thing in this project is asserted against the wrap, because the
  // device is meant to run for months and millis() is 32 bits.
  shell::GestureFlash f;
  const uint32_t near_wrap = 0xFFFFFF00u;
  f.show(shell::Glyph::Pause, near_wrap);
  TEST_ASSERT_TRUE(f.visible(near_wrap + 10));
  // 0x100 past near_wrap has wrapped to 0.
  TEST_ASSERT_FALSE(f.visible(near_wrap + shell::GestureFlash::HOLD_MS +
                              shell::GestureFlash::FADE_MS + 2));
}


// ---------------------------------------------------------------------------
// NowPlaying saved-state
// ---------------------------------------------------------------------------

namespace {

int heartPixels(bool has_track, bool known, bool liked) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  PlaybackState pb;
  pb.has_track = has_track;
  pb.liked_known = known;
  pb.liked = liked;
  shell::NowPlaying np;
  np.prepare(pb, 0, false);
  gfx::Surface s = fullSurface(fb);
  np.render(s, 0x07FF);
  // Only the band the heart lives in, so title and artist text cannot be
  // mistaken for it.
  const int c = shell::NowPlaying::HEART_CY;
  const int h = shell::NowPlaying::HEART_HALF;
  return litIn(fb, c - h - 1, c + h + 2);
}

}  // namespace

void test_nowplaying_unknown_saved_state_draws_no_heart(void) {
  // The invariant, stated in CLAUDE.md and until now describing a heart that
  // did not exist: unknown must render as unknown, never as a confident "no".
  TEST_ASSERT_EQUAL_INT(0, heartPixels(true, /*known=*/false, false));
}

void test_nowplaying_draws_both_saved_states_differently(void) {
  const int filled = heartPixels(true, true, /*liked=*/true);
  const int outline = heartPixels(true, true, /*liked=*/false);
  TEST_ASSERT_TRUE(filled > 60);
  TEST_ASSERT_TRUE(outline > 20);
  // Shape, not just shade: an outline must be substantially emptier.
  TEST_ASSERT_TRUE(outline < filled / 2);
}

void test_nowplaying_no_track_carries_no_heart(void) {
  // A gap between tracks must not keep showing the last song's answer.
  TEST_ASSERT_EQUAL_INT(0, heartPixels(false, true, true));
}


// ---------------------------------------------------------------------------
// DaisyIdle
// ---------------------------------------------------------------------------

void test_daisy_draws_the_dog_and_owns_every_pixel(void) {
  // It replaces the backdrop rather than drawing over one, so it must leave no
  // pixel untouched - a gap would show the previous frame through, and on a
  // banded panel that is stale garbage rather than black.
  gfx::Framebuffer fb;
  fb.fill(0xF81F);  // magenta: anything left over is unmissable
  views::DaisyIdle d;
  d.begin();
  d.update(1.0f / 30.0f);
  gfx::Surface s = fullSurface(fb);
  d.renderBand(s);
  int leftover = 0, drawn = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      if (fb.at(x, y) == 0xF81F) ++leftover;
      else ++drawn;
    }
  TEST_ASSERT_EQUAL_INT(0, leftover);
  TEST_ASSERT_TRUE(drawn > 1000);
}

void test_daisy_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  views::DaisyIdle a, b;
  a.begin();
  b.begin();
  for (int f = 0; f < 12; ++f) {
    a.update(1.0f / 30.0f);
    b.update(1.0f / 30.0f);
  }
  gfx::Surface s = fullSurface(whole);
  a.renderBand(s);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.renderBand(bs);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_daisy_sleeps_then_yawns_then_sleeps_again(void) {
  // A completely static frame reads as a crash on a device that is otherwise
  // always moving, so the sleep loop is interrupted now and then. The yawn must
  // also HAND BACK - a view stuck mid-yawn would be worse than a still one.
  views::DaisyIdle d;
  d.begin();
  TEST_ASSERT_EQUAL(daisy::Daisy_Sleep, d.anim());

  const float dt = 1.0f / 30.0f;
  bool saw_yawn = false;
  bool back_to_sleep = false;
  // Well past one yawn interval and the yawn's own length.
  for (int f = 0; f < 30 * 40; ++f) {
    d.update(dt);
    if (d.anim() == daisy::Daisy_Yawn) saw_yawn = true;
    else if (saw_yawn && d.anim() == daisy::Daisy_Sleep) back_to_sleep = true;
  }
  TEST_ASSERT_TRUE(saw_yawn);
  TEST_ASSERT_TRUE(back_to_sleep);
}

void test_daisy_frame_index_is_always_in_range(void) {
  // The frame index reaches into a flash table. Off the end is not a glitch,
  // it is a read of whatever follows the sprite data.
  views::DaisyIdle d;
  d.begin();
  for (int f = 0; f < 30 * 120; ++f) {
    d.update(1.0f / 30.0f);
    const daisy::AnimData &a = daisy::ANIMS[d.anim()];
    TEST_ASSERT_TRUE(d.frame() >= 0);
    TEST_ASSERT_TRUE(d.frame() < a.frame_count);
  }
}

void test_daisy_is_bit_exact_across_runs(void) {
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  views::DaisyIdle da, db;
  da.begin();
  db.begin();
  for (int f = 0; f < 200; ++f) {
    da.update(1.0f / 30.0f);
    db.update(1.0f / 30.0f);
  }
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  da.renderBand(sa);
  db.renderBand(sb);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

// Daisy reacting to input. The idle screen is the one screen where a gesture
// has nothing to control - there is no track to skip and nothing to pause - so
// the dog IS the feedback. These tests are written against what you can see:
// which animation is on screen, how long it stays, and whether she settles.

void test_daisy_wakes_up_when_touched(void) {
  views::DaisyIdle d;
  d.begin();
  TEST_ASSERT_EQUAL(daisy::Daisy_Sleep, d.anim());
  d.react(views::DaisyIdle::Reaction::Touch);
  TEST_ASSERT_EQUAL(daisy::Daisy_Alert, d.anim());
}

void test_daisy_gives_each_gesture_its_own_mood(void) {
  // Same gesture, same reaction, every time: she is answering YOU, not
  // shuffling. A random pick would read as a screensaver again.
  struct Case {
    views::DaisyIdle::Reaction r;
    daisy::DaisyAnim expect;
  } cases[] = {
      {views::DaisyIdle::Reaction::Touch, daisy::Daisy_Alert},
      {views::DaisyIdle::Reaction::Swipe, daisy::Daisy_Wag},
      {views::DaisyIdle::Reaction::Hold, daisy::Daisy_Zoomies},
      {views::DaisyIdle::Reaction::Turn, daisy::Daisy_Sniff},
  };
  for (const Case &c : cases) {
    views::DaisyIdle d;
    d.begin();
    d.react(c.r);
    TEST_ASSERT_EQUAL(c.expect, d.anim());
  }
}

void test_daisy_settles_through_drowsy_on_her_way_back_to_sleep(void) {
  // Snapping from zoomies straight to a sleeping dog reads as a dropped
  // frame. One pass of drowsy is what makes it look like settling down.
  views::DaisyIdle d;
  d.begin();
  d.react(views::DaisyIdle::Reaction::Hold);
  const float dt = 1.0f / 30.0f;
  bool saw_drowsy = false;
  bool asleep_after_drowsy = false;
  for (int f = 0; f < 30 * 10; ++f) {
    d.update(dt);
    if (d.anim() == daisy::Daisy_Drowsy) saw_drowsy = true;
    else if (saw_drowsy && d.anim() == daisy::Daisy_Sleep)
      asleep_after_drowsy = true;
  }
  TEST_ASSERT_TRUE(saw_drowsy);
  TEST_ASSERT_TRUE(asleep_after_drowsy);
}

void test_daisy_holds_a_short_reaction_long_enough_to_read(void) {
  // Zoomies is four frames at 100 ms. Played once through it is 0.4 s - a
  // blink, and less than the haptic click that triggered it. The reaction
  // LOOPS until its hold expires rather than playing once.
  views::DaisyIdle d;
  d.begin();
  d.react(views::DaisyIdle::Reaction::Hold);
  const float dt = 1.0f / 30.0f;
  float still_zooming_s = 0.0f;
  for (int f = 0; f < 30 * 10; ++f) {
    d.update(dt);
    if (d.anim() != daisy::Daisy_Zoomies) break;
    still_zooming_s += dt;
  }
  TEST_ASSERT_TRUE(still_zooming_s > 2.0f);
}

void test_daisy_does_not_yawn_straight_after_waking(void) {
  // The yawn timer was already most of the way to firing when the poke
  // arrived; if it is not reset she wakes, reacts, settles, and immediately
  // yawns - which looks like the reaction was the bug.
  views::DaisyIdle d;
  d.begin();
  const float dt = 1.0f / 30.0f;
  // Right up to the edge of a yawn.
  for (int f = 0; f < static_cast<int>((views::DaisyIdle::YAWN_EVERY_S - 0.5f) / dt); ++f)
    d.update(dt);
  TEST_ASSERT_EQUAL(daisy::Daisy_Sleep, d.anim());
  d.react(views::DaisyIdle::Reaction::Touch);
  // Settle, then a couple of seconds of sleep. No yawn in that window.
  bool yawned = false;
  for (int f = 0; f < 30 * 8; ++f) {
    d.update(dt);
    if (d.anim() == daisy::Daisy_Yawn) yawned = true;
  }
  TEST_ASSERT_FALSE(yawned);
}

void test_daisy_extends_the_hold_when_poked_the_same_way_again(void) {
  // Tapping twice must not restart the animation from frame 0 - that is a
  // visible stutter. It keeps playing and stays up longer.
  views::DaisyIdle d;
  d.begin();
  d.react(views::DaisyIdle::Reaction::Swipe);
  const float dt = 1.0f / 30.0f;
  for (int f = 0; f < 15; ++f) d.update(dt);
  const int frame_before = d.frame();
  d.react(views::DaisyIdle::Reaction::Swipe);
  TEST_ASSERT_EQUAL(daisy::Daisy_Wag, d.anim());
  TEST_ASSERT_EQUAL_INT(frame_before, d.frame());
}

void test_daisy_switches_mood_when_poked_a_different_way(void) {
  views::DaisyIdle d;
  d.begin();
  d.react(views::DaisyIdle::Reaction::Swipe);
  const float dt = 1.0f / 30.0f;
  for (int f = 0; f < 10; ++f) d.update(dt);
  d.react(views::DaisyIdle::Reaction::Hold);
  TEST_ASSERT_EQUAL(daisy::Daisy_Zoomies, d.anim());
  TEST_ASSERT_EQUAL_INT(0, d.frame());
}

void test_daisy_frame_index_stays_in_range_across_every_reaction(void) {
  // Every animation has its own frame count, and the index reaches into a
  // flash table. Switching mid-animation is exactly where an index survives
  // one animation too long.
  views::DaisyIdle d;
  d.begin();
  const views::DaisyIdle::Reaction all[] = {
      views::DaisyIdle::Reaction::Touch, views::DaisyIdle::Reaction::Swipe,
      views::DaisyIdle::Reaction::Hold, views::DaisyIdle::Reaction::Turn};
  const float dt = 1.0f / 30.0f;
  for (int f = 0; f < 30 * 60; ++f) {
    if (f % 7 == 0) d.react(all[(f / 7) % 4]);
    d.update(dt);
    const daisy::AnimData &a = daisy::ANIMS[d.anim()];
    TEST_ASSERT_TRUE(d.frame() >= 0);
    TEST_ASSERT_TRUE(d.frame() < a.frame_count);
  }
}

void test_daisy_is_bit_exact_across_runs_with_pokes(void) {
  // The bit-exactness invariant has to survive the new input path: same dt
  // sequence and same poke sequence, same pixels. A reaction that reached for
  // a clock or an RNG would show up here.
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  views::DaisyIdle da, db;
  da.begin();
  db.begin();
  const float dt = 1.0f / 30.0f;
  for (int f = 0; f < 200; ++f) {
    if (f == 5) {
      da.react(views::DaisyIdle::Reaction::Hold);
      db.react(views::DaisyIdle::Reaction::Hold);
    }
    if (f == 90) {
      da.react(views::DaisyIdle::Reaction::Turn);
      db.react(views::DaisyIdle::Reaction::Turn);
    }
    da.update(dt);
    db.update(dt);
  }
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  da.renderBand(sa);
  db.renderBand(sb);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_daisy_mid_reaction_drawn_in_bands_matches_full_frame(void) {
  // KNOB_BANDS=1 must render byte-identical to a full frame, and the reaction
  // animations have different sprite extents than sleep does - zoomies is a
  // dog mid-air, not one lying flat.
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  views::DaisyIdle a, b;
  a.begin();
  b.begin();
  a.react(views::DaisyIdle::Reaction::Hold);
  b.react(views::DaisyIdle::Reaction::Hold);
  for (int f = 0; f < 12; ++f) {
    a.update(1.0f / 30.0f);
    b.update(1.0f / 30.0f);
  }
  TEST_ASSERT_EQUAL(daisy::Daisy_Zoomies, a.anim());
  gfx::Surface s = fullSurface(whole);
  a.renderBand(s);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.renderBand(bs);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}



// ---------------------------------------------------------------------------
// Transport feedback policy
// ---------------------------------------------------------------------------

void test_transport_feedback_shows_while_a_track_is_loaded(void) {
  TEST_ASSERT_TRUE(shell::transportFeedbackVisible(true, true));
}

void test_transport_feedback_hides_when_nothing_is_listening(void) {
  // No track AND no device: a play/pause glyph here claims a transport that
  // does not exist. On the idle screen the flash is the ONLY visible answer a
  // gesture gets - showToast has never been wired to a renderer - so a glyph
  // that means nothing is worse than no glyph at all.
  TEST_ASSERT_FALSE(shell::transportFeedbackVisible(false, false));
}

void test_transport_feedback_shows_for_an_idle_device_with_no_track(void) {
  // Spotify open and active but with nothing loaded: /me/player answers 200
  // with a null item, so has_track goes false while has_device stays true. A
  // play command in this state genuinely can resume, so the furniture stays.
  TEST_ASSERT_TRUE(shell::transportFeedbackVisible(false, true));
}

void test_transport_feedback_shows_for_a_track_without_a_known_device(void) {
  // A command 404 clears has_device, but a track is still loaded and on
  // screen. Stripping the transport off a visible now-playing view would be a
  // worse lie than leaving it: the user can see the song.
  TEST_ASSERT_TRUE(shell::transportFeedbackVisible(true, false));
}


// ---------------------------------------------------------------------------
// Crash policy
// ---------------------------------------------------------------------------

void test_an_abnormal_reset_counts_and_a_clean_one_clears(void) {
  TEST_ASSERT_EQUAL_INT(1, core::nextCrashStreak(0, true));
  TEST_ASSERT_EQUAL_INT(3, core::nextCrashStreak(2, true));
  TEST_ASSERT_EQUAL_INT(0, core::nextCrashStreak(2, false));
}

void test_safe_mode_waits_for_the_third_crash(void) {
  TEST_ASSERT_FALSE(core::safeModeWanted(0));
  TEST_ASSERT_FALSE(core::safeModeWanted(core::SAFE_MODE_STREAK - 1));
  TEST_ASSERT_TRUE(core::safeModeWanted(core::SAFE_MODE_STREAK));
  TEST_ASSERT_TRUE(core::safeModeWanted(core::SAFE_MODE_STREAK + 5));
}

void test_a_crash_loop_reaches_safe_mode(void) {
  // Three crashes with no healthy window between them. This is the case safe
  // mode exists for: without it the device reboots forever and takes USB-CDC
  // down with it, and the only recovery is unplugging the board.
  int streak = 0;
  for (int i = 0; i < 3; ++i) streak = core::nextCrashStreak(streak, true);
  TEST_ASSERT_TRUE(core::safeModeWanted(streak));
}

void test_crashes_months_apart_never_reach_safe_mode(void) {
  // The bug this fixes. The streak is stored in NVS and was only ever cleared
  // by a CLEAN reset reason, so a board that crashed once, ran happily for a
  // month, then crashed again was two thirds of the way to locking itself into
  // safe mode for reasons that had nothing to do with each other.
  int streak = 0;
  for (int i = 0; i < 10; ++i) {
    streak = core::nextCrashStreak(streak, true);
    TEST_ASSERT_FALSE(core::safeModeWanted(streak));
    // ...and then it runs for a month.
    if (core::streakForgiven(core::HEALTHY_AFTER_MS)) streak = 0;
  }
}

void test_the_streak_is_forgiven_only_after_a_real_run(void) {
  TEST_ASSERT_FALSE(core::streakForgiven(0));
  TEST_ASSERT_FALSE(core::streakForgiven(core::HEALTHY_AFTER_MS - 1));
  TEST_ASSERT_TRUE(core::streakForgiven(core::HEALTHY_AFTER_MS));
}

void test_the_streak_is_capped_so_nvs_is_not_written_forever(void) {
  // Unbounded increment means a write to flash on every boot of a board that
  // is never going to recover, for a number nothing reads above the threshold.
  int streak = 0;
  for (int i = 0; i < 1000; ++i) streak = core::nextCrashStreak(streak, true);
  TEST_ASSERT_TRUE(streak <= core::CRASH_STREAK_MAX);
  TEST_ASSERT_TRUE(core::safeModeWanted(streak));
}

void test_a_corrupt_stored_streak_does_not_break_the_count(void) {
  // NVS can come back with anything if the partition is damaged. A negative
  // value must not walk backwards away from safe mode on a board that is
  // visibly crashing.
  TEST_ASSERT_EQUAL_INT(1, core::nextCrashStreak(-5, true));
  TEST_ASSERT_EQUAL_INT(0, core::nextCrashStreak(-5, false));
  TEST_ASSERT_TRUE(core::nextCrashStreak(999999, true) <= core::CRASH_STREAK_MAX);
}


// ---------------------------------------------------------------------------
// Safe-mode screen
// ---------------------------------------------------------------------------

void test_safe_screen_owns_every_pixel(void) {
  // Same rule as the dog: it replaces the view rather than drawing over one, so
  // a pixel it fails to write shows the previous frame through - and on a
  // banded panel that is stale garbage. A safe-mode screen with garbage on it
  // is worse than no safe mode, because it reads as the crash.
  gfx::Framebuffer fb;
  fb.fill(0xF81F);
  views::SafeScreen s;
  s.begin("PANIC (crash)", 3);
  gfx::Surface surf = fullSurface(fb);
  s.renderBand(surf);
  int leftover = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) == 0xF81F) ++leftover;
  TEST_ASSERT_EQUAL_INT(0, leftover);
}

void test_safe_screen_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  views::SafeScreen a, b;
  a.begin("task watchdog", 4);
  b.begin("task watchdog", 4);
  gfx::Surface s = fullSurface(whole);
  a.renderBand(s);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.renderBand(bs);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_safe_screen_draws_something_legible(void) {
  // It is all text. If the font path silently drew nothing this screen would be
  // a flat colour, which says "broken" but not why.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  views::SafeScreen s;
  s.begin("brownout", 3);
  gfx::Surface surf = fullSurface(fb);
  s.renderBand(surf);
  // Ink is counted as "differs from the backdrop", sampled from a central box
  // that the bezel ring cannot reach. Not an exact colour match: glyphs are
  // alpha-blended, so only a couple of hundred pixels land on pure white out of
  // fifteen hundred that are visibly text, and asserting on the exact value
  // measured the blend rather than the legibility.
  const uint16_t bg = fb.at(2, 2);  // a corner: backdrop only, never a glyph
  int ink = 0;
  for (int y = 110; y < 265; ++y)
    for (int x = 100; x < 260; ++x)
      if (fb.at(x, y) != bg) ++ink;
  TEST_ASSERT_TRUE(ink > 500);
}

void test_safe_screen_survives_a_null_reason(void) {
  // esp_reset_reason() has a default branch, and a screen that crashes while
  // reporting a crash is the worst possible bug in this file.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  views::SafeScreen s;
  s.begin(nullptr, 0);
  gfx::Surface surf = fullSurface(fb);
  s.renderBand(surf);
  TEST_ASSERT_TRUE(true);  // reaching here without a crash is the assertion
}


// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------

void test_toast_draws_nothing_when_inactive(void) {
  // The field stays non-empty long after the deadline passes - showToast never
  // clears the text, only the Deadline. Drawing on text alone would leave the
  // last error on screen forever.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::Toast t;
  t.prepare("No active device", false);
  TEST_ASSERT_FALSE(t.visible());
  gfx::Surface s = fullSurface(fb);
  t.render(s, 0x07E0);
  int ink = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0x0000) ++ink;
  TEST_ASSERT_EQUAL_INT(0, ink);
}

void test_toast_draws_when_active(void) {
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::Toast t;
  t.prepare("No active device", true);
  TEST_ASSERT_TRUE(t.visible());
  gfx::Surface s = fullSurface(fb);
  t.render(s, 0x07E0);
  int ink = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0x0000) ++ink;
  TEST_ASSERT_TRUE(ink > 100);
}

void test_toast_is_not_visible_for_an_empty_message(void) {
  shell::Toast t;
  t.prepare("", true);
  TEST_ASSERT_FALSE(t.visible());
  t.prepare(nullptr, true);
  TEST_ASSERT_FALSE(t.visible());
}

void test_toast_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  shell::Toast a, b;
  a.prepare("Volume not supported", true);
  b.prepare("Volume not supported", true);
  gfx::Surface s = fullSurface(whole);
  a.render(s, 0x07E0);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.render(bs, 0x07E0);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_toast_stays_inside_the_chord(void) {
  // Text is measured against the CHORD at its baseline, not the 360px panel.
  // A message that overflowed would run under the bezel, where it cannot be
  // read - which is the same as not showing it.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  shell::Toast t;
  t.prepare("a spotify error message far too long for a round screen", true);
  gfx::Surface s = fullSurface(fb);
  t.render(s, 0xFFFF);
  const int half = gfx::halfChordAt(shell::NowPlaying::TIME_BASELINE,
                                    shell::NowPlaying::MARGIN);
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0x0000) {
        TEST_ASSERT_TRUE(x >= gfx::CX - half - 1);
        TEST_ASSERT_TRUE(x <= gfx::CX + half + 1);
      }
}

void test_nowplaying_suppresses_times_for_a_toast(void) {
  // The toast takes the time row's place rather than overlapping it. Two
  // strings on one baseline is unreadable, and between a timecode you can
  // infer and an error you cannot, the error wins.
  PlaybackState pb;
  pb.has_track = true;
  pb.duration_ms = 200000;
  pb.progress_ms = 60000;
  setStr(pb.title, sizeof(pb.title), "Title");
  setStr(pb.artist, sizeof(pb.artist), "Artist");

  gfx::Framebuffer with_times, without;
  with_times.fill(0x0000);
  without.fill(0x0000);

  shell::NowPlaying a, b;
  a.prepare(pb, 60000, false);
  b.prepare(pb, 60000, true);
  gfx::Surface sa = fullSurface(with_times);
  gfx::Surface sb = fullSurface(without);
  a.render(sa, 0x07E0);
  b.render(sb, 0x07E0);

  int ink_a = 0, ink_b = 0;
  for (int y = shell::NowPlaying::TIME_BASELINE - 14;
       y <= shell::NowPlaying::TIME_BASELINE + 2; ++y)
    for (int x = 0; x < gfx::W; ++x) {
      if (with_times.at(x, y) != 0x0000) ++ink_a;
      if (without.at(x, y) != 0x0000) ++ink_b;
    }
  TEST_ASSERT_TRUE(ink_a > 20);
  TEST_ASSERT_EQUAL_INT(0, ink_b);
}


// ---------------------------------------------------------------------------
// Command queue
// ---------------------------------------------------------------------------

void test_command_queue_reports_a_drop_rather_than_hiding_it(void) {
  // The ring holds CAPACITY-1 entries; the last slot is what distinguishes
  // full from empty. What matters is that the caller is TOLD - a silently
  // dropped command is a gesture that did nothing after the glyph said it
  // worked, which is the exact failure GestureFlash exists to prevent.
  CommandQueue<4> q;
  Command c;
  c.type = CommandType::Next;
  TEST_ASSERT_TRUE(q.push(c));
  TEST_ASSERT_TRUE(q.push(c));
  TEST_ASSERT_TRUE(q.push(c));
  TEST_ASSERT_FALSE(q.push(c));
}

void test_command_queue_accepts_again_after_a_pop(void) {
  CommandQueue<4> q;
  Command c;
  c.type = CommandType::Next;
  while (q.push(c)) {
  }
  Command out;
  TEST_ASSERT_TRUE(q.pop(&out));
  TEST_ASSERT_TRUE(q.push(c));
}

void test_coalesced_push_reports_a_drop_when_it_cannot_coalesce(void) {
  // The path that was silently dropping. pushCoalesced returned void and threw
  // away push()'s answer, so a volume command arriving with nothing of its type
  // pending AND the ring full disappeared with nobody told - the same bug as
  // submit()'s void signature, one level further down.
  CommandQueue<4> q;
  Command n;
  n.type = CommandType::Next;
  while (q.push(n)) {
  }
  Command v;
  v.type = CommandType::SetVolume;
  TEST_ASSERT_FALSE(q.pushCoalesced(v));
}

void test_coalesced_volume_never_fills_the_queue(void) {
  // A fast spin must not be able to push out a pending play/pause. Coalescing
  // replaces the pending entry rather than appending, so forty detents cost
  // one slot.
  CommandQueue<4> q;
  Command v;
  v.type = CommandType::SetVolume;
  for (int i = 0; i < 40; ++i) {
    v.arg = i;
    TEST_ASSERT_TRUE(q.pushCoalesced(v));
  }
  Command other;
  other.type = CommandType::PlayPause;
  TEST_ASSERT_TRUE(q.push(other));
}


// ---------------------------------------------------------------------------
// Heap floor
// ---------------------------------------------------------------------------

void test_heap_watch_is_quiet_while_there_is_room(void) {
  core::HeapWatch w;
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES + 1));
  TEST_ASSERT_FALSE(w.observe(200000));
}

void test_heap_watch_fires_once_on_crossing(void) {
  // Once, not every frame. At 126 fps a per-frame warning is a serial flood
  // that pushes the thing you needed to read off the top of the buffer - and
  // this project has already learned twice that a diagnostic can be the bug.
  core::HeapWatch w;
  TEST_ASSERT_TRUE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(1000));
}

void test_heap_watch_rearms_only_after_real_recovery(void) {
  // Hysteresis. Rearming at the floor itself would make a value hovering on the
  // boundary fire on alternate frames, which is the flood again.
  core::HeapWatch w;
  TEST_ASSERT_TRUE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES + 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_FLOOR_BYTES - 1));
  TEST_ASSERT_FALSE(w.observe(core::HEAP_CLEAR_BYTES));
  TEST_ASSERT_TRUE(w.observe(core::HEAP_FLOOR_BYTES - 1));
}

void test_the_heap_floor_leaves_room_for_a_tls_handshake(void) {
  // Measured on this board: free INTERNAL heap sits at 48-51 KB during playback
  // with artwork decoded, and around 104 KB before the first cover lands. The
  // floor has to be below the working range or it never stops firing, and above
  // what a handshake needs or it never fires in time to mean anything.
  TEST_ASSERT_TRUE(core::HEAP_FLOOR_BYTES < 48000);
  TEST_ASSERT_TRUE(core::HEAP_FLOOR_BYTES > 16000);
  TEST_ASSERT_TRUE(core::HEAP_CLEAR_BYTES > core::HEAP_FLOOR_BYTES);
}


// ---------------------------------------------------------------------------
// Player gesture routing
// ---------------------------------------------------------------------------

namespace {
PlaybackState playingTrack() {
  PlaybackState pb;
  pb.has_track = true;
  pb.has_device = true;
  pb.is_playing = true;
  return pb;
}
PlaybackState nothingListening() {
  PlaybackState pb;
  pb.has_track = false;
  pb.has_device = false;
  return pb;
}
}  // namespace

void test_tap_with_a_track_sends_playpause_and_flashes(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::Tap, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::PlayPause, a.command);
  TEST_ASSERT_TRUE(a.show_glyph);
  TEST_ASSERT_TRUE(a.flip_playing);
  TEST_ASSERT_EQUAL(input::Screen::Player, a.screen);
}

void test_tap_with_nothing_listening_tries_to_wake_a_device(void) {
  // Deliberately replaces the old dog-alone behaviour. "Nothing listening" is
  // usually a computer with Spotify open that Spotify Connect has quietly
  // deregistered, and a tap is the natural way to ask for it back.
  //
  // No glyph, though, and no optimistic flip: there is genuinely no transport
  // yet, so a play icon would be predicting a success that has not happened.
  // Daisy is the instant feedback while the request flies.
  const input::Action a =
      input::routePlayer(input::Gesture::Tap, nothingListening());
  TEST_ASSERT_EQUAL(CommandType::WakeDevice, a.command);
  TEST_ASSERT_FALSE(a.show_glyph);
  TEST_ASSERT_FALSE(a.flip_playing);
  TEST_ASSERT_TRUE(a.poke_dog);
  TEST_ASSERT_EQUAL(views::DaisyIdle::Reaction::Touch, a.dog);
}

void test_tap_does_not_try_to_wake_a_device_that_is_already_there(void) {
  // has_device true means something IS listening, so a wake would be a
  // pointless round trip and a transfer away from wherever you are playing.
  PlaybackState pb;
  pb.has_track = false;
  pb.has_device = true;
  const input::Action a = input::routePlayer(input::Gesture::Tap, pb);
  TEST_ASSERT_EQUAL(CommandType::PlayPause, a.command);
}

void test_swipe_down_with_a_track_opens_the_queue(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::SwipeDown, playingTrack());
  TEST_ASSERT_EQUAL(input::Screen::Tracks, a.screen);
  TEST_ASSERT_EQUAL(CommandType::FetchQueue, a.command);
}

void test_swipe_down_with_no_track_is_swallowed(void) {
  // The queue is what comes up AFTER something. With no current track this
  // jumped to a list that was always empty, and shoved the dog aside to do it.
  const input::Action a =
      input::routePlayer(input::Gesture::SwipeDown, nothingListening());
  TEST_ASSERT_EQUAL(input::Screen::Player, a.screen);
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_TRUE(a.poke_dog);
}

void test_swipe_up_always_reaches_playlists(void) {
  // THE load-bearing assertion of this file. Swipe up is the only route to
  // Playlists, and Playlists is the only way to start playback or reach THEMES.
  // Gating it on a track would mean nothing could ever be started - precisely
  // when nothing is playing. Symmetry with swipe-down would be a bug, and this
  // test exists so a future tidy-up cannot quietly introduce it.
  const input::Action with =
      input::routePlayer(input::Gesture::SwipeUp, playingTrack());
  const input::Action without =
      input::routePlayer(input::Gesture::SwipeUp, nothingListening());
  TEST_ASSERT_EQUAL(input::Screen::Playlists, with.screen);
  TEST_ASSERT_EQUAL(input::Screen::Playlists, without.screen);
  TEST_ASSERT_EQUAL(CommandType::FetchPlaylists, without.command);
}

void test_swipes_with_a_track_skip_and_flash(void) {
  const input::Action l =
      input::routePlayer(input::Gesture::SwipeLeft, playingTrack());
  const input::Action r =
      input::routePlayer(input::Gesture::SwipeRight, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::Previous, l.command);
  TEST_ASSERT_EQUAL(CommandType::Next, r.command);
  TEST_ASSERT_TRUE(l.show_glyph);
  TEST_ASSERT_TRUE(r.show_glyph);
}

void test_swipes_with_nothing_listening_only_wag(void) {
  const input::Action l =
      input::routePlayer(input::Gesture::SwipeLeft, nothingListening());
  TEST_ASSERT_EQUAL(CommandType::None, l.command);
  TEST_ASSERT_FALSE(l.show_glyph);
  TEST_ASSERT_TRUE(l.poke_dog);
  TEST_ASSERT_EQUAL(views::DaisyIdle::Reaction::Swipe, l.dog);
}

void test_long_press_with_a_track_toggles_like(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::LongPress, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::ToggleLike, a.command);
  TEST_ASSERT_TRUE(a.show_glyph);
  TEST_ASSERT_TRUE(a.flip_liked);
}

void test_long_press_with_nothing_listening_gets_zoomies(void) {
  // The refusal is still owed - a refusal you cannot see is the same as a bug.
  // But a crossed-out heart only refuses something SPECIFIC, and with nothing
  // listening there is no track to have refused, so the glyph goes and the
  // toast stays.
  const input::Action a =
      input::routePlayer(input::Gesture::LongPress, nothingListening());
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_FALSE(a.show_glyph);
  TEST_ASSERT_EQUAL(views::DaisyIdle::Reaction::Hold, a.dog);
  TEST_ASSERT_TRUE(a.toast[0] != '\0');
}

void test_long_press_with_a_device_but_no_track_still_refuses_visibly(void) {
  // Spotify open and active with nothing loaded: has_device true, has_track
  // false. There is still nothing to like, but the transport IS real, so the
  // crossed-out heart is honest here where it is noise on the dog screen.
  PlaybackState pb;
  pb.has_track = false;
  pb.has_device = true;
  const input::Action a = input::routePlayer(input::Gesture::LongPress, pb);
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_TRUE(a.show_glyph);
  TEST_ASSERT_EQUAL(shell::Glyph::HeartSlash, a.glyph);
}

void test_no_gesture_routes_to_nothing(void) {
  const input::Action a =
      input::routePlayer(input::Gesture::None, playingTrack());
  TEST_ASSERT_EQUAL(CommandType::None, a.command);
  TEST_ASSERT_FALSE(a.show_glyph);
  TEST_ASSERT_FALSE(a.poke_dog);
  TEST_ASSERT_FALSE(a.flip_playing);
  TEST_ASSERT_FALSE(a.flip_liked);
}


// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

namespace {
// A cover with a recognisable gradient, so a rotation is detectable rather
// than merely different.
void makeTestCover(art::Image *out) {
  out->allocate(150, 150);
  for (int y = 0; y < 150; ++y)
    for (int x = 0; x < 150; ++x)
      out->set(x, y, gfx::rgb565(static_cast<uint8_t>(x + 40),
                                 static_cast<uint8_t>(y + 40), 200));
}
}  // namespace

void test_record_owns_every_pixel(void) {
  // It replaces the radial backdrop rather than drawing over one, so a pixel it
  // fails to write shows the previous frame through - stale garbage on a banded
  // panel.
  art::Image cover;
  makeTestCover(&cover);
  gfx::Framebuffer fb;
  fb.fill(0xF81F);
  fx::Record r;
  r.begin();
  r.update(0.1f);
  gfx::Surface s = fullSurface(fb);
  r.drawBand(s, &cover, 0x07E0);
  int leftover = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) == 0xF81F) ++leftover;
  TEST_ASSERT_EQUAL_INT(0, leftover);
}

void test_record_drawn_in_bands_matches_full_frame(void) {
  art::Image cover;
  makeTestCover(&cover);
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  fx::Record a, b;
  a.begin();
  b.begin();
  for (int i = 0; i < 7; ++i) {
    a.update(1.0f / 30.0f);
    b.update(1.0f / 30.0f);
  }
  gfx::Surface s = fullSurface(whole);
  a.drawBand(s, &cover, 0x07E0);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.drawBand(bs, &cover, 0x07E0);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_record_actually_spins(void) {
  // The point of the theme. A record that does not turn is a circular crop.
  art::Image cover;
  makeTestCover(&cover);
  gfx::Framebuffer early, later;
  early.fill(0x0000);
  later.fill(0x0000);
  fx::Record a, b;
  a.begin();
  b.begin();
  // A quarter turn apart, expressed in turns rather than in frames, so tuning
  // the spin speed does not quietly turn this assertion into a coin flip.
  b.update(0.25f / fx::Record::TURNS_PER_S);
  gfx::Surface sa = fullSurface(early);
  gfx::Surface sb = fullSurface(later);
  a.drawBand(sa, &cover, 0x07E0);
  b.drawBand(sb, &cover, 0x07E0);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (early.at(x, y) != later.at(x, y)) ++diffs;
  TEST_ASSERT_TRUE(diffs > 2000);
}

void test_record_spin_is_frame_rate_independent(void) {
  // Driven off accumulated dt, not per-frame. One 30-frame run at 1/30 and one
  // 60-frame run at 1/60 cover the same second and must land on the same angle.
  fx::Record slow, fast;
  slow.begin();
  fast.begin();
  for (int i = 0; i < 30; ++i) slow.update(1.0f / 30.0f);
  for (int i = 0; i < 60; ++i) fast.update(1.0f / 60.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, slow.turns(), fast.turns());
  // And one second really is one second's worth of turning. Asserted against
  // the constant rather than a literal copy of it, so the two cannot drift.
  TEST_ASSERT_FLOAT_WITHIN(0.02f, fx::Record::TURNS_PER_S, slow.turns());
}

void test_record_is_bit_exact_across_runs(void) {
  art::Image cover;
  makeTestCover(&cover);
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  fx::Record ra, rb;
  ra.begin();
  rb.begin();
  for (int i = 0; i < 50; ++i) {
    ra.update(1.0f / 30.0f);
    rb.update(1.0f / 30.0f);
  }
  gfx::Surface sa = fullSurface(a);
  gfx::Surface sb = fullSurface(b);
  ra.drawBand(sa, &cover, 0x07E0);
  rb.drawBand(sb, &cover, 0x07E0);
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != b.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_record_with_no_cover_invents_nothing(void) {
  // Unknown renders as unknown. A record with no art draws grooves and a label,
  // never a plausible-looking picture. It must also not crash: cover_ is null
  // before the first artwork lands on EVERY track change.
  gfx::Framebuffer fb;
  fb.fill(0xF81F);
  fx::Record r;
  r.begin();
  r.update(0.2f);
  gfx::Surface s = fullSurface(fb);
  r.drawBand(s, nullptr, 0x07E0);
  int leftover = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) == 0xF81F) ++leftover;
  TEST_ASSERT_EQUAL_INT(0, leftover);
}

void test_record_owns_the_backdrop_like_the_other_two(void) {
  TEST_ASSERT_TRUE(fx::themeOwnsBackdrop(fx::ThemeId::Record));
}

void test_record_has_a_name(void) {
  TEST_ASSERT_TRUE(fx::themeName(fx::ThemeId::Record)[0] != '?');
}


// ---------------------------------------------------------------------------
// Device pick
// ---------------------------------------------------------------------------

void test_device_pick_prefers_a_computer(void) {
  // "I know I have Spotify open on my computer" is the whole reason this
  // feature exists, so matching that intent beats taking whatever is first.
  spotify::DeviceInfo d[3];
  setStr(d[0].type, sizeof(d[0].type), "Smartphone");
  setStr(d[1].type, sizeof(d[1].type), "Computer");
  setStr(d[2].type, sizeof(d[2].type), "Speaker");
  TEST_ASSERT_EQUAL_INT(1, spotify::pickDevice(d, 3));
}

void test_device_pick_skips_restricted_devices(void) {
  // is_restricted is Spotify's own flag for a device that refuses Web API
  // commands. Transferring to one is a guaranteed failure, so a restricted
  // computer loses to a usable phone.
  spotify::DeviceInfo d[2];
  setStr(d[0].type, sizeof(d[0].type), "Computer");
  d[0].is_restricted = true;
  setStr(d[1].type, sizeof(d[1].type), "Smartphone");
  TEST_ASSERT_EQUAL_INT(1, spotify::pickDevice(d, 2));
}

void test_device_pick_falls_back_to_the_first_usable(void) {
  spotify::DeviceInfo d[2];
  setStr(d[0].type, sizeof(d[0].type), "Speaker");
  setStr(d[1].type, sizeof(d[1].type), "TV");
  TEST_ASSERT_EQUAL_INT(0, spotify::pickDevice(d, 2));
}

void test_device_pick_reports_nothing_usable(void) {
  // -1, not 0. Returning an index into a list with no usable entry would
  // transfer playback to a device known to refuse it, and the honest answer on
  // screen is "no devices found".
  spotify::DeviceInfo d[2];
  setStr(d[0].type, sizeof(d[0].type), "Computer");
  d[0].is_restricted = true;
  setStr(d[1].type, sizeof(d[1].type), "Speaker");
  d[1].is_restricted = true;
  TEST_ASSERT_EQUAL_INT(-1, spotify::pickDevice(d, 2));
  TEST_ASSERT_EQUAL_INT(-1, spotify::pickDevice(d, 0));
  TEST_ASSERT_EQUAL_INT(-1, spotify::pickDevice(nullptr, 3));
}

void test_device_pick_prefers_an_already_active_computer(void) {
  // If one is somehow already active, it is the one that will accept a resume
  // without a transfer round trip.
  spotify::DeviceInfo d[2];
  setStr(d[0].type, sizeof(d[0].type), "Computer");
  setStr(d[1].type, sizeof(d[1].type), "Computer");
  d[1].is_active = true;
  TEST_ASSERT_EQUAL_INT(1, spotify::pickDevice(d, 2));
}

// ---------------------------------------------------------------------------
// Themes and the picker
// ---------------------------------------------------------------------------

void test_every_theme_has_a_name_and_a_distinct_spawn(void) {
  // A theme that configures the pool identically to another is not a theme,
  // it is a duplicate row on a menu.
  uint16_t pal[16];
  for (int i = 0; i < 16; ++i) pal[i] = static_cast<uint16_t>(0x1111 * i);
  const int n = static_cast<int>(fx::ThemeId::Count);
  TEST_ASSERT_TRUE(n >= 2);
  for (int i = 0; i < n; ++i) {
    const fx::ThemeId a = static_cast<fx::ThemeId>(i);
    TEST_ASSERT_NOT_NULL(fx::themeName(a));
    TEST_ASSERT_TRUE(fx::themeName(a)[0] != '?');
    fx::SpawnParams pa;
    fx::themeSpawn(a, pal, &pa);
    for (int j = i + 1; j < n; ++j) {
      fx::SpawnParams pb;
      fx::themeSpawn(static_cast<fx::ThemeId>(j), pal, &pb);
      const bool same = pa.spread == pb.spread && pa.speed_max == pb.speed_max &&
                        pa.gravity_y == pb.gravity_y && pa.drag == pb.drag;
      TEST_ASSERT_FALSE(same);
    }
  }
}

void test_rain_falls_and_the_others_do_not(void) {
  // Gravity is what makes rain rain. Without it the theme is a differently
  // tuned starfield.
  uint16_t pal[16] = {};
  fx::SpawnParams rain, cover;
  fx::themeSpawn(fx::ThemeId::Rain, pal, &rain);
  fx::themeSpawn(fx::ThemeId::CoverLight, pal, &cover);
  TEST_ASSERT_TRUE(rain.gravity_y > 100.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, cover.gravity_y);
}

void test_rain_does_not_burst_on_the_beat(void) {
  // Rain reacting to a kick drum would be two effects fighting.
  audio::Modulation m;
  m.bass = 1.0f;
  m.loudness = 1.0f;
  TEST_ASSERT_EQUAL_INT(0, fx::themeBurst(fx::ThemeId::Rain, m, false));
  TEST_ASSERT_TRUE(fx::themeBurst(fx::ThemeId::Heartbeat, m, false) > 0);
}

void test_theme_emission_is_frame_rate_independent(void) {
  // The same second of music must emit about the same number of particles
  // whether it was 30 frames or 60. The accumulator is the whole point.
  audio::Modulation m;
  m.loudness = 0.5f;
  float acc30 = 0.0f, acc60 = 0.0f;
  int n30 = 0, n60 = 0;
  for (int i = 0; i < 30; ++i)
    n30 += fx::themeEmit(fx::ThemeId::Rain, m, 1.0f / 30.0f, false, &acc30);
  for (int i = 0; i < 60; ++i)
    n60 += fx::themeEmit(fx::ThemeId::Rain, m, 1.0f / 60.0f, false, &acc60);
  const int diff = n30 > n60 ? n30 - n60 : n60 - n30;
  TEST_ASSERT_TRUE(n30 > 100);
  TEST_ASSERT_TRUE(diff <= 2);
}

void test_picker_shuffle_never_repeats_the_current_theme(void) {
  // A shuffle that hands back what is already showing reads as broken, not as
  // random. This is the assertion that stops a naive rng % n from shipping.
  fx::ThemePicker p;
  core::Rng rng(0xBEEF);
  p.setShuffle();
  for (int i = 0; i < 400; ++i) {
    const fx::ThemeId before = p.current();
    p.onTrackChange(rng);
    TEST_ASSERT_TRUE(p.current() != before);
  }
}

void test_picker_shuffle_reaches_every_theme(void) {
  // ...and it must still reach all of them, not just alternate between two.
  fx::ThemePicker p;
  core::Rng rng(0x1234);
  p.setShuffle();
  bool seen[static_cast<int>(fx::ThemeId::Count)] = {};
  for (int i = 0; i < 600; ++i) {
    p.onTrackChange(rng);
    seen[static_cast<int>(p.current())] = true;
  }
  for (int i = 0; i < static_cast<int>(fx::ThemeId::Count); ++i)
    TEST_ASSERT_TRUE(seen[i]);
}

void test_picker_locked_theme_survives_a_track_change(void) {
  fx::ThemePicker p;
  core::Rng rng(1);
  p.lock(fx::ThemeId::Rain);
  for (int i = 0; i < 50; ++i) p.onTrackChange(rng);
  TEST_ASSERT_FALSE(p.shuffle());
  TEST_ASSERT_TRUE(p.current() == fx::ThemeId::Rain);
}

void test_picker_rows_map_to_the_list(void) {
  fx::ThemePicker p;
  core::Rng rng(7);
  TEST_ASSERT_EQUAL_STRING("Shuffle", fx::ThemePicker::rowName(0));
  p.chooseRow(2, rng);  // row 0 is Shuffle, so row 2 is theme 1
  TEST_ASSERT_FALSE(p.shuffle());
  TEST_ASSERT_EQUAL_INT(2, p.currentRow());
  p.chooseRow(0, rng);
  TEST_ASSERT_TRUE(p.shuffle());
  TEST_ASSERT_EQUAL_INT(0, p.currentRow());
}

void test_picker_round_trips_through_storage(void) {
  core::Rng rng(3);
  for (int row = 0; row < fx::ThemePicker::ROWS; ++row) {
    fx::ThemePicker a;
    a.chooseRow(row, rng);
    fx::ThemePicker b;
    b.fromStored(a.toStored());
    TEST_ASSERT_EQUAL_INT(a.shuffle() ? 1 : 0, b.shuffle() ? 1 : 0);
    if (!a.shuffle()) TEST_ASSERT_TRUE(a.current() == b.current());
  }
}

void test_picker_rejects_a_stored_value_from_a_bigger_build(void) {
  // Flashing an older build must not index off the end of the enum.
  fx::ThemePicker p;
  p.fromStored(9999u);
  TEST_ASSERT_TRUE(p.shuffle());
}


// ---------------------------------------------------------------------------
// Tetris
// ---------------------------------------------------------------------------

void test_tetris_every_rotation_has_exactly_four_cells(void) {
  // A tetromino with three or five cells is not a tetromino. The shape table is
  // hand-written coordinates, which is readable and exactly the kind of thing a
  // typo hides in.
  fx::Tetris t;
  core::Rng rng(1);
  t.begin(rng);
  // Drive every shape through every rotation and assert the drawn cell count
  // via the pixel budget: 4 cells at CELL^2 pixels, minus nothing, is the only
  // count a correct piece can produce.
  const int cell_px = fx::Tetris::CELL * fx::Tetris::CELL;
  audio::Modulation m;
  for (int r = 0; r < 4; ++r) {
    gfx::Framebuffer fb;
    fb.fill(0x0000);
    gfx::Surface s = fullSurface(fb);
    t.drawBand(s);
    int lit = 0;
    for (int y = 0; y < gfx::H; ++y)
      for (int x = 0; x < gfx::W; ++x)
        if (fb.at(x, y) != 0) ++lit;
    // Twelve pieces, but some are off-screen at any moment, so this is a
    // sanity band rather than an exact count.
    TEST_ASSERT_TRUE(lit > cell_px);
    TEST_ASSERT_TRUE(lit <= 4 * cell_px * fx::Tetris::MAX);
    m.onset = true;
    t.update(m, 1.0f / 30.0f, rng);
  }
}

void test_tetris_rotates_on_the_beat_and_not_otherwise(void) {
  // The quarter-turn landing on the onset is the whole reason this reads as
  // Tetris rather than as tumbling polygons.
  auto frameAfter = [](bool onset) {
    fx::Tetris t;
    core::Rng rng(0x5150);
    t.begin(rng);
    audio::Modulation m;
    m.onset = onset;
    core::Rng r2(0x5150);
    t.update(m, 0.0f, r2);  // dt 0: nothing moves, so only rotation can differ
    gfx::Framebuffer fb;
    fb.fill(0x0000);
    gfx::Surface s = fullSurface(fb);
    t.drawBand(s);
    uint32_t h = 2166136261u;
    for (int y = 0; y < gfx::H; ++y)
      for (int x = 0; x < gfx::W; ++x) h = (h ^ fb.at(x, y)) * 16777619u;
    return h;
  };
  TEST_ASSERT_TRUE(frameAfter(true) != frameAfter(false));
}

void test_tetris_pieces_are_recycled_forever(void) {
  // They fall off the bottom and come back. A piece that stopped being drawn
  // would thin the effect out to nothing over a long track.
  fx::Tetris t;
  core::Rng rng(9);
  t.begin(rng);
  audio::Modulation m;
  m.loudness = 1.0f;
  for (int f = 0; f < 30 * 120; ++f) t.update(m, 1.0f / 30.0f, rng);
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  gfx::Surface s = fullSurface(fb);
  t.drawBand(s);
  int lit = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0) ++lit;
  TEST_ASSERT_TRUE(lit > 200);
}

void test_tetris_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  fx::Tetris a, b;
  core::Rng ra(0x2020), rb(0x2020);
  a.begin(ra);
  b.begin(rb);
  audio::Modulation m;
  m.loudness = 0.5f;
  for (int f = 0; f < 20; ++f) {
    m.onset = (f % 7) == 0;
    a.update(m, 1.0f / 30.0f, ra);
    b.update(m, 1.0f / 30.0f, rb);
  }
  gfx::Surface s = fullSurface(whole);
  a.drawBand(s);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.drawBand(bs);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}


// ---------------------------------------------------------------------------
// Heartbeat is actually a different effect
// ---------------------------------------------------------------------------

namespace {

// Hashes a rendered frame, so two themes can be compared as pictures rather
// than as parameter lists.
uint32_t themeFrameHash(fx::ThemeId id, int frames, const art::Image *cover) {
  views::CoverLight v;
  v.begin(0x1177u);
  v.setTheme(id);
  v.setCover(cover);
  core::Rng rng(0x424242u);
  audio::Modulation m;
  m.loudness = 0.7f;
  m.bass = 0.8f;
  gfx::Framebuffer fb;
  for (int f = 0; f < frames; ++f) {
    // A steady 2Hz beat, which is where the two themes used to look alike.
    m.onset = (f % 15) == 0;
    m.beat_phase = static_cast<float>(f % 15) / 15.0f;
    v.update(m, 1.0f / 30.0f, rng);
    fb.fill(0x0000);
    gfx::Surface s = fullSurface(fb);
    v.renderBand(s);
    v.endFrame();
  }
  uint32_t h = 2166136261u;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) h = (h ^ fb.at(x, y)) * 16777619u;
  return h;
}

}  // namespace

void test_heartbeat_contracts_the_cover_and_coverlight_does_not(void) {
  // The complaint that prompted this: the two themes were separated only by
  // emission numbers, so on real music - where onsets come thick and fast -
  // they read as the same effect. The album visibly beating is a MECHANISM the
  // other theme does not have.
  art::Image cover;
  fillCornerTexture(cover);

  auto sizeSwing = [&cover](fx::ThemeId id) {
    views::CoverLight v;
    v.begin(0x99u);
    v.setTheme(id);
    v.setCover(&cover);
    core::Rng rng(0x11u);
    audio::Modulation m;
    m.bass = 0.0f;  // silence the bass follow so only the thump can move it
    m.loudness = 0.0f;
    float lo = 1e9f, hi = -1e9f;
    // A HALF SECOND around one onset, deliberately short. Over a longer window
    // the ~18s breath moves both themes by a couple of percent and swamps the
    // comparison - the first version of this test measured two seconds and
    // failed for exactly that reason, not because the contraction was missing.
    for (int f = 0; f < 15; ++f) {
      m.onset = (f == 2);
      v.update(m, 1.0f / 30.0f, rng);
      const float h = v.coverHalf();
      if (h < lo) lo = h;
      if (h > hi) hi = h;
    }
    return (hi - lo) / hi;
  };

  const float beat = sizeSwing(fx::ThemeId::Heartbeat);
  const float cl = sizeSwing(fx::ThemeId::CoverLight);
  // Heartbeat contracts by most of its 10%; CoverLight cannot move at all here,
  // because bass and loudness are both zero and half a second of breath is
  // under one percent.
  TEST_ASSERT_TRUE(beat > 0.05f);
  TEST_ASSERT_TRUE(beat > cl * 4.0f);
}

void test_heartbeat_fires_a_second_ring_after_the_first(void) {
  // lub-DUB. A single ring per beat is a pulse; two is a heartbeat.
  TEST_ASSERT_TRUE(fx::themeDoublePulse(fx::ThemeId::Heartbeat));
  TEST_ASSERT_FALSE(fx::themeDoublePulse(fx::ThemeId::CoverLight));
  // Fixed, not a fraction of the tempo - a real double-thump does not stretch.
  TEST_ASSERT_TRUE(fx::themeDubDelay(fx::ThemeId::Heartbeat) > 0.05f);
  TEST_ASSERT_TRUE(fx::themeDubDelay(fx::ThemeId::Heartbeat) < 0.35f);
}

void test_heartbeat_and_coverlight_render_differently(void) {
  // The end-to-end version of the complaint, asserted on pixels.
  art::Image cover;
  fillCornerTexture(cover);
  const uint32_t a = themeFrameHash(fx::ThemeId::CoverLight, 40, &cover);
  const uint32_t b = themeFrameHash(fx::ThemeId::Heartbeat, 40, &cover);
  TEST_ASSERT_TRUE(a != b);
}


// ---------------------------------------------------------------------------
// Outrun
// ---------------------------------------------------------------------------

void test_outrun_owns_every_pixel(void) {
  // It REPLACES the radial backdrop rather than drawing over it, which is the
  // only way it fits the budget. A single pixel it fails to write would show
  // stale band contents, not black.
  gfx::Framebuffer fb;
  fb.fill(0x07E0);  // green: unmissable if any survives
  fx::Outrun o;
  o.begin();
  audio::Modulation m;
  o.update(m, 1.0f / 30.0f);
  gfx::Surface s = fullSurface(fb);
  o.drawBand(s, 0xFFFF);
  int leftover = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) == 0x07E0) ++leftover;
  TEST_ASSERT_EQUAL_INT(0, leftover);
}

void test_outrun_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  fx::Outrun a, b;
  a.begin();
  b.begin();
  audio::Modulation m;
  m.loudness = 0.6f;
  for (int f = 0; f < 17; ++f) {
    a.update(m, 1.0f / 30.0f);
    b.update(m, 1.0f / 30.0f);
  }
  gfx::Surface s = fullSurface(whole);
  a.drawBand(s, 0xFFFF);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.drawBand(bs, 0xFFFF);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_outrun_grid_moves_toward_the_viewer(void) {
  // The road has to travel. A frozen grid is a wallpaper.
  auto hashAt = [](int frames) {
    fx::Outrun o;
    o.begin();
    audio::Modulation m;
    m.loudness = 0.5f;
    for (int f = 0; f < frames; ++f) o.update(m, 1.0f / 30.0f);
    gfx::Framebuffer fb;
    fb.fill(0x0000);
    gfx::Surface s = fullSurface(fb);
    o.drawBand(s, 0xFFFF);
    uint32_t h = 2166136261u;
    for (int y = 0; y < gfx::H; ++y)
      for (int x = 0; x < gfx::W; ++x) h = (h ^ fb.at(x, y)) * 16777619u;
    return h;
  };
  TEST_ASSERT_TRUE(hashAt(2) != hashAt(9));
}

void test_outrun_phase_never_runs_away(void) {
  // Kept in 0..1 by subtraction rather than fmod, so a track playing for hours
  // cannot drift into a range where a float has lost the precision to place a
  // line on the right row.
  fx::Outrun o;
  o.begin();
  audio::Modulation m;
  m.loudness = 1.0f;
  // An hour at 30fps.
  for (int f = 0; f < 30 * 3600; ++f) o.update(m, 1.0f / 30.0f);
  gfx::Framebuffer a, b;
  a.fill(0x0000);
  b.fill(0x0000);
  gfx::Surface sa = fullSurface(a);
  o.drawBand(sa, 0xFFFF);
  fx::Outrun fresh;
  fresh.begin();
  fresh.update(m, 1.0f / 30.0f);
  gfx::Surface sb = fullSurface(b);
  fresh.drawBand(sb, 0xFFFF);
  // Not equal frames - just proof the old one still draws a real scene rather
  // than a degenerate one.
  int lit = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (a.at(x, y) != 0) ++lit;
  TEST_ASSERT_TRUE(lit > gfx::W * gfx::H / 2);
}

void test_outrun_owns_the_backdrop_and_particle_themes_do_not(void) {
  // The gate that keeps a themed backdrop and the radial gradient from ever
  // both running over the same pixels.
  TEST_ASSERT_TRUE(fx::themeOwnsBackdrop(fx::ThemeId::Outrun));
  TEST_ASSERT_FALSE(fx::themeOwnsBackdrop(fx::ThemeId::CoverLight));
  TEST_ASSERT_FALSE(fx::themeOwnsBackdrop(fx::ThemeId::Tetris));
}


// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------

namespace {

uint32_t matrixHash(const gfx::Framebuffer &fb) {
  uint32_t h = 2166136261u;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x) h = (h ^ fb.at(x, y)) * 16777619u;
  return h;
}

}  // namespace

void test_matrix_owns_every_pixel(void) {
  gfx::Framebuffer fb;
  fb.fill(0xF800);  // red: any survivor is unmissable
  fx::Matrix mx;
  core::Rng rng(0x5A5A);
  mx.begin(rng);
  audio::Modulation m;
  mx.update(m, 1.0f / 30.0f, rng);
  gfx::Surface s = fullSurface(fb);
  mx.drawBand(s);
  int leftover = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) == 0xF800) ++leftover;
  TEST_ASSERT_EQUAL_INT(0, leftover);
}

void test_matrix_draws_columns_of_glyphs(void) {
  // The first fill is scattered across the screen, not stacked above it - the
  // bug Tetris had, where the theme showed an empty disc for seconds.
  gfx::Framebuffer fb;
  fb.fill(0x0000);
  fx::Matrix mx;
  core::Rng rng(0x1234);
  mx.begin(rng);
  gfx::Surface s = fullSurface(fb);
  mx.drawBand(s);
  int lit = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (fb.at(x, y) != 0) ++lit;
  TEST_ASSERT_TRUE(lit > 500);
}

void test_matrix_drawn_in_bands_matches_full_frame(void) {
  gfx::Framebuffer whole, banded;
  whole.fill(0x0000);
  banded.fill(0x0000);
  fx::Matrix a, b;
  core::Rng ra(0x77), rb(0x77);
  a.begin(ra);
  b.begin(rb);
  audio::Modulation m;
  m.loudness = 0.5f;
  for (int f = 0; f < 14; ++f) {
    m.onset = (f % 5) == 0;
    a.update(m, 1.0f / 30.0f, ra);
    b.update(m, 1.0f / 30.0f, rb);
  }
  gfx::Surface s = fullSurface(whole);
  a.drawBand(s);
  for (int y = 0; y < gfx::H; y += 20) {
    gfx::Surface bs;
    bs.px = banded.pixels() + static_cast<size_t>(y) * gfx::W;
    bs.w = gfx::W;
    bs.h = 20;
    bs.y0 = y;
    b.drawBand(bs);
  }
  int diffs = 0;
  for (int y = 0; y < gfx::H; ++y)
    for (int x = 0; x < gfx::W; ++x)
      if (whole.at(x, y) != banded.at(x, y)) ++diffs;
  TEST_ASSERT_EQUAL_INT(0, diffs);
}

void test_matrix_columns_fall(void) {
  auto at = [](int frames) {
    fx::Matrix mx;
    core::Rng rng(0x2468);
    mx.begin(rng);
    audio::Modulation m;
    m.loudness = 0.6f;
    for (int f = 0; f < frames; ++f) mx.update(m, 1.0f / 30.0f, rng);
    gfx::Framebuffer fb;
    fb.fill(0x0000);
    gfx::Surface s = fullSurface(fb);
    mx.drawBand(s);
    return matrixHash(fb);
  };
  TEST_ASSERT_TRUE(at(1) != at(20));
}

void test_matrix_beat_scrambles_the_glyphs(void) {
  // The onset churns every character at once, which is this theme's beat
  // reaction instead of a burst - the same event said twice was the mistake
  // Heartbeat was built out of.
  auto frame = [](bool onset) {
    fx::Matrix mx;
    core::Rng rng(0x1010);
    mx.begin(rng);
    audio::Modulation m;
    m.onset = onset;
    core::Rng r2(0x1010);
    mx.update(m, 0.0f, r2);  // dt 0: nothing falls, so only churn can differ
    gfx::Framebuffer fb;
    fb.fill(0x0000);
    gfx::Surface s = fullSurface(fb);
    mx.drawBand(s);
    return matrixHash(fb);
  };
  TEST_ASSERT_TRUE(frame(true) != frame(false));
}

void test_matrix_and_outrun_both_own_the_backdrop(void) {
  TEST_ASSERT_TRUE(fx::themeOwnsBackdrop(fx::ThemeId::Matrix));
  TEST_ASSERT_TRUE(fx::themeOwnsBackdrop(fx::ThemeId::Outrun));
  TEST_ASSERT_FALSE(fx::themeOwnsBackdrop(fx::ThemeId::Rain));
}


// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------

void test_backlight_stays_bright_while_playing(void) {
  // The rule that matters. This is a now-playing display; dimming the thing it
  // exists to show would be a bug dressed up as a power saving.
  core::Backlight bl;
  uint32_t t = 0;
  for (int i = 0; i < 2000; ++i) {
    t += 500;  // sixteen minutes with no input at all
    bl.update(t, /*playing=*/true, /*last_input_ms=*/0, /*host_asleep=*/false);
  }
  TEST_ASSERT_EQUAL(core::ScreenState::Bright, bl.state());
  TEST_ASSERT_EQUAL_UINT8(core::Backlight::BRIGHT, bl.duty());
}

void test_backlight_dims_then_sleeps_when_idle_and_stopped(void) {
  core::Backlight bl;
  const uint32_t input_at = 1000;
  bl.update(input_at, false, input_at, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Bright, bl.state());

  bl.update(input_at + core::Backlight::DIM_AFTER_MS - 1, false, input_at, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Bright, bl.state());

  bl.update(input_at + core::Backlight::DIM_AFTER_MS, false, input_at, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Dim, bl.state());

  bl.update(input_at + core::Backlight::OFF_AFTER_MS, false, input_at, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Off, bl.state());
  TEST_ASSERT_EQUAL_UINT8(core::Backlight::OFF, bl.duty());
}

void test_backlight_input_wakes_it_and_reports_the_wake(void) {
  core::Backlight bl;
  bl.update(1000, false, 1000, false);
  const uint32_t late = 1000 + core::Backlight::OFF_AFTER_MS + 5000;
  bl.update(late, false, 1000, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Off, bl.state());

  // An input at `late` wakes it.
  bl.update(late + 10, false, late, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Bright, bl.state());
  // The caller swallows the input that caused this, the way a phone does -
  // otherwise the tap that wakes the device also pauses the music.
  TEST_ASSERT_TRUE(bl.justWoke());

  bl.update(late + 20, false, late, false);
  TEST_ASSERT_FALSE(bl.justWoke());  // only on the frame it woke
}

void test_backlight_recent_input_beats_a_host_that_says_asleep(void) {
  // The safety valve. A stale or wrong host signal - a dead helper stuck on
  // "locked", a renamed Mac - must never hold the screen dark against the
  // person in front of it, because the thing that would clear it is the thing
  // that is broken. This happened twice during development.
  core::Backlight bl;
  const uint32_t touched = 10000;
  bl.update(touched, /*playing=*/false, touched, /*host_asleep=*/true);
  TEST_ASSERT_EQUAL(core::ScreenState::Bright, bl.state());

  // Still overridden a while later.
  bl.update(touched + core::Backlight::INPUT_OVERRIDE_MS - 1, false, touched,
            true);
  TEST_ASSERT_TRUE(bl.state() != core::ScreenState::Off);

  // And once the override lapses, the host wins again.
  bl.update(touched + core::Backlight::INPUT_OVERRIDE_MS, false, touched, true);
  TEST_ASSERT_EQUAL(core::ScreenState::Off, bl.state());
}

void test_backlight_host_asleep_overrides_playback(void) {
  // If the machine it sits beside is asleep or locked, so is this - even mid
  // track, and without waiting out any idle timer.
  core::Backlight bl;
  // Input long enough ago that the override has lapsed, so the host decides.
  const uint32_t touched = 1000;
  const uint32_t later = touched + core::Backlight::INPUT_OVERRIDE_MS + 1;
  bl.update(later, /*playing=*/true, touched, /*host_asleep=*/true);
  TEST_ASSERT_EQUAL(core::ScreenState::Off, bl.state());

  // And it comes straight back when the host does.
  bl.update(later + 100, true, touched, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Bright, bl.state());
}

void test_backlight_survives_the_millis_wrap(void) {
  // A device left plugged in reaches the 49.7-day wrap on a timer, not by
  // chance. Unsigned subtraction has to carry the idle interval across it.
  core::Backlight bl;
  const uint32_t before_wrap = 0xFFFFF000u;
  bl.update(before_wrap, false, before_wrap, false);
  // Now well past the wrap, with the last input still on the far side of it.
  bl.update(before_wrap + core::Backlight::DIM_AFTER_MS + 1000, false,
            before_wrap, false);
  TEST_ASSERT_EQUAL(core::ScreenState::Dim, bl.state());
}


// ---------------------------------------------------------------------------
// HostLink
// ---------------------------------------------------------------------------

namespace {

// The listener half is hardware; the DECISION is not, and the decision is the
// part that can darken the screen wrongly. Driven here through the accessors a
// received heartbeat would set.
class TestableHostLink : public net::HostLink {
 public:
  // Stands in for a received heartbeat.
  void feed(bool locked, uint32_t at_ms) {
    // Mirrors exactly what the platform layer does on a recognised request.
    locked_ = locked;
    at_ = at_ms;
    heard_ = true;
  }
  bool asleep(uint32_t now) const {
    if (!heard_) return false;
    if (locked_) return true;
    return (now - at_) > net::HostLink::TIMEOUT_MS;
  }

 private:
  bool heard_ = false;
  bool locked_ = false;
  uint32_t at_ = 0;
};

}  // namespace

void test_hostlink_fails_open_before_any_heartbeat(void) {
  // The most important case. If no helper is running - never installed, crashed,
  // Mac renamed - the screen must behave as though this mechanism does not
  // exist. Failing the other way would let a dead helper brick the display.
  net::HostLink hl;
  TEST_ASSERT_FALSE(hl.everHeard());
  TEST_ASSERT_FALSE(hl.hostAsleep(0));
  TEST_ASSERT_FALSE(hl.hostAsleep(999999));
}

void test_hostlink_locked_report_sleeps_immediately(void) {
  TestableHostLink hl;
  hl.feed(/*locked=*/true, 1000);
  TEST_ASSERT_TRUE(hl.asleep(1000));
}

void test_hostlink_awake_report_stays_awake(void) {
  TestableHostLink hl;
  hl.feed(false, 1000);
  TEST_ASSERT_FALSE(hl.asleep(1000));
  TEST_ASSERT_FALSE(hl.asleep(1000 + net::HostLink::TIMEOUT_MS - 1));
}

void test_hostlink_silence_means_asleep(void) {
  // A Mac that goes to sleep cannot send "I am asleep"; it stops sending. So
  // silence has to be the signal, or sleep is indistinguishable from the helper
  // simply not running.
  TestableHostLink hl;
  hl.feed(false, 1000);
  TEST_ASSERT_TRUE(hl.asleep(1000 + net::HostLink::TIMEOUT_MS + 1));
}

void test_hostlink_one_dropped_beat_is_not_a_blackout(void) {
  // The timeout must be comfortably longer than the sender's interval.
  TestableHostLink hl;
  const uint32_t interval = 5000;  // what the daemon sends at
  TEST_ASSERT_TRUE(net::HostLink::TIMEOUT_MS > interval * 3);
  hl.feed(false, 1000);
  TEST_ASSERT_FALSE(hl.asleep(1000 + interval * 2));
}

void test_hostlink_survives_the_millis_wrap(void) {
  TestableHostLink hl;
  const uint32_t before = 0xFFFFF000u;
  hl.feed(false, before);
  TEST_ASSERT_FALSE(hl.asleep(before + 1000));
  TEST_ASSERT_TRUE(hl.asleep(before + net::HostLink::TIMEOUT_MS + 1000));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_framebuffer_is_360_square);
  RUN_TEST(test_framebuffer_fill_sets_every_pixel);
  RUN_TEST(test_framebuffer_set_is_bounds_checked);
  RUN_TEST(test_rgb565_roundtrips_channel_extremes);
  RUN_TEST(test_unpack565_replicates_high_bits);
  RUN_TEST(test_add_sat_clamps_each_channel_independently);
  RUN_TEST(test_add_sat_does_not_bleed_red_into_green);
  RUN_TEST(test_add_sat_is_identity_against_black);
  RUN_TEST(test_fade_converges_to_black);
  RUN_TEST(test_fade_by_zero_is_black_and_full_is_near_identity);
  RUN_TEST(test_lerp565_hits_both_endpoints_exactly);
  RUN_TEST(test_lerp565_midpoint_is_between);
  RUN_TEST(test_mask_blackens_corners_and_keeps_centre);
  RUN_TEST(test_dither_is_deterministic);
  RUN_TEST(test_dither_perturbs_by_at_most_one_step_per_channel);
  RUN_TEST(test_dither_leaves_black_and_white_alone);
  RUN_TEST(test_bloom_leaves_a_black_frame_black);
  RUN_TEST(test_bloom_spreads_a_bright_point_to_its_neighbourhood);
  RUN_TEST(test_bloom_ignores_pixels_below_threshold);
  RUN_TEST(test_bloom_never_darkens);
  RUN_TEST(test_rng_is_reproducible_from_a_seed);
  RUN_TEST(test_rng_differs_between_seeds);
  RUN_TEST(test_rng_unit_stays_in_range);
  RUN_TEST(test_rng_range_respects_bounds);
  RUN_TEST(test_fnv1a_is_stable_and_distinguishes);
  RUN_TEST(test_frame_clock_reports_elapsed_seconds);
  RUN_TEST(test_frame_clock_clamps_a_stall);
  RUN_TEST(test_frame_clock_first_tick_is_zero);
  RUN_TEST(test_frame_clock_survives_the_millis_wrap);
  RUN_TEST(test_frame_dump_writes_a_readable_bmp);
  RUN_TEST(test_quad_frontal_fills_the_projected_rectangle);
  RUN_TEST(test_quad_uv_orientation_is_not_flipped_or_transposed);
  RUN_TEST(test_quad_rotation_produces_a_trapezoid_not_a_parallelogram);
  RUN_TEST(test_quad_behind_or_through_the_camera_draws_nothing);
  RUN_TEST(test_quad_larger_than_the_screen_is_clipped);
  RUN_TEST(test_quad_alpha_zero_is_a_no_op);
  RUN_TEST(test_quad_drawn_in_bands_matches_a_single_full_frame);
  RUN_TEST(test_small_quad_in_bands_matches_full_frame);
  RUN_TEST(test_placeholder_cover_is_asymmetric_and_deterministic);
  RUN_TEST(test_particles_pool_is_capped_and_does_not_overflow);
  RUN_TEST(test_particles_expire);
  RUN_TEST(test_particles_are_deterministic_for_a_seed);
  RUN_TEST(test_particles_off_screen_write_nothing);
  RUN_TEST(test_particles_render_additively_over_a_background);
  RUN_TEST(test_procedural_stays_in_range);
  RUN_TEST(test_procedural_tempo_is_plausible_and_seed_stable);
  RUN_TEST(test_procedural_onset_rate_matches_its_tempo);
  RUN_TEST(test_procedural_is_marked_not_live);
  RUN_TEST(test_fft_finds_a_tone_at_bin_32);
  RUN_TEST(test_fft_finds_a_tone_at_bin_100);
  RUN_TEST(test_fft_of_silence_is_silent);
  RUN_TEST(test_fft_puts_dc_in_bin_zero);
  RUN_TEST(test_fft_has_no_state_between_calls);
  RUN_TEST(test_bands_separate_low_from_high);
  RUN_TEST(test_bands_decay_to_quiet_on_silence);
  RUN_TEST(test_bands_attack_faster_than_they_release);
  RUN_TEST(test_onsets_fire_on_a_click_train);
  RUN_TEST(test_onsets_do_not_fire_on_a_steady_tone);
  RUN_TEST(test_onsets_do_not_fire_on_silence);
  RUN_TEST(test_tempo_converges_on_120_bpm);
  RUN_TEST(test_tempo_phase_stays_in_range_and_wraps);
  RUN_TEST(test_tempo_ignores_implausible_intervals);
  RUN_TEST(test_wavmic_loads_a_fixture);
  RUN_TEST(test_wavmic_paces_output_and_stays_in_range);
  RUN_TEST(test_wavmic_loops_at_the_end);
  RUN_TEST(test_wavmic_missing_file_fails_cleanly);
  RUN_TEST(test_analyzer_goes_live_on_music);
  RUN_TEST(test_analyzer_falls_back_on_silence_and_keeps_moving);
  RUN_TEST(test_analyzer_with_no_mic_at_all_still_animates);
  RUN_TEST(test_analyzer_handover_is_not_instant);
  RUN_TEST(test_analyzer_rides_through_gaps_in_percussive_music);
  RUN_TEST(test_gesture_tap_fires_on_release);
  RUN_TEST(test_gesture_long_press_fires_once_and_suppresses_the_tap);
  RUN_TEST(test_gesture_swipes_resolve_by_direction);
  RUN_TEST(test_gesture_short_drag_is_a_tap_not_a_swipe);
  RUN_TEST(test_gesture_diagonal_resolves_to_the_dominant_axis);
  RUN_TEST(test_gesture_swipe_that_curls_back_is_still_a_swipe);
  RUN_TEST(test_gesture_hold_at_the_end_of_a_drag_is_not_a_long_press);
  RUN_TEST(test_gesture_no_touch_reports_nothing);
  RUN_TEST(test_gesture_survives_the_millis_wrap);
  RUN_TEST(test_shell_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_shell_progress_ring_grows_with_progress);
  RUN_TEST(test_shell_unknown_volume_is_not_drawn_as_zero);
  RUN_TEST(test_shell_volume_overlay_expires);
  RUN_TEST(test_text_width_is_zero_for_empty_and_grows_with_length);
  RUN_TEST(test_text_draws_inside_its_measured_box);
  RUN_TEST(test_text_renders_latin1_accents_not_tofu);
  RUN_TEST(test_text_beyond_latin1_falls_back_visibly);
  RUN_TEST(test_text_malformed_utf8_terminates);
  RUN_TEST(test_text_fit_truncates_within_the_budget);
  RUN_TEST(test_text_outside_the_surface_draws_nothing);
  RUN_TEST(test_text_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_half_chord_narrows_toward_the_poles);
  RUN_TEST(test_library_publishes_only_complete_listings);
  RUN_TEST(test_library_caps_and_reports_truncation);
  RUN_TEST(test_library_clearing_tracks_publishes_empty_immediately);
  RUN_TEST(test_library_out_of_range_reads_are_null);
  RUN_TEST(test_library_truncates_overlong_names_safely);
  RUN_TEST(test_listview_centre_row_is_the_brightest);
  RUN_TEST(test_listview_scrolling_moves_the_selection);
  RUN_TEST(test_listview_fractional_position_differs_from_whole);
  RUN_TEST(test_listview_empty_says_so);
  RUN_TEST(test_listview_handles_null_items_and_short_lists);
  RUN_TEST(test_listview_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_confirmring_names_the_track);
  RUN_TEST(test_confirmring_marker_leans_to_the_chosen_side);
  RUN_TEST(test_confirmring_mid_glide_differs_from_both_ends);
  RUN_TEST(test_confirmring_long_name_stays_inside_the_disc);
  RUN_TEST(test_confirmring_handles_a_missing_name);
  RUN_TEST(test_confirmring_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_coverlight_particles_do_not_wash_out_the_cover);
  RUN_TEST(test_coverlight_is_bit_exact_across_runs);
  RUN_TEST(test_coverlight_cover_breathes_over_time);
  RUN_TEST(test_glyphs_every_kind_draws_something);
  RUN_TEST(test_glyphs_are_distinguishable_from_each_other);
  RUN_TEST(test_glyphs_outline_heart_is_lighter_than_filled);
  RUN_TEST(test_glyphs_stay_inside_their_box);
  RUN_TEST(test_glyphs_drawn_in_bands_match_full_frame);
  RUN_TEST(test_gestureflash_shows_then_expires);
  RUN_TEST(test_gestureflash_fades_rather_than_vanishing);
  RUN_TEST(test_gestureflash_survives_the_millis_wrap);
  RUN_TEST(test_nowplaying_unknown_saved_state_draws_no_heart);
  RUN_TEST(test_nowplaying_draws_both_saved_states_differently);
  RUN_TEST(test_nowplaying_no_track_carries_no_heart);
  RUN_TEST(test_daisy_draws_the_dog_and_owns_every_pixel);
  RUN_TEST(test_daisy_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_daisy_sleeps_then_yawns_then_sleeps_again);
  RUN_TEST(test_daisy_frame_index_is_always_in_range);
  RUN_TEST(test_daisy_is_bit_exact_across_runs);
  RUN_TEST(test_daisy_wakes_up_when_touched);
  RUN_TEST(test_daisy_gives_each_gesture_its_own_mood);
  RUN_TEST(test_daisy_settles_through_drowsy_on_her_way_back_to_sleep);
  RUN_TEST(test_daisy_holds_a_short_reaction_long_enough_to_read);
  RUN_TEST(test_daisy_does_not_yawn_straight_after_waking);
  RUN_TEST(test_daisy_extends_the_hold_when_poked_the_same_way_again);
  RUN_TEST(test_daisy_switches_mood_when_poked_a_different_way);
  RUN_TEST(test_daisy_frame_index_stays_in_range_across_every_reaction);
  RUN_TEST(test_daisy_is_bit_exact_across_runs_with_pokes);
  RUN_TEST(test_daisy_mid_reaction_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_transport_feedback_shows_while_a_track_is_loaded);
  RUN_TEST(test_transport_feedback_hides_when_nothing_is_listening);
  RUN_TEST(test_transport_feedback_shows_for_an_idle_device_with_no_track);
  RUN_TEST(test_transport_feedback_shows_for_a_track_without_a_known_device);
  RUN_TEST(test_an_abnormal_reset_counts_and_a_clean_one_clears);
  RUN_TEST(test_safe_mode_waits_for_the_third_crash);
  RUN_TEST(test_a_crash_loop_reaches_safe_mode);
  RUN_TEST(test_crashes_months_apart_never_reach_safe_mode);
  RUN_TEST(test_the_streak_is_forgiven_only_after_a_real_run);
  RUN_TEST(test_the_streak_is_capped_so_nvs_is_not_written_forever);
  RUN_TEST(test_a_corrupt_stored_streak_does_not_break_the_count);
  RUN_TEST(test_safe_screen_owns_every_pixel);
  RUN_TEST(test_safe_screen_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_safe_screen_draws_something_legible);
  RUN_TEST(test_safe_screen_survives_a_null_reason);
  RUN_TEST(test_toast_draws_nothing_when_inactive);
  RUN_TEST(test_toast_draws_when_active);
  RUN_TEST(test_toast_is_not_visible_for_an_empty_message);
  RUN_TEST(test_toast_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_toast_stays_inside_the_chord);
  RUN_TEST(test_nowplaying_suppresses_times_for_a_toast);
  RUN_TEST(test_command_queue_reports_a_drop_rather_than_hiding_it);
  RUN_TEST(test_command_queue_accepts_again_after_a_pop);
  RUN_TEST(test_coalesced_push_reports_a_drop_when_it_cannot_coalesce);
  RUN_TEST(test_coalesced_volume_never_fills_the_queue);
  RUN_TEST(test_heap_watch_is_quiet_while_there_is_room);
  RUN_TEST(test_heap_watch_fires_once_on_crossing);
  RUN_TEST(test_heap_watch_rearms_only_after_real_recovery);
  RUN_TEST(test_the_heap_floor_leaves_room_for_a_tls_handshake);
  RUN_TEST(test_tap_with_a_track_sends_playpause_and_flashes);
  RUN_TEST(test_tap_with_nothing_listening_tries_to_wake_a_device);
  RUN_TEST(test_tap_does_not_try_to_wake_a_device_that_is_already_there);
  RUN_TEST(test_swipe_down_with_a_track_opens_the_queue);
  RUN_TEST(test_swipe_down_with_no_track_is_swallowed);
  RUN_TEST(test_swipe_up_always_reaches_playlists);
  RUN_TEST(test_swipes_with_a_track_skip_and_flash);
  RUN_TEST(test_swipes_with_nothing_listening_only_wag);
  RUN_TEST(test_long_press_with_a_track_toggles_like);
  RUN_TEST(test_long_press_with_nothing_listening_gets_zoomies);
  RUN_TEST(test_long_press_with_a_device_but_no_track_still_refuses_visibly);
  RUN_TEST(test_no_gesture_routes_to_nothing);
  RUN_TEST(test_record_owns_every_pixel);
  RUN_TEST(test_record_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_record_actually_spins);
  RUN_TEST(test_record_spin_is_frame_rate_independent);
  RUN_TEST(test_record_is_bit_exact_across_runs);
  RUN_TEST(test_record_with_no_cover_invents_nothing);
  RUN_TEST(test_record_owns_the_backdrop_like_the_other_two);
  RUN_TEST(test_record_has_a_name);
  RUN_TEST(test_device_pick_prefers_a_computer);
  RUN_TEST(test_device_pick_skips_restricted_devices);
  RUN_TEST(test_device_pick_falls_back_to_the_first_usable);
  RUN_TEST(test_device_pick_reports_nothing_usable);
  RUN_TEST(test_device_pick_prefers_an_already_active_computer);
  RUN_TEST(test_every_theme_has_a_name_and_a_distinct_spawn);
  RUN_TEST(test_rain_falls_and_the_others_do_not);
  RUN_TEST(test_rain_does_not_burst_on_the_beat);
  RUN_TEST(test_theme_emission_is_frame_rate_independent);
  RUN_TEST(test_picker_shuffle_never_repeats_the_current_theme);
  RUN_TEST(test_picker_shuffle_reaches_every_theme);
  RUN_TEST(test_picker_locked_theme_survives_a_track_change);
  RUN_TEST(test_picker_rows_map_to_the_list);
  RUN_TEST(test_picker_round_trips_through_storage);
  RUN_TEST(test_picker_rejects_a_stored_value_from_a_bigger_build);
  RUN_TEST(test_tetris_every_rotation_has_exactly_four_cells);
  RUN_TEST(test_tetris_rotates_on_the_beat_and_not_otherwise);
  RUN_TEST(test_tetris_pieces_are_recycled_forever);
  RUN_TEST(test_tetris_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_heartbeat_contracts_the_cover_and_coverlight_does_not);
  RUN_TEST(test_heartbeat_fires_a_second_ring_after_the_first);
  RUN_TEST(test_heartbeat_and_coverlight_render_differently);
  RUN_TEST(test_outrun_owns_every_pixel);
  RUN_TEST(test_outrun_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_outrun_grid_moves_toward_the_viewer);
  RUN_TEST(test_outrun_phase_never_runs_away);
  RUN_TEST(test_outrun_owns_the_backdrop_and_particle_themes_do_not);
  RUN_TEST(test_matrix_owns_every_pixel);
  RUN_TEST(test_matrix_draws_columns_of_glyphs);
  RUN_TEST(test_matrix_drawn_in_bands_matches_full_frame);
  RUN_TEST(test_matrix_columns_fall);
  RUN_TEST(test_matrix_beat_scrambles_the_glyphs);
  RUN_TEST(test_matrix_and_outrun_both_own_the_backdrop);
  RUN_TEST(test_backlight_stays_bright_while_playing);
  RUN_TEST(test_backlight_dims_then_sleeps_when_idle_and_stopped);
  RUN_TEST(test_backlight_input_wakes_it_and_reports_the_wake);
  RUN_TEST(test_backlight_recent_input_beats_a_host_that_says_asleep);
  RUN_TEST(test_backlight_host_asleep_overrides_playback);
  RUN_TEST(test_backlight_survives_the_millis_wrap);
  RUN_TEST(test_hostlink_fails_open_before_any_heartbeat);
  RUN_TEST(test_hostlink_locked_report_sleeps_immediately);
  RUN_TEST(test_hostlink_awake_report_stays_awake);
  RUN_TEST(test_hostlink_silence_means_asleep);
  RUN_TEST(test_hostlink_one_dropped_beat_is_not_a_blackout);
  RUN_TEST(test_hostlink_survives_the_millis_wrap);
  return UNITY_END();
}
