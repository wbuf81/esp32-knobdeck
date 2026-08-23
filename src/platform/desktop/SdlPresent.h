#pragma once

// The desktop's panel: an SDL window that shows the framebuffer.
//
// Deliberately NOT LovyanGFX's Panel_sdl. The ancestor project used it and
// documented an unsynchronised startup race upstream that segfaulted roughly
// one run in eighty. Since this project owns its framebuffer outright, showing
// it is a texture upload and needs none of that machinery.

#include <cstdint>

#include "gfx/Framebuffer.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace desktop {

class SdlPresent {
 public:
  // scale is an integer window magnification; the texture stays 360x360.
  bool begin(const char *title, int scale);
  void present(const gfx::Framebuffer &fb);

  // Returns false when the user has asked to quit.
  bool pumpEvents();

  void end();

 private:
  SDL_Window *win_ = nullptr;
  SDL_Renderer *ren_ = nullptr;
  SDL_Texture *tex_ = nullptr;
  bool running_ = true;
};

}  // namespace desktop
