#include "InputHw.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/pcnt.h>

#include "Pins.h"

namespace esp32 {
namespace {

constexpr uint8_t CST816_ADDR = 0x15;
constexpr uint8_t DRV2605_ADDR = 0x5A;
constexpr pcnt_unit_t ENC_UNIT = PCNT_UNIT_0;

bool g_touch_ok = false;
uint8_t g_touch_id = 0;
bool g_haptics_ok = false;
int16_t g_last_count = 0;

bool readReg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  const size_t got = Wire.requestFrom(static_cast<int>(addr),
                                      static_cast<int>(n), true);
  if (got != n) return false;
  for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(Wire.read());
  return true;
}

bool writeReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission(true) == 0;
}

bool ensureBus() {
  static bool started = false;
  if (started) return true;
  started = Wire.begin(pins::TP_SDA, pins::TP_SCL, 400000);
  return started;
}

}  // namespace

void scanI2c() {
  if (!ensureBus()) {
    Serial.printf("i2c: Wire.begin(sda=%d, scl=%d) FAILED\n", pins::TP_SDA,
                  pins::TP_SCL);
    return;
  }
  Serial.printf("i2c: scanning sda=%d scl=%d ...\n", pins::TP_SDA, pins::TP_SCL);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; ++a) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission(true) == 0) {
      const char *known = a == CST816_ADDR
                              ? "  <- CST816 touch (expected)"
                              : (a == DRV2605_ADDR ? "  <- DRV2605 haptics (expected)"
                                                   : "");
      Serial.printf("i2c:   0x%02X%s\n", a, known);
      ++found;
    }
  }
  if (found == 0) {
    Serial.println("i2c:   nothing answered.");
    Serial.println("i2c:   Either the SDA/SCL pins in Pins.h are wrong - they are");
    Serial.println("i2c:   community-sourced, not from a datasheet - or the bus");
    Serial.println("i2c:   is held by something else. A wrong pin and a dead chip");
    Serial.println("i2c:   are indistinguishable from here.");
  } else {
    Serial.printf("i2c: %d device(s)\n", found);
  }
}

bool encoderBegin() {
  // Full quadrature: count on both channels' edges, with each channel's level
  // controlling the other's direction. That is what makes one physical detent
  // one count in both directions rather than two counts one way and none back.
  pcnt_config_t cfg = {};
  cfg.pulse_gpio_num = pins::ENC_A;
  cfg.ctrl_gpio_num = pins::ENC_B;
  cfg.channel = PCNT_CHANNEL_0;
  cfg.unit = ENC_UNIT;
  cfg.pos_mode = PCNT_COUNT_INC;
  cfg.neg_mode = PCNT_COUNT_DEC;
  cfg.lctrl_mode = PCNT_MODE_REVERSE;
  cfg.hctrl_mode = PCNT_MODE_KEEP;
  cfg.counter_h_lim = 30000;
  cfg.counter_l_lim = -30000;
  if (pcnt_unit_config(&cfg) != ESP_OK) return false;

  // A mechanical encoder's contacts bounce. Without the hardware filter a
  // single detent reads as a burst, which is the classic "my knob jumps ten
  // steps" symptom.
  pcnt_set_filter_value(ENC_UNIT, 1000);
  pcnt_filter_enable(ENC_UNIT);
  pcnt_counter_pause(ENC_UNIT);
  pcnt_counter_clear(ENC_UNIT);
  pcnt_counter_resume(ENC_UNIT);
  g_last_count = 0;

  gpio_set_pull_mode(static_cast<gpio_num_t>(pins::ENC_A), GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(pins::ENC_B), GPIO_PULLUP_ONLY);
  return true;
}

int encoderDelta() {
  int16_t now = 0;
  if (pcnt_get_counter_value(ENC_UNIT, &now) != ESP_OK) return 0;
  // Signed 16-bit subtraction, so it is correct across the counter's wrap.
  const int d = static_cast<int16_t>(now - g_last_count);
  g_last_count = now;
  // Four quadrature edges per detent on a typical mechanical encoder. Dividing
  // here rather than in the caller keeps "one detent" meaning one thing
  // everywhere above this.
  return d / 4;
}

bool touchBegin() {
  if (!ensureBus()) return false;
  pinMode(pins::TP_RST, OUTPUT);
  digitalWrite(pins::TP_RST, LOW);
  delay(20);
  digitalWrite(pins::TP_RST, HIGH);
  delay(60);  // the CST816 needs a moment before it answers

  uint8_t id = 0;
  if (readReg(CST816_ADDR, 0xA7, &id, 1)) g_touch_id = id;
  g_touch_ok = g_touch_id != 0;
  return g_touch_ok;
}

uint8_t touchChipId() { return g_touch_id; }

bool touchRead(int *x, int *y) {
  if (!g_touch_ok) return false;
  uint8_t b[6] = {};
  // 0x02 is the finger count, then X high/low and Y high/low. The high bytes
  // carry flags in their top nibble, hence the masks.
  if (!readReg(CST816_ADDR, 0x02, b, 6)) return false;
  if (b[0] == 0) return false;
  if (x) *x = ((b[1] & 0x0F) << 8) | b[2];
  if (y) *y = ((b[3] & 0x0F) << 8) | b[4];
  return true;
}

bool hapticsBegin() {
  if (!ensureBus()) return false;
  uint8_t status = 0;
  if (!readReg(DRV2605_ADDR, 0x00, &status, 1)) return false;

  writeReg(DRV2605_ADDR, 0x01, 0x00);  // out of standby, internal trigger
  writeReg(DRV2605_ADDR, 0x1A, 0x36);  // LRA mode
  writeReg(DRV2605_ADDR, 0x17, 0x89);  // overdrive clamp
  writeReg(DRV2605_ADDR, 0x03, 0x06);  // library 6, the LRA library
  g_haptics_ok = true;
  return true;
}

void hapticsClick() {
  if (!g_haptics_ok) return;
  writeReg(DRV2605_ADDR, 0x04, 7);  // "soft bump" - one detent
  writeReg(DRV2605_ADDR, 0x05, 0);  // end of sequence
  writeReg(DRV2605_ADDR, 0x0C, 1);  // go
}

void hapticsBump() {
  if (!g_haptics_ok) return;
  writeReg(DRV2605_ADDR, 0x04, 14);  // heavier, for a confirmed action
  writeReg(DRV2605_ADDR, 0x05, 0);
  writeReg(DRV2605_ADDR, 0x0C, 1);
}

}  // namespace esp32
