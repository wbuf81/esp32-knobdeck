// Device entry point: Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Bring-up order is deliberate. Nothing touches the panel until the boot banner
// has confirmed PSRAM is actually present and contiguous, because every later
// stage assumes it and a wrong memory_type setting reports zero on perfectly
// good hardware.

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "gfx/Geometry.h"
#include "platform/esp32/Bench.h"
#include "platform/esp32/Boot.h"
#include "platform/esp32/Panel.h"
#include "platform/esp32/Pins.h"
#include "platform/esp32/TestPattern.h"

namespace {
uint16_t *g_fb = nullptr;
bool g_panel_ok = false;
}  // namespace

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment before the host is listening, and a banner nobody
  // sees is worse than none: it looks like a board that did not boot.
  delay(1500);
  esp32::printBootBanner();
  esp32::runMemoryBenchmark();

  Serial.println("stage 3: panel bring-up");
  g_panel_ok = esp32::panelBegin();
  if (!g_panel_ok) {
    Serial.println("panel: FAILED to initialise. Check the pin map in Pins.h -");
    Serial.println("every pin there is unconfirmed community data.");
    return;
  }

  g_fb = static_cast<uint16_t *>(heap_caps_malloc(
      static_cast<size_t>(gfx::W) * gfx::H * 2, MALLOC_CAP_SPIRAM));
  if (!g_fb) {
    Serial.println("panel: test framebuffer allocation failed");
    return;
  }

  esp32::drawTestPattern(g_fb);
  esp32::panelPushFrame(g_fb);
  esp32::panelBacklight(200);
  Serial.printf("panel: test pattern pushed in %lu us\n",
                (unsigned long)esp32::panelLastPushUs());
  Serial.println();
  Serial.println("Look at the screen and report:");
  Serial.println("  1. quadrants clockwise from top-left: RED GREEN WHITE BLUE?");
  Serial.println("     (if red and blue are swapped, the BGR bit is wrong;");
  Serial.println("      if they are rotated, MADCTL is wrong)");
  Serial.println("  2. is the yellow wedge pointing UP?");
  Serial.println("  3. is the white ring complete, not clipped on any side?");
  Serial.println("  4. is the centre grey ramp smooth, 16 steps?");
}

void loop() {
  static uint32_t last = 0;
  static int frames = 0;

  if (g_panel_ok && g_fb) {
    // Re-push continuously to get a real sustained push rate, which is the
    // number that decides whether the band renderer's budget is real.
    esp32::panelPushFrame(g_fb);
    ++frames;
  }

  if (millis() - last >= 5000) {
    last = millis();
    Serial.printf("alive  uptime %lus  fps %.1f  push %lu us  int-heap %lu\n",
                  (unsigned long)(millis() / 1000), frames / 5.0f,
                  (unsigned long)esp32::panelLastPushUs(),
                  (unsigned long)ESP.getFreeHeap());
    frames = 0;
  }
}
