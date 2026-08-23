// Host unit tests for the display-free engine.
//
// Everything here runs without SDL, without hardware, and in milliseconds.
// Each test is written against a symptom that can actually be seen on screen,
// not just against a function signature.
//
//   pio test -e test

#include <unity.h>

#include <cstdio>
#include <cstdlib>

#include "core/FrameClock.h"
#include "core/Hash.h"
#include "core/Rng.h"
#include "gfx/Blend.h"
#include "gfx/Bloom.h"
#include "gfx/CircleMask.h"
#include "gfx/Color.h"
#include "gfx/Dither.h"
#include "gfx/Framebuffer.h"
#include "art/Image.h"
#include "audio/Procedural.h"
#include "fx/Particles.h"
#include "gfx/Quad3D.h"
#include "gfx/Surface.h"
#include "platform/desktop/FrameDump.h"

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
  return UNITY_END();
}
