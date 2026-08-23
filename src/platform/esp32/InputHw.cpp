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
// Counts per physical detent. One: this encoder emits a single pulse per detent
// on one line or the other, and only one edge of it is counted. Verified against
// encoderRawCount() on hardware rather than assumed.
constexpr int COUNTS_PER_DETENT = 1;

bool g_touch_ok = false;
uint8_t g_touch_id = 0;
bool g_haptics_ok = false;
int16_t g_last_count = 0;
// Sub-detent counts not yet reported. Losing these was a real bug: a detent
// produces about two quadrature counts, so integer-dividing each poll's delta
// discarded everything below the threshold and a slow turn did nothing at all,
// no matter how far it went.
int g_accum = 0;
int32_t g_raw_total = 0;

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
  // This is NOT a quadrature encoder, despite looking like one on the schematic.
  //
  // Established on hardware, in two steps. First the pin trace: it rests with
  // both contacts open at (1,1) and pulses them one at a time -
  //
  //   A=0 B=1 -> A=1 B=1        (one direction)
  //   A=1 B=0 -> A=1 B=1        (the other)
  //
  // - never passing through (0,0). Then the counter: configured as 4x quadrature
  // it oscillated -1, 0, -1, 0 and netted exactly zero, because both edges of a
  // single A pulse were counted with opposite signs. In real quadrature B
  // changes state between A's two edges, which is precisely what makes them
  // count the same way; that they cancelled is the proof that B does not move.
  //
  // So each line is an independent direction pulse: count ONE edge per line, and
  // let which line it was decide the sign.
  pcnt_config_t ch0 = {};
  ch0.pulse_gpio_num = pins::ENC_A;
  ch0.ctrl_gpio_num = PCNT_PIN_NOT_USED;
  ch0.channel = PCNT_CHANNEL_0;
  ch0.unit = ENC_UNIT;
  ch0.neg_mode = PCNT_COUNT_INC;  // falling edge on A: one step clockwise
  ch0.pos_mode = PCNT_COUNT_DIS;  // the release is the same detent, not another
  ch0.lctrl_mode = PCNT_MODE_KEEP;
  ch0.hctrl_mode = PCNT_MODE_KEEP;
  ch0.counter_h_lim = 30000;
  ch0.counter_l_lim = -30000;
  if (pcnt_unit_config(&ch0) != ESP_OK) return false;

  pcnt_config_t ch1 = ch0;
  ch1.pulse_gpio_num = pins::ENC_B;
  ch1.channel = PCNT_CHANNEL_1;
  ch1.neg_mode = PCNT_COUNT_DEC;  // falling edge on B: one step anticlockwise
  ch1.pos_mode = PCNT_COUNT_DIS;
  if (pcnt_unit_config(&ch1) != ESP_OK) return false;

  // A mechanical encoder's contacts bounce. Without the hardware filter one
  // detent reads as a burst, which is the classic "my knob jumps ten steps".
  // The value is in APB cycles: 1000 at 80 MHz is 12.5 us.
  pcnt_set_filter_value(ENC_UNIT, 1000);
  pcnt_filter_enable(ENC_UNIT);
  pcnt_counter_pause(ENC_UNIT);
  pcnt_counter_clear(ENC_UNIT);
  pcnt_counter_resume(ENC_UNIT);
  g_last_count = 0;

  // The board has 10K pull-ups on both lines (schematic sheet 1, R59/R60); the
  // internal ones cost nothing and make the pins defined if a variant omits them.
  gpio_set_pull_mode(static_cast<gpio_num_t>(pins::ENC_A), GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(pins::ENC_B), GPIO_PULLUP_ONLY);
  g_accum = 0;
  g_raw_total = 0;
  return true;
}

int encoderDelta() {
  int16_t now = 0;
  if (pcnt_get_counter_value(ENC_UNIT, &now) != ESP_OK) return 0;
  // Signed 16-bit subtraction, so it is correct across the counter's wrap.
  const int d = static_cast<int16_t>(now - g_last_count);
  g_last_count = now;
  g_raw_total += d;

  // Accumulate, then report whole detents and KEEP the remainder.
  //
  // This counts both edges of channel A with channel B selecting direction, so
  // one quadrature cycle - one detent on an EC11 - is two counts. Dropping the
  // remainder each poll meant a slow turn never accumulated to a whole detent
  // and the knob appeared completely dead. Integer division truncating toward
  // zero is what makes this correct in both directions.
  g_accum += d;
  const int detents = g_accum / COUNTS_PER_DETENT;
  g_accum -= detents * COUNTS_PER_DETENT;
  return detents;
}

int32_t encoderRawCount() { return g_raw_total; }

void encoderPlainInputMode() {
  pcnt_counter_pause(ENC_UNIT);
  pinMode(pins::ENC_A, INPUT_PULLUP);
  pinMode(pins::ENC_B, INPUT_PULLUP);
}

int encoderProbe() {
  const int a = digitalRead(pins::ENC_A) ? 1 : 0;
  const int b = digitalRead(pins::ENC_B) ? 1 : 0;
  return (a << 1) | b;
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
