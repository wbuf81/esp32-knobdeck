#include "SdlPresent.h"

#include <SDL.h>

namespace desktop {

bool SdlPresent::begin(const char *title, int scale) {
  if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
  if (scale < 1) scale = 1;

  win_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED, gfx::W * scale,
                          gfx::H * scale, SDL_WINDOW_SHOWN);
  if (!win_) return false;

  ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED);
  if (!ren_) return false;

  // RGB565 straight through: no per-pixel conversion on the host, and the
  // bytes we hand SDL are the bytes the device's DMA will push (modulo the
  // swap the panel bus wants, which is the device's business alone).
  tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGB565,
                           SDL_TEXTUREACCESS_STREAMING, gfx::W, gfx::H);
  return tex_ != nullptr;
}

void SdlPresent::present(const gfx::Framebuffer &fb) {
  SDL_UpdateTexture(tex_, nullptr, fb.pixels(), gfx::W * 2);
  SDL_RenderClear(ren_);
  SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
  SDL_RenderPresent(ren_);
}

bool SdlPresent::pumpEvents() {
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (e.type == SDL_QUIT) running_ = false;
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_q) running_ = false;
  }
  return running_;
}

void SdlPresent::end() {
  if (tex_) SDL_DestroyTexture(tex_);
  if (ren_) SDL_DestroyRenderer(ren_);
  if (win_) SDL_DestroyWindow(win_);
  tex_ = nullptr;
  ren_ = nullptr;
  win_ = nullptr;
  SDL_Quit();
}

}  // namespace desktop
