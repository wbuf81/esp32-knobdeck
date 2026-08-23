// Desktop entry point.
//
// Runs the identical view source as the device. The device composites 40-row
// bands and DMAs each to the panel; a host has no reason to band, so it renders
// one full-frame Surface. That is the whole difference, and it lives here rather
// than in any effect.
//
// Environment hooks exist so every state is reachable headlessly, which is what
// the visual tests are built on:
//
//   KNOB_HEADLESS=1     no window; render, dump, exit
//   KNOB_EXIT_MS=<ms>   quit after this much SIMULATED time
//   KNOB_DUMP=<path>    write the final framebuffer as a 24-bit BMP
//   KNOB_SEED=<n>       track seed, which sets hue and tempo
//   KNOB_BLOOM=<0-255>  bloom strength override
//   KNOB_PARTICLES=0    disable the particle layer
//   KNOB_BANDS=1        composite in 40-row bands, as the device does, so the
//                       band path itself can be regression-tested on the host
//   KNOB_SCREEN=list    preview the playlist browser instead of now-playing
//   KNOB_POS=<f>        scroll position within that list, fractional
//   KNOB_WAV=<path>     drive the analyser from a WAV instead of the procedural
//                       fallback, standing in for the device's microphone
//   KNOB_SCALE=<n>      window magnification (default 2)

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/Modulation.h"
#include "audio/AudioAnalyzer.h"
#include "audio/Procedural.h"
#include "core/FrameClock.h"
#include "core/Hash.h"
#include "core/Rng.h"
#include "gfx/Framebuffer.h"
#include "gfx/Surface.h"
#include "platform/desktop/FrameDump.h"
#include "platform/desktop/SdlPresent.h"
#include "platform/desktop/WavMic.h"
#include "art/Image.h"
#include "core/PlaybackState.h"
#include "shell/ListView.h"
#include "shell/NowPlaying.h"
#include "shell/RadialShell.h"
#include "views/CoverLight.h"

namespace {

int envInt(const char *k, int fallback) {
  const char *v = std::getenv(k);
  return v ? std::atoi(v) : fallback;
}

bool envFlag(const char *k) {
  const char *v = std::getenv(k);
  return v && std::strcmp(v, "0") != 0;
}

}  // namespace

