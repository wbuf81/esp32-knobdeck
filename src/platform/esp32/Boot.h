#pragma once

// The boot banner.
//
// The ancestor project's board notes say this "answered more questions than any
// other single thing", and that is not hyperbole: a device that quietly reboots
// every twenty seconds is indistinguishable from a slow one until something
// prints the reset reason and the crash streak.
//
// Two numbers here decide whether this project's whole render design is viable:
// PSRAM total, and PSRAM largest free block. If PSRAM reads zero, the build's
// memory_type is wrong - not the hardware.

#include <cstdint>

namespace esp32 {

void printBootBanner();

// Consecutive abnormal resets, persisted in NVS. Read after printBootBanner().
int crashStreak();

// Why the device last restarted, as a short human string ("PANIC (crash)",
// "brownout", ...). Valid after printBootBanner(); never null.
const char *resetReasonText();

// True when this boot should skip the network and the effects. See
// core/CrashPolicy.h for the rule; printBootBanner() decides it.
bool safeMode();

// Call once per loop with millis(). After a healthy stretch it zeroes the
// stored streak, which is what stops unrelated crashes months apart from
// accumulating into a safe mode neither of them earned. Cheap after the first
// successful call - it writes to NVS exactly once per boot.
void noteUptime(uint32_t now_ms);

// Hardware watchdog - for the UI task ONLY.
//
// This comment is the whole reason the feature is worth having, and it is
// inherited from a design that was written down and then never built:
//
//   The first version subscribed the net task too and put the device into a
//   reboot loop every 30 seconds. That was a design error, not a tuning one:
//   the net task legitimately blocks on network I/O, and a single request
//   could hold it for connect-timeout plus read-timeout. A watchdog is for
//   code that must never block, and the render loop is exactly that - it runs
//   at hundreds of frames a second and any stall is a genuine hang, like the
//   SPI bus deadlock that started all this.
//
// The net task gets a heartbeat instead; see NetWorker::stalled().
//
// Arduino already initialises the task watchdog (CONFIG_ESP_TASK_WDT=y,
// PANIC=y, 5 s) and simply never subscribes the loop task, so on this
// framework watchdogBegin() is a subscribe rather than an init. It falls back
// to initialising one if a future framework ships with it off.
void watchdogBegin();
void watchdogFeed();

// Call once per loop. Watches free INTERNAL heap and shouts once if it falls
// below the level where TLS handshakes start failing. See core/HeapPolicy.h;
// this is the heapTick() the deleted Diag.h declared and never had.
void heapTick(uint32_t now_ms);

}  // namespace esp32
