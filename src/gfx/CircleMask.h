#pragma once
#include "Framebuffer.h"
namespace gfx {
// Blackens everything outside the visible disc. Runs last, so any effect that
// overdraws the corners fails as a missing edge rather than as garbage behind
// the bezel.
void maskToCircle(Framebuffer &fb);
}  // namespace gfx
