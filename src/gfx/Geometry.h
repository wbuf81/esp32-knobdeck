#pragma once

// The one place the screen's shape is stated.
//
// The panel is a 360x360 pixel grid behind a round window: only the disc of
// RADIUS centred on (CX, CY) is physically visible. Nothing informative may be
// placed outside it, and CircleMask blackens it at the end of every frame so a
// bug there shows up as a missing corner rather than as invisible garbage.

#include <cstdint>

namespace gfx {

constexpr int W = 360;
constexpr int H = 360;
constexpr int CX = 180;
constexpr int CY = 180;
constexpr int RADIUS = 180;

// Squared radius, so hot loops compare against this without a sqrt.
constexpr int RADIUS_SQ = RADIUS * RADIUS;

}  // namespace gfx
