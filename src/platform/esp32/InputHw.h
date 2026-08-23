#pragma once

// Knob, touch and haptics on the device.
//
// All three sit behind this so the gesture logic above them stays hardware-free
// and host-tested. The pin numbers these use are still community-sourced (see
// Pins.h); scanI2c() exists to confirm them without anyone having to touch the
// board, which matters because a wrong I2C pin and a dead chip look identical.

#include <cstdint>

namespace esp32 {

// Probes every address on the touch I2C bus and prints what answers. Expected:
// CST816 touch at 0x15, DRV2605 haptic driver at 0x5A.
void scanI2c();

// --- rotary encoder ---
//
// Uses the PCNT peripheral rather than interrupts. Hardware quadrature decoding
// does not miss steps during a fast spin, and a knob that loses steps when spun
// quickly feels broken in a way no amount of UI polish covers up.
bool encoderBegin();
// Net detents since the last call. Positive is clockwise. Sub-detent counts are
// carried over rather than discarded.
int encoderDelta();

// Diagnostic: configure the encoder pins as plain pulled-up inputs, with no
// PCNT involvement at all. Used to separate "the knob is not wired to this MCU"
// from "the counter is configured wrongly" - two failures that produce an
// identical silent log.
void encoderPlainInputMode();

// Raw A/B pin levels as a 2-bit value (bit1 = A, bit0 = B).
//
// This exists to separate two failures that look identical from the outside: a
// wrong pin number, and a correct pin with a wrong PCNT configuration. If these
// levels do not change while the knob turns, no counter setting will help.
int encoderProbe();

// Total raw quadrature counts since boot. Exposed so the counts-per-detent
// figure can be checked against a known number of physical clicks instead of
// being assumed.
int32_t encoderRawCount();

// --- capacitive touch ---
bool touchBegin();
// Returns true while a finger is down, filling x and y in panel coordinates.
bool touchRead(int *x, int *y);
// Chip id read back at init, for the log. 0 means nothing answered.
uint8_t touchChipId();

// --- haptics ---
bool hapticsBegin();
void hapticsClick();  // one detent
void hapticsBump();   // heavier confirmation, for like and skip

}  // namespace esp32
