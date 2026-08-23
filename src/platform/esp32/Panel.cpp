#include "Panel.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstring>

#include "Pins.h"
#include "gfx/Geometry.h"

namespace esp32 {
namespace {

constexpr spi_host_device_t HOST = SPI2_HOST;
// 70 MHz is reported working on this panel by community drivers; 80 is not
// reliable. Raise only with a test pattern on screen to catch tearing.
constexpr int CLOCK_HZ = 70000000;
constexpr size_t CHUNK_BYTES = 32768;

spi_device_handle_t g_spi = nullptr;
uint8_t *g_chunk[2] = {nullptr, nullptr};
uint16_t *g_band[2] = {nullptr, nullptr};
int g_band_slot = 0;
spi_transaction_t g_trans[2];
bool g_busy[2] = {false, false};
int g_inflight = 0;
uint32_t g_last_push_us = 0;

// The vendor init sequence, as {cmd, param_count, delay_ms, params...}.
const uint8_t kInit[] = {
    0xF0, 1, 0, 0x28,
    0xF2, 1, 0, 0x28,
    0x73, 1, 0, 0xF0,
    0x7C, 1, 0, 0xD1,
    0x83, 1, 0, 0xE0,
    0x84, 1, 0, 0x61,
    0xF2, 1, 0, 0x82,
    0xF0, 1, 0, 0x00,
    0xF0, 1, 0, 0x01,
    0xF1, 1, 0, 0x01,
    0xB0, 1, 0, 0x69,
    0xB1, 1, 0, 0x4A,
    0xB2, 1, 0, 0x2F,
    0xB3, 1, 0, 0x01,
    0xB4, 1, 0, 0x69,
    0xB5, 1, 0, 0x45,
    0xB6, 1, 0, 0xAB,
    0xB7, 1, 0, 0x41,
    0xB8, 1, 0, 0x86,
    0xB9, 1, 0, 0x15,
    0xBA, 1, 0, 0x00,
    0xBB, 1, 0, 0x08,
    0xBC, 1, 0, 0x08,
    0xBD, 1, 0, 0x00,
    0xBE, 1, 0, 0x00,
    0xBF, 1, 0, 0x07,
    0xC0, 1, 0, 0x80,
    0xC1, 1, 0, 0x10,
    0xC2, 1, 0, 0x37,
    0xC3, 1, 0, 0x80,
    0xC4, 1, 0, 0x10,
    0xC5, 1, 0, 0x37,
    0xC6, 1, 0, 0xA9,
    0xC7, 1, 0, 0x41,
    0xC8, 1, 0, 0x01,
    0xC9, 1, 0, 0xA9,
    0xCA, 1, 0, 0x41,
    0xCB, 1, 0, 0x01,
    0xCC, 1, 0, 0x7F,
    0xCD, 1, 0, 0x7F,
    0xCE, 1, 0, 0xFF,
    0xD0, 1, 0, 0x91,
    0xD1, 1, 0, 0x68,
    0xD2, 1, 0, 0x68,
    0xF5, 2, 0, 0x00, 0xA5,
    0xF1, 1, 0, 0x10,
    0xF0, 1, 0, 0x00,
    0xF0, 1, 0, 0x02,
    0xE0, 14, 0, 0xF0, 0x10, 0x18, 0x0D, 0x0C, 0x38, 0x3E, 0x44, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34,
    0xE1, 14, 0, 0xF0, 0x0F, 0x17, 0x0D, 0x0B, 0x07, 0x3E, 0x33, 0x51, 0x39, 0x15, 0x15, 0x30, 0x34,
    0xF0, 1, 0, 0x10,
    0xF3, 1, 0, 0x10,
    0xE0, 1, 0, 0x08,
    0xE1, 1, 0, 0x00,
    0xE2, 1, 0, 0x00,
    0xE3, 1, 0, 0x00,
    0xE4, 1, 0, 0xE0,
    0xE5, 1, 0, 0x06,
    0xE6, 1, 0, 0x21,
    0xE7, 1, 0, 0x03,
    0xE8, 1, 0, 0x05,
    0xE9, 1, 0, 0x02,
    0xEA, 1, 0, 0xE9,
    0xEB, 1, 0, 0x00,
    0xEC, 1, 0, 0x00,
    0xED, 1, 0, 0x14,
    0xEE, 1, 0, 0xFF,
    0xEF, 1, 0, 0x00,
    0xF8, 1, 0, 0xFF,
    0xF9, 1, 0, 0x00,
    0xFA, 1, 0, 0x00,
    0xFB, 1, 0, 0x30,
    0xFC, 1, 0, 0x00,
    0xFD, 1, 0, 0x00,
    0xFE, 1, 0, 0x00,
    0xFF, 1, 0, 0x00,
    0x60, 1, 0, 0x40,
    0x61, 1, 0, 0x05,
    0x62, 1, 0, 0x00,
    0x63, 1, 0, 0x42,
    0x64, 1, 0, 0xDA,
    0x65, 1, 0, 0x00,
    0x66, 1, 0, 0x00,
    0x67, 1, 0, 0x00,
    0x68, 1, 0, 0x00,
    0x69, 1, 0, 0x00,
    0x6A, 1, 0, 0x00,
    0x6B, 1, 0, 0x00,
    0x70, 1, 0, 0x40,
    0x71, 1, 0, 0x04,
    0x72, 1, 0, 0x00,
    0x73, 1, 0, 0x42,
    0x74, 1, 0, 0xD9,
    0x75, 1, 0, 0x00,
    0x76, 1, 0, 0x00,
    0x77, 1, 0, 0x00,
    0x78, 1, 0, 0x00,
    0x79, 1, 0, 0x00,
    0x7A, 1, 0, 0x00,
    0x7B, 1, 0, 0x00,
    0x80, 1, 0, 0x48,
    0x81, 1, 0, 0x00,
    0x82, 1, 0, 0x07,
    0x83, 1, 0, 0x02,
    0x84, 1, 0, 0xD7,
    0x85, 1, 0, 0x04,
    0x86, 1, 0, 0x00,
    0x87, 1, 0, 0x00,
    0x88, 1, 0, 0x48,
    0x89, 1, 0, 0x00,
    0x8A, 1, 0, 0x09,
    0x8B, 1, 0, 0x02,
    0x8C, 1, 0, 0xD9,
    0x8D, 1, 0, 0x04,
    0x8E, 1, 0, 0x00,
    0x8F, 1, 0, 0x00,
    0x90, 1, 0, 0x48,
    0x91, 1, 0, 0x00,
    0x92, 1, 0, 0x0B,
    0x93, 1, 0, 0x02,
    0x94, 1, 0, 0xDB,
    0x95, 1, 0, 0x04,
    0x96, 1, 0, 0x00,
    0x97, 1, 0, 0x00,
    0x98, 1, 0, 0x48,
    0x99, 1, 0, 0x00,
    0x9A, 1, 0, 0x0D,
    0x9B, 1, 0, 0x02,
    0x9C, 1, 0, 0xDD,
    0x9D, 1, 0, 0x04,
    0x9E, 1, 0, 0x00,
    0x9F, 1, 0, 0x00,
    0xA0, 1, 0, 0x48,
    0xA1, 1, 0, 0x00,
    0xA2, 1, 0, 0x06,
    0xA3, 1, 0, 0x02,
    0xA4, 1, 0, 0xD6,
    0xA5, 1, 0, 0x04,
    0xA6, 1, 0, 0x00,
    0xA7, 1, 0, 0x00,
    0xA8, 1, 0, 0x48,
    0xA9, 1, 0, 0x00,
    0xAA, 1, 0, 0x08,
    0xAB, 1, 0, 0x02,
    0xAC, 1, 0, 0xD8,
    0xAD, 1, 0, 0x04,
    0xAE, 1, 0, 0x00,
    0xAF, 1, 0, 0x00,
    0xB0, 1, 0, 0x48,
    0xB1, 1, 0, 0x00,
    0xB2, 1, 0, 0x0A,
    0xB3, 1, 0, 0x02,
    0xB4, 1, 0, 0xDA,
    0xB5, 1, 0, 0x04,
    0xB6, 1, 0, 0x00,
    0xB7, 1, 0, 0x00,
    0xB8, 1, 0, 0x48,
    0xB9, 1, 0, 0x00,
    0xBA, 1, 0, 0x0C,
    0xBB, 1, 0, 0x02,
    0xBC, 1, 0, 0xDC,
    0xBD, 1, 0, 0x04,
    0xBE, 1, 0, 0x00,
    0xBF, 1, 0, 0x00,
    0xC0, 1, 0, 0x10,
    0xC1, 1, 0, 0x47,
    0xC2, 1, 0, 0x56,
    0xC3, 1, 0, 0x65,
    0xC4, 1, 0, 0x74,
    0xC5, 1, 0, 0x88,
    0xC6, 1, 0, 0x99,
    0xC7, 1, 0, 0x01,
    0xC8, 1, 0, 0xBB,
    0xC9, 1, 0, 0xAA,
    0xD0, 1, 0, 0x10,
    0xD1, 1, 0, 0x47,
    0xD2, 1, 0, 0x56,
    0xD3, 1, 0, 0x65,
    0xD4, 1, 0, 0x74,
    0xD5, 1, 0, 0x88,
    0xD6, 1, 0, 0x99,
    0xD7, 1, 0, 0x01,
    0xD8, 1, 0, 0xBB,
    0xD9, 1, 0, 0xAA,
    0xF3, 1, 0, 0x01,
    0xF0, 1, 0, 0x00,
    0x3A, 1, 0, 0x05,
    0x35, 1, 0, 0x00,
    0x36, 1, 0, 0x00,
    0x21, 0, 0,
    0x11, 0, 120,
    0x29, 0, 0,
    0x2C, 0, 0,
};

inline void csLow() { digitalWrite(pins::LCD_CS, LOW); }
inline void csHigh() { digitalWrite(pins::LCD_CS, HIGH); }

// Single-line write. Used for commands and for the pixel-stream prefix.
void writeSingle(const uint8_t *data, size_t len) {
  if (!g_spi || !data || !len) return;
  spi_transaction_t t = {};
  t.length = len * 8;
  t.tx_buffer = data;
  spi_device_polling_transmit(g_spi, &t);
}

void cmd(uint8_t c, const uint8_t *params, size_t n) {
  const uint8_t packet[4] = {0x02, 0x00, c, 0x00};
  csLow();
  writeSingle(packet, sizeof(packet));
  if (params && n) writeSingle(params, n);
  csHigh();
}

void setFullWindow() {
  const uint8_t cols[4] = {0x00, 0x00,
                           static_cast<uint8_t>((gfx::W - 1) >> 8),
                           static_cast<uint8_t>((gfx::W - 1) & 0xFF)};
  const uint8_t rows[4] = {0x00, 0x00,
                           static_cast<uint8_t>((gfx::H - 1) >> 8),
                           static_cast<uint8_t>((gfx::H - 1) & 0xFF)};
  cmd(0x2A, cols, 4);
  cmd(0x2B, rows, 4);
  cmd(0x2C, nullptr, 0);
}

// Swap the byte order of two pixels at a time.
//
// The panel wants big-endian; the framebuffer is native-endian by design. Doing
// it 32 bits at a time is two operations per pixel instead of three, and the
// measured internal-SRAM numbers showed these per-pixel loops are CPU-bound,
// so that ratio is the whole cost.
void swapInPlace(uint16_t *px, size_t count) {
  uint32_t *p = reinterpret_cast<uint32_t *>(px);
  const size_t pairs = count / 2;
  for (size_t i = 0; i < pairs; ++i) {
    const uint32_t v = p[i];
    p[i] = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
  }
  if (count & 1) {
    const uint16_t v = px[count - 1];
    px[count - 1] = static_cast<uint16_t>((v >> 8) | (v << 8));
  }
}

void queueQuad(const void *buf, size_t bytes, int slot) {
  spi_transaction_t *t = &g_trans[slot];
  std::memset(t, 0, sizeof(*t));
  t->flags = SPI_TRANS_MODE_QIO;
  t->length = bytes * 8;
  t->tx_buffer = buf;
  if (spi_device_queue_trans(g_spi, t, portMAX_DELAY) == ESP_OK) {
    ++g_inflight;
    g_busy[slot] = true;
  }
}

void drainAll() {
  spi_transaction_t *r = nullptr;
  while (g_inflight > 0) {
    if (spi_device_get_trans_result(g_spi, &r, portMAX_DELAY) != ESP_OK) break;
    --g_inflight;
    g_busy[static_cast<size_t>(r - g_trans)] = false;
  }
}

void waitSlot(int slot) {
  spi_transaction_t *r = nullptr;
  while (g_busy[slot]) {
    if (spi_device_get_trans_result(g_spi, &r, portMAX_DELAY) != ESP_OK) break;
    --g_inflight;
    g_busy[static_cast<size_t>(r - g_trans)] = false;
  }
}

}  // namespace

uint32_t panelLastPushUs() { return g_last_push_us; }

void panelBacklight(uint8_t duty) {
  ledcWrite(0, duty);
}

bool panelBegin() {
  pinMode(pins::LCD_CS, OUTPUT);
  digitalWrite(pins::LCD_CS, HIGH);
  pinMode(pins::LCD_RST, OUTPUT);
  digitalWrite(pins::LCD_RST, HIGH);

  // Backlight off during init so a garbage first frame is never visible.
  ledcSetup(0, 20000, 8);
  ledcAttachPin(pins::LCD_BL, 0);
  ledcWrite(0, 0);

  spi_bus_config_t bus = {};
  bus.sclk_io_num = pins::LCD_SCK;
  bus.mosi_io_num = pins::LCD_D0;
  bus.miso_io_num = pins::LCD_D1;
  bus.quadwp_io_num = pins::LCD_D2;
  bus.quadhd_io_num = pins::LCD_D3;
  bus.max_transfer_sz = static_cast<int>(CHUNK_BYTES);
  esp_err_t err = spi_bus_initialize(HOST, &bus, SPI_DMA_CH_AUTO);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    Serial.printf("panel: spi_bus_initialize failed: %d\n", (int)err);
    return false;
  }

  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = CLOCK_HZ;
  dev.mode = 0;
  dev.spics_io_num = -1;  // CS is driven by hand so it can span a whole frame
  dev.queue_size = 4;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  if (spi_bus_add_device(HOST, &dev, &g_spi) != ESP_OK) {
    Serial.println("panel: spi_bus_add_device failed");
    return false;
  }

