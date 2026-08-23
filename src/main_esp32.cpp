// Device entry point: Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Bring-up order is deliberate. Nothing touches the panel until the boot banner
// has confirmed PSRAM is actually present and contiguous, because every later
// stage assumes it and a wrong memory_type setting reports zero on perfectly
// good hardware.
//
// The render loop is a band renderer with no framebuffer anywhere. Measured on
// this board, PSRAM writes cap at 33 MB/s regardless of access width, so any
// full-frame pass over a PSRAM framebuffer costs at least 7.5 ms and the
// pipeline wanted six of them. Compositing 40-row bands in internal SRAM and
// DMA-ing each straight to the panel never pays that cost at all.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "art/Image.h"
#include "audio/Modulation.h"
#include "audio/Procedural.h"
#include "core/FrameClock.h"
#include "core/Hash.h"
#include "core/Rng.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"
#include "platform/esp32/Bench.h"
#include "platform/esp32/Boot.h"
#include "platform/esp32/Panel.h"
#include "platform/esp32/Pins.h"
#include "views/CoverLight.h"

namespace {

views::CoverLight g_view;
art::Image g_cover;
audio::Procedural g_proc;
audio::Modulation g_mod;
core::FrameClock g_clock;
core::Rng g_rng(0xC0FFEE);
bool g_ok = false;

// Rolling timing, so the serial line reports what the render actually costs
// rather than what it was estimated to cost.
uint32_t g_frames = 0;
uint64_t g_render_us = 0;
uint64_t g_total_us = 0;
uint64_t g_update_us = 0;
uint64_t g_wait_us = 0;    // blocked waiting for a band's DMA to finish
uint64_t g_commit_us = 0;  // byte swap + queue
uint64_t g_drain_us = 0;   // end-of-frame drain
uint64_t g_blur_us = 0;    // the 90x90 bloom blur

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment before the host is listening, and a banner nobody
  // sees is worse than none: it looks like a board that did not boot.
  delay(1500);
  esp32::printBootBanner();
  esp32::runMemoryBenchmark();

  Serial.println("stage 4: band renderer, Cover Light");
  g_ok = esp32::panelBegin();
  if (!g_ok) {
    Serial.println("panel: FAILED. Check Pins.h - touch/encoder pins are still");
    Serial.println("unconfirmed community data.");
    return;
  }

  g_proc.reseed(fnv1a("first-light"));
  g_view.begin(fnv1a("first-light"));

  // A synthetic cover until Spotify artwork exists. 192x192 is 73 KB and lands
  // in PSRAM; see art/ImageAlloc.cpp for why that is mandatory rather than
  // preferred.
  art::makePlaceholderCover(fnv1a("first-light"), 192, &g_cover);
  if (g_cover.valid()) {
    g_view.setCover(&g_cover);
    Serial.printf("cover: %dx%d in PSRAM\n", g_cover.width(), g_cover.height());
  } else {
    Serial.println("cover: allocation failed; rendering the no-artwork path");
  }
  esp32::panelBacklight(210);
  Serial.printf("procedural tempo: %.1f bpm\n", g_proc.bpm());
  Serial.printf("internal heap after setup: %lu, largest %lu\n",
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_largest_free_block(
                    MALLOC_CAP_INTERNAL));
}

void loop() {
  if (!g_ok) {
    delay(1000);
    return;
  }

  const uint64_t t_frame = esp_timer_get_time();
  const float dt = g_clock.tick(millis());

  g_proc.fill(&g_mod, dt);
  g_view.update(g_mod, dt, g_rng);

  const uint64_t t_render = esp_timer_get_time();
  g_update_us += t_render - t_frame;

  esp32::panelBeginFrame();
  for (int y = 0; y < gfx::H; y += esp32::PANEL_BAND_H) {
    const uint64_t w0 = esp_timer_get_time();
    gfx::Surface s;
    s.px = esp32::panelNextBand();
    const uint64_t w1 = esp_timer_get_time();
    g_wait_us += w1 - w0;

    s.w = gfx::W;
    s.h = esp32::PANEL_BAND_H;
    s.y0 = y;
    g_view.renderBand(s);

    const uint64_t c0 = esp_timer_get_time();
    esp32::panelCommitBand();
    g_commit_us += esp_timer_get_time() - c0;
  }
  const uint64_t d0 = esp_timer_get_time();
  esp32::panelEndFrame();
  g_drain_us += esp_timer_get_time() - d0;
  const uint64_t b0 = esp_timer_get_time();
  g_view.endFrame();
  g_blur_us += esp_timer_get_time() - b0;

  g_render_us += esp_timer_get_time() - t_render;
  g_total_us += esp_timer_get_time() - t_frame;
  ++g_frames;

  static uint32_t last = 0;
  if (millis() - last >= 3000) {
    last = millis();
    const float fps = g_frames * 1000000.0f / static_cast<float>(g_total_us);
    auto &t = g_view.timing();
    const float nb = t.frames ? static_cast<float>(t.frames) : 1.0f;
    const float f = 1000.0f / g_frames;
    Serial.printf(
        "fps %5.1f frame %6.2f ms | upd %5.2f back %5.2f cover %5.2f "
        "part %5.2f accum %5.2f wait %5.2f swap %5.2f drain %5.2f blur %5.2f | "
        "parts %4d heap %lu\n",
        fps, g_total_us * f / 1000000.0f, g_update_us * f / 1000000.0f,
        t.backdrop / 1000.0f / nb * 9.0f, t.cover / 1000.0f / nb * 9.0f,
        t.particles / 1000.0f / nb * 9.0f,
        t.bloom / 1000.0f / nb * 9.0f, g_wait_us * f / 1000000.0f,
        g_commit_us * f / 1000000.0f, g_drain_us * f / 1000000.0f,
        g_blur_us * f / 1000000.0f,
        g_view.particleCount(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    g_frames = 0;
    g_render_us = 0;
    g_total_us = 0;
    g_update_us = 0;
    g_wait_us = 0;
    g_commit_us = 0;
    g_drain_us = 0;
    g_blur_us = 0;
    t = views::CoverLight::Timing();
  }
}
