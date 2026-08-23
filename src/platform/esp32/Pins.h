#pragma once

// Pin map for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Display pins CONFIRMED on hardware 2026-08-23: the init sequence completes,
// the test pattern renders with correct RGB order (MADCTL 0x00, no BGR bit),
// correct orientation, and an unclipped ring at r=179 - so there is no panel
// row/column offset either.
//
// Touch and encoder pins remain UNCONFIRMED. All of these came from community
// ESPHome and Tasmota configurations, because Waveshare's product page and wiki
// both answer HTTP 403. Do not build on an unconfirmed pin.

namespace pins {

// Display: ST77916, QSPI.
constexpr int LCD_SCK = 13;   // confirmed
constexpr int LCD_CS = 14;
constexpr int LCD_D0 = 15;
constexpr int LCD_D1 = 16;
constexpr int LCD_D2 = 17;
constexpr int LCD_D3 = 18;
constexpr int LCD_RST = 21;
constexpr int LCD_BL = 47;

// Touch: CST816, I2C.  UNCONFIRMED.
constexpr int TP_SDA = 11;
constexpr int TP_SCL = 12;
constexpr int TP_INT = 9;
constexpr int TP_RST = 10;

// Rotary encoder, quadrature.  UNCONFIRMED. The board carries a second encoder wired to its
// ESP32-U4WDH, which this project does not program; treated as absent.
constexpr int ENC_A = 8;
constexpr int ENC_B = 7;

}  // namespace pins
