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

namespace esp32 {

void printBootBanner();

// Consecutive abnormal resets, persisted in NVS. Read after printBootBanner().
int crashStreak();

}  // namespace esp32
