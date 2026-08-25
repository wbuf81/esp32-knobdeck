#include "Boot.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_task_wdt.h>

#include "core/CrashPolicy.h"

namespace esp32 {
namespace {

int g_streak = 0;
bool g_safe_mode = false;
bool g_forgiven = false;
bool g_wdt_on = false;
const char *g_reason = "unknown";

// Only reached if a future framework ships with the watchdog off; Arduino's own
// 5 s is used as-is otherwise. Generous next to a loop that runs at 126 fps -
// anything this catches is a hang, not a slow frame.
constexpr uint32_t WDT_TIMEOUT_S = 8;

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
  g_reason = resetReasonName(reason);

  // A reset caused by attaching to the serial port shows as power-on; a real
  // crash shows as PANIC. Read this line before concluding anything about a
  // fault, because attaching to observe one destroys the state you wanted.
  Preferences p;
  if (p.begin("boot", false)) {
    const int stored = p.getInt("streak", 0);
    g_streak = core::nextCrashStreak(stored, abnormal(reason));
    // Only write when the value actually moves. The old code wrote on every
    // boot including every clean one, which is a flash erase cycle per power
    // cycle for a number that was already zero.
    if (g_streak != stored) p.putInt("streak", g_streak);
    p.end();
  }
  g_safe_mode = core::safeModeWanted(g_streak);

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

  if (g_safe_mode) {
    Serial.println();
    Serial.printf("  *** SAFE MODE (%d consecutive abnormal resets) ***\n",
                  g_streak);
    Serial.println("  Network and effects are DISABLED for this boot, so the");
    Serial.println("  two most likely sources of the crash are not running and");
    Serial.println("  this serial port stays up. Fix, flash, and it clears");
    Serial.printf("  itself after %lu s of running.\n",
                  (unsigned long)(core::HEALTHY_AFTER_MS / 1000));
    Serial.println("=====================================================");
  }
  Serial.println();
}

bool safeMode() { return g_safe_mode; }

const char *resetReasonText() { return g_reason; }

void noteUptime(uint32_t now_ms) {
  if (g_forgiven) return;
  if (!core::streakForgiven(now_ms)) return;
  g_forgiven = true;  // set first: one attempt per boot either way

  // Nothing to forgive, and nothing worth an erase cycle to say so.
  if (g_streak == 0) return;

  Preferences p;
  if (p.begin("boot", false)) {
    p.putInt("streak", 0);
    p.end();
  }
  Serial.printf("boot: ran %lu s cleanly, crash streak %d -> 0\n",
                (unsigned long)(now_ms / 1000), g_streak);
  g_streak = 0;
}

void watchdogBegin() {
  // Subscribe the CALLING task, which must be the UI task - see the header.
  esp_err_t err = esp_task_wdt_add(nullptr);
  if (err == ESP_ERR_INVALID_STATE) {
    // No watchdog running at all. Not the case on this framework, but a
    // future one shipping with CONFIG_ESP_TASK_WDT=n would otherwise silently
    // leave the device unguarded, which is the failure mode this whole change
    // exists to remove.
    if (esp_task_wdt_init(WDT_TIMEOUT_S, true) == ESP_OK) {
      err = esp_task_wdt_add(nullptr);
    }
  }
  g_wdt_on = err == ESP_OK;
  Serial.printf("watchdog: %s\n",
                g_wdt_on ? "UI task subscribed" : "NOT ARMED");
}

void watchdogFeed() {
  if (g_wdt_on) esp_task_wdt_reset();
}

}  // namespace esp32
