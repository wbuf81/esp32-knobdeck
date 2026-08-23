// SD card mount.
//
// Arduino's SD library registers with the ESP-IDF VFS at /sd, so the ordinary
// fopen/mkdir/rename in ArtCache and ArtRenderer work unchanged on the device.
// That is why the storage code needed no porting at all.

#if defined(DEVICE)

#include <SD.h>
#include <SPI.h>

#include "../../net/NetLog.h"
#include "Esp32Storage.h"

namespace {
bool g_mounted = false;
uint32_t g_hz = 0;
}

uint32_t storageClockHz() { return g_hz; }

bool storageAvailable() { return g_mounted; }

bool mountStorage() {
  // Try progressively slower clocks. Marginal cards and longer signal paths
  // often fail at 25MHz and mount fine at 10 or 4, and a slow cache is far
  // better than none — artwork is written once per album, not per frame.
  static const uint32_t SPEEDS[] = {25000000, 10000000, 4000000, 1000000};

  for (uint32_t hz : SPEEDS) {
    // GPIO4 is the SD chip select on the M5Stack Core.
    if (SD.begin(GPIO_NUM_4, SPI, hz)) {
      const uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
      NETLOG("SD mounted at %uMHz, %llu MB, type %d", hz / 1000000, mb,
             (int)SD.cardType());
      if (mb > 33000) {
        // M5Stack documents this slot as 16GB maximum. Larger cards are SDXC,
        // which the SPI-mode driver frequently cannot initialise.
        NETLOG("note: %llu MB exceeds the documented 16GB limit for this slot",
               mb);
      }
      g_mounted = true;
      g_hz = hz;
      return true;
    }
    SD.end();
  }

  // Reached only after every speed failed.
  NETLOG("SD not mounted at any clock. Card must be FAT32 on an MBR scheme,");
  NETLOG("and 32GB or smaller — this slot is documented for 16GB maximum.");
  g_mounted = false;
  return false;
}

#endif  // DEVICE