int main(int, char **) {
  const bool headless = envFlag("KNOB_HEADLESS");
  const int exit_ms = envInt("KNOB_EXIT_MS", 0);
  const char *dump = std::getenv("KNOB_DUMP");
  const uint32_t seed = static_cast<uint32_t>(envInt("KNOB_SEED", 0)) != 0
                            ? static_cast<uint32_t>(envInt("KNOB_SEED", 0))
                            : fnv1a("first-light");
  const bool use_bands = envFlag("KNOB_BANDS");
  const int band_h = 40;

  gfx::Framebuffer fb;
  views::CoverLight view;
  shell::RadialShell shell;
  shell::NowPlaying nowplaying;
  shell::ListView listview;

  // Stand-in library, so the browser layout can be judged without a network.
  // Deliberately includes a name far too long for the disc and one with accents:
  // those are the two cases the fitting and the font range exist for.
  static const char *const kFakeItems[] = {
      "Discover Weekly",
      "Road Trip",
      "Bj\xc3\xb6rk & Sigur R\xc3\xb3s Mix",
      "Liked Songs",
      "Deep Focus For Very Long Working Afternoons",
      "Workout",
      "Late Night Jazz",
      "90s Rock Anthems",
  };
  const int kFakeCount = 8;
  const char *screen_env = std::getenv("KNOB_SCREEN");
  const bool list_mode = screen_env && std::strcmp(screen_env, "list") == 0;
  view.setAmbient(list_mode);

  // A stand-in track so the text layout can be judged without a network. The
  // accented name is deliberate: it is the case the Latin-1 font range exists
  // for, and it should look right rather than like broken metadata.
  PlaybackState fake;
  fake.has_track = true;
  fake.is_playing = true;
  setStr(fake.title, TEXT_LEN, "East Side of Sorrow - Live From Nashville");
  setStr(fake.artist, TEXT_LEN, "Bj\xc3\xb6rk & Sigur R\xc3\xb3s");
  fake.duration_ms = 264148;
  fake.volume_pct = 62;
  audio::Modulation mod;
  core::Rng rng(0xC0FFEE);

  // The analyser owns the procedural fallback, so the desktop path is the same
  // one the device runs: mic if there is something to hear, procedural if not.
  desktop::WavMic mic;
  const char *wav = std::getenv("KNOB_WAV");
  const bool have_wav = wav && mic.open(wav);
  if (wav && !have_wav) std::fprintf(stderr, "could not open %s\n", wav);
  audio::AudioAnalyzer analyzer;
  analyzer.begin(have_wav ? &mic : nullptr);
  analyzer.setTrack(seed);
  view.begin(seed);

  // A synthetic cover until real artwork arrives, unless asked for the
  // no-artwork path - which must look deliberate, because it is what every
  // uncached album shows for its first second.
  art::Image cover;
  if (!envFlag("KNOB_NOCOVER")) {
    art::makePlaceholderCover(seed, 192, &cover);
    view.setCover(&cover);
  }
  if (std::getenv("KNOB_BLOOM"))
    view.setBloomStrength(static_cast<uint8_t>(envInt("KNOB_BLOOM", 150)));
  if (std::getenv("KNOB_PARTICLES") && !envFlag("KNOB_PARTICLES"))
    view.setParticlesEnabled(false);

  desktop::SdlPresent screen;
  if (!headless && !screen.begin("knob-spotify", envInt("KNOB_SCALE", 2)))
    return 1;

  // Simulated time, fixed step. A headless run must produce identical frames
  // every time regardless of how fast the machine is - that determinism is what
  // the pixel assertions rest on.
  uint32_t sim_ms = 0;
  const uint32_t step_ms = 16;
  const float dt = step_ms * 0.001f;

  for (;;) {
    if (have_wav) mic.advance(dt);
    analyzer.update(&mod, dt);
    view.update(mod, dt, rng);

    // A slow sweep so the progress ring is visible without playback state.
    mod.progress01 = static_cast<float>(sim_ms % 90000) / 90000.0f;
    const uint32_t shown =
        static_cast<uint32_t>(mod.progress01 * fake.duration_ms);
    nowplaying.prepare(fake, shown);

    // A slow drift through the list, so a still frame shows it mid-glide - which
    // is the state the fractional scroll exists to make look right.
    float list_pos = static_cast<float>(sim_ms) / 1400.0f;
    if (const char *p = std::getenv("KNOB_POS")) list_pos = std::atof(p);
    while (list_pos > kFakeCount - 1) list_pos -= kFakeCount - 1;
    if (list_mode)
      listview.prepare(kFakeItems, kFakeCount, list_pos, "PLAYLISTS", false);

    if (use_bands) {
      for (int y = 0; y < gfx::H; y += band_h) {
        gfx::Surface s;
        s.px = fb.pixels() + static_cast<size_t>(y) * gfx::W;
        s.w = gfx::W;
        s.h = band_h;
        s.y0 = y;
        view.renderBand(s);
        shell.render(s, mod.progress01, view.tint(), 62, sim_ms, mod.bass);
        if (list_mode) listview.render(s, view.tint());
        else nowplaying.render(s, view.tint());
      }
    } else {
      gfx::Surface s;
      s.px = fb.pixels();
      s.w = gfx::W;
      s.h = gfx::H;
      s.y0 = 0;
      view.renderBand(s);
      shell.render(s, mod.progress01, view.tint(), 62, sim_ms, mod.bass);
      if (list_mode) listview.render(s, view.tint());
      else nowplaying.render(s, view.tint());
    }
    view.endFrame();

    if (!headless) {
      if (!screen.pumpEvents()) break;
      screen.present(fb);
      SDL_Delay(step_ms);
    }

    sim_ms += step_ms;
    if (exit_ms > 0 && sim_ms >= static_cast<uint32_t>(exit_ms)) break;
    if (headless && exit_ms == 0) break;
  }

  if (dump) {
    if (!desktop::dumpFrameBmp(fb, dump)) {
      std::fprintf(stderr, "failed to write %s\n", dump);
      return 2;
    }
  }
  if (!headless) screen.end();
  return 0;
}