  const size_t band_bytes =
      static_cast<size_t>(gfx::W) * PANEL_BAND_H * sizeof(uint16_t);
  for (int i = 0; i < 2; ++i) {
    g_chunk[i] = static_cast<uint8_t *>(
        heap_caps_malloc(CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!g_chunk[i]) {
      Serial.println("panel: DMA staging buffer allocation failed");
      return false;
    }
    // Bands must be in internal SRAM: it is DMA-capable, and the measurements
    // put internal writes at 181 MB/s against PSRAM's hard 33 MB/s ceiling.
    g_band[i] = static_cast<uint16_t *>(
        heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!g_band[i]) {
      Serial.println("panel: internal band buffer allocation failed");
      return false;
    }
  }
  Serial.printf("panel: 2 x %u B band buffers in internal SRAM\n",
                (unsigned)band_bytes);

  // Hardware reset.
  digitalWrite(pins::LCD_RST, LOW);
  delay(100);
  digitalWrite(pins::LCD_RST, HIGH);
  delay(120);

  // Run the init table.
  size_t i = 0;
  int count = 0;
  while (i + 3 <= sizeof(kInit)) {
    const uint8_t c = kInit[i];
    const uint8_t n = kInit[i + 1];
    const uint8_t d = kInit[i + 2];
    cmd(c, n ? &kInit[i + 3] : nullptr, n);
    if (d) delay(d);
    i += 3 + n;
    ++count;
  }
  Serial.printf("panel: init sequence sent, %d commands\n", count);
  return true;
}

void panelBeginFrame() {
  g_last_push_us = esp_timer_get_time();
  setFullWindow();
  csLow();
  const uint8_t prefix[4] = {0x32, 0x00, 0x2C, 0x00};
  writeSingle(prefix, sizeof(prefix));
  g_inflight = 0;
  g_busy[0] = g_busy[1] = false;
  g_band_slot = 0;
}

uint16_t *panelNextBand() {
  waitSlot(g_band_slot);
  return g_band[g_band_slot];
}

void panelCommitBand() {
  const size_t px = static_cast<size_t>(gfx::W) * PANEL_BAND_H;
  // The band lives in internal SRAM, which is DMA-capable, so it goes to the
  // hardware directly with no staging copy. Slots alternate so one band streams
  // out while the caller composites the next.
  swapInPlace(g_band[g_band_slot], px);
  queueQuad(g_band[g_band_slot], px * 2, g_band_slot);
  g_band_slot ^= 1;
}

void panelEndFrame() {
  drainAll();
  csHigh();
  g_last_push_us = static_cast<uint32_t>(esp_timer_get_time() - g_last_push_us);
}

void panelPushFrame(const uint16_t *src) {
  const uint32_t t0 = esp_timer_get_time();
  setFullWindow();
  csLow();
  const uint8_t prefix[4] = {0x32, 0x00, 0x2C, 0x00};
  writeSingle(prefix, sizeof(prefix));

  g_inflight = 0;
  g_busy[0] = g_busy[1] = false;

  const size_t chunk_px = CHUNK_BYTES / 2;
  size_t left = static_cast<size_t>(gfx::W) * gfx::H;
  int slot = 0;
  while (left) {
    const size_t n = left < chunk_px ? left : chunk_px;
    waitSlot(slot);
    uint16_t *dst = reinterpret_cast<uint16_t *>(g_chunk[slot]);
    for (size_t k = 0; k < n; ++k) {
      const uint16_t v = src[k];
      dst[k] = static_cast<uint16_t>((v >> 8) | (v << 8));
    }
    queueQuad(g_chunk[slot], n * 2, slot);
    src += n;
    left -= n;
    slot ^= 1;
  }
  drainAll();
  csHigh();
  g_last_push_us = static_cast<uint32_t>(esp_timer_get_time() - t0);
}

}  // namespace esp32
