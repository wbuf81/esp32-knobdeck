#pragma once

// ST77916 over QSPI: the Waveshare knob's 360x360 round panel.
//
// Written directly against driver/spi_master.h rather than esp_lcd, because the
// Arduino ESP32 core here is IDF 4.4.7 and its esp_lcd_panel_io_spi_config_t has
// no quad_mode flag - only octal. spi_master does have SPI_TRANS_MODE_QIO, so
// the quad path is available; it just has to be driven by hand.
//
// The QSPI framing is not ordinary SPI. Every command is a four-byte packet
// {0x02, 0x00, cmd, 0x00} sent on ONE data line, and pixel data is prefixed
// with {0x32, 0x00, 0x2C, 0x00} and then streamed on FOUR lines. A driver that
// ignores this produces a black screen and no error at all, which is why it is
// worth stating here.
//
// The panel wants big-endian pixels. This project's framebuffer is deliberately
// native-endian so that every blend operates on channels where they look like
// they are, so the swap happens here, once, on the way out - and nowhere else.

#include <cstddef>
#include <cstdint>

namespace esp32 {

// Bring up the SPI bus, reset the panel, run the vendor init sequence.
bool panelBegin();

// LEDC-driven backlight, 0..255.
void panelBacklight(uint8_t duty);

// --- simple blocking path, for bring-up and test patterns ---
// Pushes a whole 360x360 native-endian frame. Byte-swaps as it stages.
void panelPushFrame(const uint16_t *native_endian);

// --- band path, which is what the renderer actually uses ---
// One frame is: beginFrame, then one pushBand per band top to bottom, then
// endFrame. The address window is set once per frame and the pixel stream runs
// as a single CS-low transaction across every band.
//
// pushBand byte-swaps the band IN PLACE and then hands it to DMA, so the caller
// must ping-pong two band buffers and must not touch a band until the following
// pushBand or endFrame returns.
void panelBeginFrame();
void panelPushBand(uint16_t *band_native_endian, size_t pixel_count);
void panelEndFrame();

// Microseconds the last frame's pixel stream occupied the bus.
uint32_t panelLastPushUs();

}  // namespace esp32
