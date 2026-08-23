# Waveshare ESP32-S3-Knob-Touch-LCD-1.8 — platform reference

Everything a new project needs for this board: the pinout, the numbers that
actually constrain a design, and every trap this project fell into.

Written to be copied into another repo as-is. It does not depend on anything
else here.

**Every figure below was measured on the physical unit, or read off the
manufacturer's schematic.** Where something is still unverified it says so.

---

## 1. The board

| Item | Value |
|---|---|
| Main MCU | ESP32-S3R8, 2x LX7 @ 240 MHz |
| Second MCU | ESP32-U4WDH (not programmed by this project) |
| Flash | 16 MB |
| PSRAM | 8 MB octal — **8,388,607 B reported, 8,257,524 B largest block** |
| Internal SRAM | 327,680 B total; **370,508 B heap free at boot, 110,580 B largest block** |
| Display | 360x360 **round**, ST77916, **QSPI** |
| Touch | CST816, I2C, chip id `0xB6` |
| Encoder | mechanical, **not quadrature** (see §6) |
| Haptics | DRV2605 driving an LRA, on the touch I2C bus |
| Microphone | MSM261D4030H1CPM, **PDM** |
| Audio out | PCM5100A I2S DAC, 3.5 mm jack |
| SD card | SDMMC 4-bit — **not needed, see §8** |
| Serial port | `/dev/cu.usbmodem*`, native USB-JTAG (VID `303A` PID `1001`) |

The second MCU shares the audio DAC through a CH445P analog switch selected by
GPIO0, and has its own encoder. Neither is used here.

---

## 2. Pinout

From the manufacturer's schematic, and every display/touch/encoder pin confirmed
working on hardware.

| Function | GPIO | Confirmed |
|---|---|---|
| LCD QSPI SCK | 13 | yes |
| LCD QSPI CS | 14 | yes |
| LCD QSPI D0–D3 | 15, 16, 17, 18 | yes |
| LCD reset | 21 | yes |
| LCD backlight | 47 | yes |
| Touch SDA / SCL | 11 / 12 | yes (`0x15` answers) |
| Touch INT / RST | 9 / 10 | yes |
| Encoder A / B | 8 / 7 | yes |
| Haptics | on the touch I2C bus, `0x5A` | yes |
| PDM mic SCK / DATA | 45 / 46 | schematic only |
| I2S DAC BCK / LRCK / DIN | 39 / 40 / 41 | schematic only |
| I2S source switch | 0 | schematic only |
| Battery sense | 1 (ADC, 2:1 divider) | schematic only |
| SDMMC CMD/SCK/D0–D3 | 3 / 4 / 5 / 6 / 42 / 2 | schematic only |
| UART to second MCU | TX 38, RX 48 | schematic only |
| USB D-/D+ | 19 / 20 | — |

**The knob's press is not readable from the S3.** GPIO0 is the only candidate —
the schematic puts a 10K pull-up on it beside the encoder's — and it does not
move when the knob is pressed. Design selection around the touch panel.

GPIO 33–37 are the octal PSRAM and 26–32 the flash. Nothing else is free: every
other pin is committed above.

---

## 3. Toolchain

PlatformIO with the Arduino framework. **Arduino here is ESP-IDF 4.4.7**, so IDF
APIs are callable directly — which is how this project drives the QSPI panel and
uses the ROM JPEG decoder without any library.

Every `pio` command needs `HOMEBREW_PREFIX` set on macOS or it cannot find its
toolchain.

```ini
[env:esp32]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

; Two unflags, both load-bearing:
;   -std=gnu++11  the core appends it and it beats anything in [env]
;   -Os           the core's default; see §5
build_unflags = -std=gnu++11 -Os
build_flags = -std=gnu++14 -O3 -funroll-loops -DBOARD_HAS_PSRAM
              -DARDUINO_USB_MODE=1 -DARDUINO_USB_CDC_ON_BOOT=1

; OCTAL PSRAM. Getting this wrong gives a board that boots and reports ZERO
; PSRAM, which reads exactly like a hardware fault.
board_build.arduino.memory_type = qio_opi
board_build.flash_mode = qio
board_build.f_flash = 80000000L
board_build.partitions = huge_app.csv
board_upload.flash_size = 16MB
board_build.flash_size = 16MB
monitor_filters = esp32_exception_decoder
```

---

## 4. Memory: internal SRAM is the binding constraint, not PSRAM

This is the single most important fact about the board, and it is the opposite of
what 8 MB of PSRAM suggests.

