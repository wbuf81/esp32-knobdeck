#pragma once

// Boot banner and heap reporting.
//
// When the hardware arrives, the serial log is the only instrument available.
// Free heap in particular is the number that decides whether this design
// survives on a board with no PSRAM, and the failure mode without it — a silent
// reboot loop — is miserable to diagnose after the fact.

#include <cstdint>

void bootBanner(bool sd_ok);

// Hardware watchdog — for the UI task ONLY.
//
// The first version subscribed the net task too and put the device into a
// reboot loop every 30 seconds. That was a design error, not a tuning one: the
// net task legitimately blocks on network I/O, and a single request could hold
// it for connect-timeout plus read-timeout. A watchdog is for code that must
// never block, and the render loop is exactly that — it runs at hundreds of
// frames a second and any stall is a genuine hang, like the SPI bus deadlock
// that started all this.
//
// The net task gets a heartbeat instead; see NetWorker::stalled().
void watchdogBegin();
void watchdogSubscribe();
void watchdogFeed();

// Consecutive abnormal resets, counted in NVS and cleared once the device has
// run for a while. A device that quietly reboots every 20 seconds looks the
// same as one that is simply slow, unless it is counted.
uint32_t crashStreak();

// Call once per loop. Reports free heap periodically, and shouts once if it
// ever falls below a level where TLS would start failing.
void heapTick(uint32_t now_ms);
