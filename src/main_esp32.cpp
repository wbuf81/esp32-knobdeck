// Device entry point: Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Bring-up order is deliberate. Nothing here touches the panel, the touch
// controller or the encoder until the boot banner has confirmed that PSRAM is
// actually present and contiguous, because every later stage assumes it and a
// wrong memory_type setting reports zero on perfectly good hardware.

#include <Arduino.h>

#include "platform/esp32/Bench.h"
#include "platform/esp32/Boot.h"
#include "platform/esp32/Pins.h"

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment before the host is listening, and a banner nobody
  // sees is worse than no banner: it looks like a board that did not boot.
  delay(1500);
  esp32::printBootBanner();
  esp32::runMemoryBenchmark();
  Serial.println("stage 2: banner + benchmark. Panel bring-up is next.");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 5000) {
    last = millis();
    Serial.printf("alive  uptime %lus  int-heap %lu  psram %lu\n",
                  (unsigned long)(millis() / 1000),
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)ESP.getFreePsram());
  }
  delay(10);
}
