#pragma once
#include "Framebuffer.h"
namespace gfx {
// 4x4 ordered dither.
//
// The panel is 262K colour (18-bit) and the framebuffer is RGB565, so smooth
// gradients - which this project is full of - band visibly. Dithering trades
// that for imperceptible noise. Applied after bloom and before the mask.
void ditherFrame(Framebuffer &fb);
}  // namespace gfx
