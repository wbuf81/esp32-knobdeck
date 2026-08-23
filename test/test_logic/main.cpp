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
  return UNITY_END();
}
