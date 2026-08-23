// Device entry point: Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Bring-up order is deliberate. Nothing touches the panel until the boot banner
// has confirmed PSRAM is actually present and contiguous, because every later
// stage assumes it and a wrong memory_type setting reports zero on perfectly
// good hardware.
//
// The renderer is a band renderer with no framebuffer anywhere. Measured on this
// board, PSRAM writes cap at 33 MB/s regardless of access width, so any
// full-frame pass over a PSRAM framebuffer costs at least 7.5 ms and the
// pipeline wanted six of them. Compositing 40-row bands in internal SRAM and
// DMA-ing each straight to the panel never pays that cost at all.
//
// Threading follows the ancestor project, which got it right: the network task
// owns core 0 and may block for seconds; rendering owns core 1 and must not.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstring>

#include "art/Image.h"
#include "audio/AudioAnalyzer.h"
#include "audio/Modulation.h"
#include "config/DeviceConfig.h"
#include "core/AppState.h"
#include "core/FrameClock.h"
#include "core/Hash.h"
#include "core/ProgressClock.h"
#include "core/Rng.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"
#include "net/NetWorker.h"
#include "platform/esp32/Bench.h"
#include "platform/esp32/Boot.h"
#include "platform/esp32/Panel.h"
#include "platform/esp32/Pins.h"
#include "views/CoverLight.h"

namespace {

views::CoverLight g_view;
art::Image g_cover;
audio::AudioAnalyzer g_analyzer;
audio::Modulation g_mod;
core::FrameClock g_clock;
core::Rng g_rng(0xC0FFEE);
ProgressClock g_progress;

// Held for the life of the program: NetWorker keeps const char* into these.
DeviceConfig g_cfg;
NetWorker *g_net = nullptr;

bool g_panel_ok = false;
char g_track[ID_LEN] = {};

uint32_t g_frames = 0;
uint64_t g_total_us = 0;

const char *linkName(LinkStatus s) {
  switch (s) {
    case LinkStatus::Booting: return "booting";
    case LinkStatus::Connecting: return "connecting";
    case LinkStatus::Online: return "online";
    case LinkStatus::Offline: return "offline";
    case LinkStatus::AuthError: return "auth-error";
    case LinkStatus::ReauthNeeded: return "reauth-needed";
  }
  return "?";
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment before the host is listening, and a banner nobody
  // sees is worse than none: it looks like a board that did not boot.
  delay(1500);
  esp32::printBootBanner();

  g_panel_ok = esp32::panelBegin();
  if (!g_panel_ok) {
    Serial.println("panel: FAILED. Check Pins.h.");
  } else {
    esp32::panelBacklight(210);
  }

  const uint32_t seed = fnv1a("first-light");
  g_view.begin(seed);
  g_analyzer.begin(nullptr);  // no microphone yet: I2S pins are unconfirmed
  g_analyzer.setTrack(seed);

  art::makePlaceholderCover(seed, 192, &g_cover);
  if (g_cover.valid()) g_view.setCover(&g_cover);

  // Credentials resolve NVS-first, compiled secrets as the per-field fallback -
  // so a developer board keeps working with no setup step while a gifted one is
  // configured entirely through the portal.
  g_cfg = DeviceConfig::load();
  Serial.printf("config: wifi=%s spotify=%s\n",
                g_cfg.wifi_ssid.empty() ? "MISSING" : "set",
                g_cfg.refresh_token.empty() ? "MISSING" : "set");

  if (g_cfg.complete()) {
    static NetWorker net(g_cfg.client_id.c_str(), g_cfg.client_secret.c_str(),
                         g_cfg.refresh_token.c_str());
    g_net = &net;
    // No SD card wired up yet, so artwork will report unavailable rather than
    // silently failing - which is the distinction the ancestor's notes insist on.
    g_net->start("/sd/art", g_cfg.wifi_ssid.c_str(), g_cfg.wifi_password.c_str());
    Serial.println("net: worker started on core 0");
  } else {
    Serial.println("net: config incomplete; running visuals only");
  }
}

void loop() {
  const uint64_t t_frame = esp_timer_get_time();
  const float dt = g_clock.tick(millis());
  const uint32_t now = millis();

  AppState st;
  if (g_net) st = g_net->snapshot();

  // Progress is extrapolated locally between polls and resynced ONLY when the
  // publish sequence changes. Copying the published position every frame is the
  // bug this class exists for: it overwrites the extrapolation, so the clock
  // moves in 2 s jumps instead of ticking.
  g_progress.sync(st.publish_seq, st.pb.progress_ms);
  g_progress.advance(static_cast<uint32_t>(dt * 1000.0f), st.pb.is_playing,
                     st.pb.duration_ms);
  const uint32_t shown_progress = g_progress.value();

  // A new track reseeds everything derived from it: hue, tempo, particle
  // palette. Deterministic on the id, so a song always looks the same way.
  if (std::strncmp(g_track, st.pb.track_id, ID_LEN) != 0) {
    std::strncpy(g_track, st.pb.track_id, ID_LEN - 1);
    const uint32_t seed = st.pb.track_id[0] ? fnv1a(st.pb.track_id)
                                            : fnv1a("first-light");
    g_view.begin(seed);
    g_analyzer.setTrack(seed);
    if (g_cover.valid()) g_view.setCover(&g_cover);
    Serial.printf("track: %s - %s\n",
                  st.pb.artist[0] ? st.pb.artist : "(none)",
                  st.pb.title[0] ? st.pb.title : "(none)");
  }

  g_analyzer.update(&g_mod, dt);
  g_mod.progress01 = st.pb.duration_ms
                         ? static_cast<float>(shown_progress) /
                               static_cast<float>(st.pb.duration_ms)
                         : 0.0f;
  g_mod.volume01 = st.pb.volume_pct >= 0 ? st.pb.volume_pct * 0.01f : 0.7f;

  g_view.update(g_mod, dt, g_rng);

  if (g_panel_ok) {
    esp32::panelBeginFrame();
    for (int y = 0; y < gfx::H; y += esp32::PANEL_BAND_H) {
      gfx::Surface s;
      s.px = esp32::panelNextBand();
      s.w = gfx::W;
      s.h = esp32::PANEL_BAND_H;
      s.y0 = y;
      g_view.renderBand(s);
      esp32::panelCommitBand();
    }
    esp32::panelEndFrame();
    g_view.endFrame();
  }

  g_total_us += esp_timer_get_time() - t_frame;
  ++g_frames;

  static uint32_t last = 0;
  if (now - last >= 3000) {
    last = now;
    const float fps = g_frames * 1000000.0f / static_cast<float>(g_total_us);
    Serial.printf(
        "fps %5.1f | link %-13s %s %s | %d%% vol | %lu/%lu ms | parts %4d "
        "| heap %lu psram %lu\n",
        fps, linkName(st.link), st.pb.is_playing ? "play" : "paus",
        st.pb.title[0] ? st.pb.title : "(nothing)", st.pb.volume_pct,
        (unsigned long)shown_progress, (unsigned long)st.pb.duration_ms,
        g_view.particleCount(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (g_net && g_net->stalled(now)) Serial.println("net: task appears STALLED");
    g_frames = 0;
    g_total_us = 0;
  }
}
