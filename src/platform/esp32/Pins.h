#pragma once

// Pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// UNCONFIRMED. Waveshare's product page and wiki both answer HTTP 403, so these
// came from community ESPHome and Tasmota configurations for the same ST77916
// panel. Every one is confirmed on hardware during bring-up and this comment
// updated to say so. Do not build anything on top of an unconfirmed pin.

namespace pins {

// Display: ST77916, QSPI.
constexpr int LCD_SCK = 13;
constexpr int LCD_CS = 14;
constexpr int LCD_D0 = 15;
constexpr int LCD_D1 = 16;
constexpr int LCD_D2 = 17;
constexpr int LCD_D3 = 18;
constexpr int LCD_RST = 21;
constexpr int LCD_BL = 47;

// Touch: CST816, I2C.
constexpr int TP_SDA = 11;
constexpr int TP_SCL = 12;
constexpr int TP_INT = 9;
constexpr int TP_RST = 10;

// Rotary encoder, quadrature. The board carries a second encoder wired to its
// ESP32-U4WDH, which this project does not program; treated as absent.
constexpr int ENC_A = 8;
constexpr int ENC_B = 7;

}  // namespace pins
