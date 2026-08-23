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
//
// The panel owns the two band buffers rather than the caller, because the hazard
// here is invisible and expensive: commitBand byte-swaps a band in place and
// hands it to DMA without waiting, so touching that memory again before the
// hardware has finished streaming it corrupts the frame mid-push. Handing out
// the buffer makes that impossible to get wrong at the call site.
//
// One frame is:
//     panelBeginFrame();
//     for each band:  uint16_t *b = panelNextBand();  ...draw...  panelCommitBand();
//     panelEndFrame();
//
// The address window is set once per frame and the pixel stream is a single
// CS-low transaction spanning every band.

// Rows per band.
//
// 20 divides 360 into eighteen bands and is exactly five rows of the 90x90 bloom
// accumulator, so nothing straddles a boundary. It was 40, which worked until
// the network and Spotify layers were linked in: two 40-row bands need 57.6 KB
// of DMA-capable internal SRAM, and that left mbedTLS with 13 KB of headroom -
// enough for the one session it had, and nothing for the second one an artwork
// download opens. Halving the band height buys 28.8 KB back for the cost of nine
// more DMA transfers per frame, which are asynchronous anyway.
constexpr int PANEL_BAND_H = 20;

void panelBeginFrame();
// Blocks until this band's previous DMA has completed, then returns it to draw
// into. Bands are returned top to bottom, wrapping each frame.
uint16_t *panelNextBand();
// Byte-swaps the band just drawn and queues it. Do not touch that pointer after.
void panelCommitBand();
void panelEndFrame();

// Microseconds the last frame's pixel stream occupied the bus.
uint32_t panelLastPushUs();

}  // namespace esp32
