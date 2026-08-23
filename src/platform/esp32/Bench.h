#pragma once

// Memory bandwidth benchmark.
//
// This exists to answer one question the whole render design rests on: can the
// compositing passes run against a framebuffer in PSRAM, or do the hot passes
// have to move into internal-SRAM bands with ping-pong DMA?
//
// The estimates in the design spec are replaced by whatever this prints.

namespace esp32 {

void runMemoryBenchmark();

}  // namespace esp32