Measured bandwidth, one 360x360 RGB565 frame (259,200 B), 30 iterations:

| Operation | PSRAM | Internal SRAM |
|---|---|---|
| write, 16-bit stores | 33.2 MB/s | 90.9 MB/s |
| write, 32-bit stores | 33.2 MB/s | **181.7 MB/s** |
| `memset` | 33.2 MB/s | **725.3 MB/s** |
| read, 32-bit | 56.1 MB/s | 151.5 MB/s |
| read-modify-write (fade) | 34.6 MB/s | 43.3 MB/s |

Four conclusions:

1. **PSRAM writes are hard-capped at 33 MB/s.** 16-bit, 32-bit and `memset` all
   measure identically, so it is a bus ceiling and no access pattern beats it.
   Any full-frame pass over a PSRAM framebuffer costs **at least 7.45 ms**.
2. **Internal SRAM scales with access width; PSRAM does not.** 32-bit stores are
   exactly twice 16-bit. Every fill loop should write pixel pairs.
3. **Per-pixel work is CPU-bound, not memory-bound.** `fade` costs 11.4 ms in
   internal SRAM against 1.36 ms to write the same bytes. Moving hot passes
   between memories buys ~25%; reducing operations per pixel, or the number of
   passes, buys multiples.
4. **Internal SRAM runs out long before PSRAM does.** With a renderer and the
   network stack linked in, free internal heap lands around 40 KB — and
   **mbedTLS needs ~34 KB of it contiguous per session.** Anything measured in
   tens of kilobytes must go to PSRAM explicitly.

### Rules that came out of that

- Put every large buffer in PSRAM with `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`.
  **Do not fall back to internal on failure** — it succeeds for the small cases
  and then starves TLS somewhere else entirely.
- Never add a pass that reads *and* writes every pixel. Fold work into a write
  you are already making.
- Never do per-pixel what can be done per row, or per row what can be done per
  frame. This project paid for that lesson three times.
- One aligned 32-bit access beats three byte accesses, every time.

---

## 5. Compiler flags are worth 40%

The core defaults to `-Os`, which declines to unroll integer inner loops. On a
real-time renderer:

- `-Os` → `-O2` took one pass from 42 ms to 33.8 ms.
- `-O2` → `-O3 -funroll-loops` took it from 16.4 ms to 10.6 ms.

Every render loop here measured about **3.5 cycles per operation**, which is too
uniform to be instruction count: it is load-to-use stalls on an in-order core.
Unrolling interleaves independent load chains, and that is the only thing that
hides them. Hand-unrolling measured *worse* than letting the compiler do it.

---

## 6. The encoder is not quadrature

It rests with both contacts open at `(1,1)` and pulses them one at a time,
**never passing through `(0,0)`**:

```
A=0 B=1 -> A=1 B=1     one direction
A=1 B=0 -> A=1 B=1     the other
```

A standard 4x quadrature PCNT configuration counts nothing useful: it oscillates
`-1, 0, -1, 0` and nets exactly zero, because both edges of a single A pulse are
counted with opposite signs. **That cancellation is the diagnostic** — in real
quadrature B changes state between A's two edges, which is precisely what makes
them count the same way.

Configure PCNT with one counted edge per line and the line deciding the sign:

```c
ch0.pulse_gpio_num = ENC_A;  ch0.ctrl_gpio_num = PCNT_PIN_NOT_USED;
ch0.neg_mode = PCNT_COUNT_INC;  ch0.pos_mode = PCNT_COUNT_DIS;
ch1.pulse_gpio_num = ENC_B;  ch1.channel = PCNT_CHANNEL_1;
ch1.neg_mode = PCNT_COUNT_DEC;  ch1.pos_mode = PCNT_COUNT_DIS;
pcnt_set_filter_value(unit, 1000);   // 12.5us at 80MHz; contacts bounce
```

One count per detent. And when converting counts to detents, **keep the
remainder** — dividing each poll's delta and discarding the rest means a slow
turn never accumulates and the knob appears completely dead.

---

## 7. Display: ST77916 over QSPI

`esp_lcd` in IDF 4.4.7 has **no** `quad_mode` flag, only `octal_mode`, so its SPI
panel IO cannot drive this panel. `driver/spi_master.h` does have
`SPI_TRANS_MODE_QIO`, so drive it by hand.

The framing is the part that bites, and getting it wrong gives a **black screen
with no error at all**:

- **Command:** a four-byte packet `{0x02, 0x00, cmd, 0x00}` on ONE data line,
  then any parameters, also single-line.
