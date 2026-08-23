#pragma once

// The three faces this project uses. Size, not weight, carries the hierarchy:
// these have no bold variant beyond the title face.

#include "JBMonoArtist.h"  // JetBrainsMonoNerdFont_Regular7pt8b
#include "JBMonoSmall.h"   // JetBrainsMonoNerdFont_Regular6pt8b
#include "JBMonoTitle.h"   // JetBrainsMonoNerdFont_Bold8pt8b

namespace gfx {
inline const GFXfont &fontTitle() { return JetBrainsMonoNerdFont_Bold8pt8b; }
inline const GFXfont &fontArtist() { return JetBrainsMonoNerdFont_Regular7pt8b; }
inline const GFXfont &fontSmall() { return JetBrainsMonoNerdFont_Regular6pt8b; }
}  // namespace gfx
