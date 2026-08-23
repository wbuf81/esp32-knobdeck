#pragma once

// Framebuffer -> 24-bit BMP.
//
// Screen-capturing the SDL window means hunting for its position and fighting
// Retina scaling. Writing the buffer out gives an exact 360x360 image, which is
// what makes visual regression checks possible at all.
//
// Takes a Framebuffer rather than reading the panel back: it therefore has no
// SDL dependency and runs inside the unit tests.

#include "gfx/Framebuffer.h"

namespace desktop {

bool dumpFrameBmp(const gfx::Framebuffer &fb, const char *path);

}  // namespace desktop