- **Pixels:** prefix `{0x32, 0x00, 0x2C, 0x00}`, then data with
  `SPI_TRANS_MODE_QIO` on FOUR lines.
- CS must be driven by hand (`spics_io_num = -1`) so it can span a whole frame.
- 70 MHz works. The init sequence is 193 commands; the `0x11` sleep-out needs
  its 120 ms delay.
- The panel wants **big-endian** pixels.

Measured: **full-frame push 8.22 ms, 121.6 fps sustained** — about 86% of the
70 MHz x 4-lane ceiling, so the bus is not the bottleneck.

### Consequences for the renderer

Because internal SRAM is fast and PSRAM writes are capped, the winning shape is
**no framebuffer at all**: composite 20-row bands in internal SRAM and DMA each
straight to the panel. The 33 MB/s PSRAM write cost is then never paid.

Band height must divide 360 and also divide whatever a bloom/accumulator buffer
needs, or something straddles a boundary.

---

## 8. You do not need an SD card

The slot is SDMMC 4-bit and works, but nothing needs it. Album artwork streams
over plain HTTP into PSRAM and decodes with the **ROM JPEG decoder**
(`esp_rom_tjpgd.h`, zero flash cost): measured **302 ms end to end for a 300x300
cover**.

An SD card would only buy a cache surviving reboots, and it brings back a whole
class of confusing failures — a card that stops responding still reports as
mounted, and exFAT or >16 GB cards silently do not mount.

---

## 9. Networking

- 2.4 GHz only. A 5 GHz-only network is the most common "stuck connecting".
- **Fetch artwork over `http://`, not `https://`.** The CDN serves identical
  bytes, and it means a cover never needs a second TLS session — which matters
  because ~34 KB contiguous per session is most of the internal SRAM available.
- Trim responses server-side with Spotify's `fields=` parameter. Full playlist
  objects carry images, owners and follower counts; three fields turns tens of
  kilobytes into hundreds of bytes per item.
- Guard every body read on the status code. A 204 or 304 has no
  `Content-Length`, `getSize()` returns -1, and reading a body on -1 with
  keep-alive on **hangs the task forever**.
- Bound every streamed download with both a stall deadline and a total deadline.
  Without them a CDN that accepts the connection and goes quiet spins the task
  at `delay(1)` — nothing looks crashed, it simply never completes again.

---

## 10. Traps, in the order they cost the most time

1. **Concurrent serial writes shred the log.** Two tasks each doing several
   `printf`s per message interleave character by character:
   `[net]kno b pnins: Ae=1 B=1t`. That is worse than it looks, because a
   corrupted log does not read as corrupted — `grep` finds nothing and the
   obvious conclusion is that the code never ran. **Format each line into a
   buffer and write it once, under a lock.**
2. **Opening the serial port resets the board.** Any counter accumulated across
   a user's interaction is destroyed by the act of reading it. Hold one stream
   open rather than reopening, and stream it line by line — a capture that
   buffers until it finishes is unreadable exactly when you need it.
3. **A diagnostic can be the bug.** `pinMode` on the encoder pins, left in place
   to prove they moved, took them back from PCNT and paused the counter — so the
   counter under investigation was never actually running.
4. **Clip by discarding, never by clamping.** Clamping an off-screen sprite's
   coordinate range folds it onto the boundary: particles smeared onto column 0,
   and a triangle that missed a band got one spurious edge row. Three separate
   bugs, one mistake.
5. **PSRAM reports zero if `memory_type` is wrong.** Reads exactly like dead
   hardware.
6. **Fonts must cover Latin-1.** The default 0x20–0x7E range renders accented
   artist names as tofu boxes, and a real music library serves one within the
   hour.

---

## 11. Checklist for a new project on this board

1. `platformio.ini` from §3, including `qio_opi` and the `-Os` unflag.
2. Boot banner: reset reason, crash streak, PSRAM total **and largest block**,
   internal heap free and largest block. If PSRAM reads zero, stop.
3. One locked, single-write logger before anything else has two tasks.
4. Every buffer over a few kilobytes in PSRAM, explicitly, with no fallback.
5. Band renderer in internal SRAM, DMA straight to the panel. No framebuffer.
6. PCNT for the encoder, single edge per line, and keep the remainder.
7. Artwork over plain HTTP, decoded by the ROM decoder into PSRAM.
8. Measure before optimising, and measure again after. Every guess made while
   writing this project was wrong at least once.
