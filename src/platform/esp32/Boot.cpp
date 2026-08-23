#include "Boot.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_flash.h>

namespace esp32 {
namespace {

int g_streak = 0;

const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC (crash)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP:return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
#ifdef ESP_RST_USB
    case ESP_RST_USB:      return "USB peripheral";
#endif
#ifdef ESP_RST_JTAG
    case ESP_RST_JTAG:     return "JTAG";
#endif
    default:               return "unknown";
  }
}

bool abnormal(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT ||
         r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}

}  // namespace

int crashStreak() { return g_streak; }

void printBootBanner() {
  const esp_reset_reason_t reason = esp_reset_reason();

  // A reset caused by attaching to the serial port shows as power-on; a real
  // crash shows as PANIC. Read this line before concluding anything about a
  // fault, because attaching to observe one destroys the state you wanted.
  Preferences p;
  if (p.begin("boot", false)) {
    g_streak = p.getInt("streak", 0);
    g_streak = abnormal(reason) ? g_streak + 1 : 0;
    p.putInt("streak", g_streak);
    p.end();
  }

  esp_chip_info_t chip;
  esp_chip_info(&chip);
  uint32_t flash_size = 0;
  esp_flash_get_size(nullptr, &flash_size);

  const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t psram_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t int_block =
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  Serial.println();
  Serial.println("=====================================================");
  Serial.printf("  %s  -  knob spotify player\n", APP_NAME);
  Serial.println("=====================================================");
  Serial.printf("  reset        : %s\n", resetReasonName(reason));
  Serial.printf("  crash streak : %d\n", g_streak);
  Serial.printf("  chip         : ESP32-S3 rev%d, %d core(s)\n",
                chip.revision, chip.cores);
  Serial.printf("  cpu freq     : %lu MHz\n",
                (unsigned long)getCpuFrequencyMhz());
  Serial.printf("  flash        : %lu bytes\n", (unsigned long)flash_size);
  Serial.printf("  PSRAM total  : %lu bytes\n", (unsigned long)psram_total);
  Serial.printf("  PSRAM free   : %lu bytes\n", (unsigned long)psram_free);
  Serial.printf("  PSRAM block  : %lu bytes\n", (unsigned long)psram_block);
  Serial.printf("  int heap free: %lu bytes\n", (unsigned long)int_free);
  Serial.printf("  int heap blk : %lu bytes\n", (unsigned long)int_block);

  // 360x360 RGB565 is 259200 bytes; the design wants two of them plus the
  // bloom scratch. Say so loudly rather than failing mysteriously later.
  if (psram_total == 0) {
    Serial.println();
    Serial.println("  *** PSRAM READS ZERO ***");
    Serial.println("  This is almost never the hardware. Check that");
    Serial.println("  board_build.arduino.memory_type = qio_opi and");
    Serial.println("  board_build.psram_type = opi - the R8 part is OCTAL");
    Serial.println("  PSRAM, and a quad setting reports zero on a good board.");
  } else if (psram_block < 600000) {
    Serial.printf("\n  WARNING: largest PSRAM block %lu < 600000; two\n",
                  (unsigned long)psram_block);
    Serial.println("  framebuffers plus scratch will not fit contiguously.");
  }
  Serial.println("=====================================================");
  Serial.println();
}

}  // namespace esp32
