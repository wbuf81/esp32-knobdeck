// Device entry point: Waveshare ESP32-S3-Knob-Touch-LCD-1.8.
//
// Bring-up order is deliberate. Nothing touches the panel until the boot banner
// has confirmed PSRAM is actually present and contiguous, because every later
// stage assumes it and a wrong memory_type setting reports zero on perfectly
// good hardware.
//
// The renderer is a band renderer with no framebuffer anywhere. Measured on this
// board, PSRAM writes cap at 33 MB/s regardless of access width, so any
// full-frame pass over a PSRAM framebuffer costs at least 7.5 ms and the
// pipeline wanted six of them. Compositing 40-row bands in internal SRAM and
// DMA-ing each straight to the panel never pays that cost at all.
//
// Threading follows the ancestor project, which got it right: the network task
// owns core 0 and may block for seconds; rendering owns core 1 and must not.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cmath>
#include <cstring>

#include "art/Image.h"
#include "audio/AudioAnalyzer.h"
#include "audio/Modulation.h"
#include "config/DeviceConfig.h"
#include "core/AppState.h"
#include "core/FrameClock.h"
#include "core/Backlight.h"
#include "core/Hash.h"
#include "core/Log.h"
#include "core/ProgressClock.h"
#include "core/Rng.h"
#include "gfx/Geometry.h"
#include "gfx/Surface.h"
#include "input/Gesture.h"
#include "net/HostLink.h"
#include "net/NetWorker.h"
#include "spotify/Library.h"
#include "platform/esp32/Bench.h"
#include "platform/esp32/Boot.h"
#include "platform/esp32/InputHw.h"
#include "platform/esp32/Panel.h"
#include "platform/esp32/Pins.h"
#include "shell/ConfirmRing.h"
#include "shell/GestureFlash.h"
#include "shell/ListView.h"
#include "shell/NowPlaying.h"
#include "shell/RadialShell.h"
#include "views/CoverLight.h"

namespace {

views::CoverLight g_view;
art::Image g_cover;                       // synthetic fallback
const art::Image *g_shown_cover = nullptr;  // the live cover currently shown
audio::AudioAnalyzer g_analyzer;
audio::Modulation g_mod;
core::FrameClock g_clock;
core::Rng g_rng(0xC0FFEE);
ProgressClock g_progress;
shell::RadialShell g_shell;
shell::NowPlaying g_nowplaying;
shell::ListView g_list;
shell::ConfirmRing g_confirm;
shell::GestureFlash g_flash;
spotify::Library g_library;

// Which screen is up.
//
// Deliberately a plain enum and a couple of ints rather than a screen stack:
// there are three states and one way between each pair, and a stack would be
// machinery for navigation this device does not have.
// Confirm is a leaf off Tracks and nothing else: you can only arrive from a tap
// on a queue row, and both answers leave immediately. Still flat, still one way
// between each pair.
enum class Screen : uint8_t { Player, Playlists, Tracks, Confirm };
Screen g_screen = Screen::Player;

int g_sel = 0;            // the selected row
float g_sel_pos = 0.0f;   // eased position, so the wheel glides to it
uint32_t g_lib_gen = 0;   // last library generation the UI laid out
char g_open_uri[52] = {};
char g_open_name[52] = {};

// The pending jump. The row is remembered rather than the name, because the
// queue can be republished underneath the confirmation and the row is what the
// command needs anyway.
int g_confirm_row = 0;
int g_confirm_yes = 0;        // 0 = cancel, 1 = play
float g_confirm_pos = 0.0f;   // eased, so the marker glides like the list does

// Item pointers handed to the list. Rebuilt each frame, which is 32 pointer
// stores - far cheaper than any way of keeping it in sync.
const char *g_items[spotify::Library::MAX_TRACKS];

int listCount() {
  if (g_screen == Screen::Playlists) return g_library.playlistCount();
  if (g_screen == Screen::Tracks) return g_library.trackCount();
  return 0;
}
input::GestureRecognizer g_gesture;

// Volume is tracked locally so a knob turn responds on the frame it happens,
// and the value is pushed to Spotify coalesced. This is the ancestor's
// optimistic-UI rule: the settle window in AppState stops an in-flight poll
// from snapping the number back to what it was before the turn.
int g_volume = -1;

// Idle return from the browser. Long enough to read a list, short enough that
// walking away leaves the device showing what is playing.
constexpr uint32_t IDLE_RETURN_MS = 15000;
uint32_t g_last_input_ms = 0;
core::Backlight g_backlight;

// Set by the host companion when the Mac sleeps or locks. Always false until
// that exists; wired now so the backlight rule has one place to read it.
net::HostLink g_hostlink;

// The mDNS name the host companion posts to. Kept here rather than in a config
// file because it is the device's identity, not a preference.
constexpr const char *MDNS_NAME = "knobspotify";

// Cumulative knob totals, reported in the periodic status line.
//
// Kept as running totals rather than logged per event so diagnosing the knob
// does not require hitting a capture window: turn it whenever, read the numbers
// afterwards. Coordinating a serial capture with a human turning a dial cost
// three inconclusive rounds before this.
long g_knob_edges = 0;    // pin transitions seen by the burst poll
long g_knob_detents = 0;  // net detents PCNT reported

// Held for the life of the program: NetWorker keeps const char* into these.
DeviceConfig g_cfg;
NetWorker *g_net = nullptr;

bool g_panel_ok = false;
char g_track[ID_LEN] = {};

uint32_t g_frames = 0;
uint64_t g_total_us = 0;
uint64_t g_shell_us = 0;  // rings
uint64_t g_text_us = 0;   // title, artist, timecodes

const char *linkName(LinkStatus s) {
  switch (s) {
    case LinkStatus::Booting: return "booting";
    case LinkStatus::Connecting: return "connecting";
    case LinkStatus::Online: return "online";
    case LinkStatus::Offline: return "offline";
    case LinkStatus::AuthError: return "auth-error";
    case LinkStatus::ReauthNeeded: return "reauth-needed";
  }
  return "?";
}

}  // namespace

void setup() {
  Serial.begin(115200);
  // USB-CDC needs a moment before the host is listening, and a banner nobody
  // sees is worse than none: it looks like a board that did not boot.
  delay(1500);
  esp32::printBootBanner();

  g_panel_ok = esp32::panelBegin();
  if (!g_panel_ok) {
    LOGF("panel: FAILED. Check Pins.h.");
  } else {
    esp32::panelBacklight(210);
  }

  // Input bring-up. The I2C scan runs first and unconditionally: the touch and
  // encoder pins are community-sourced, and knowing what answers on the bus is
  // the difference between "wrong pin" and "dead chip".
  esp32::scanI2c();
  LOGF("touch:   %s (chip id 0x%02X)",
                esp32::touchBegin() ? "ok" : "NOT RESPONDING",
                esp32::touchChipId());
  LOGF("encoder: %s (a=%d b=%d)",
       esp32::encoderBegin() ? "pcnt configured" : "FAILED", pins::ENC_A,
       pins::ENC_B);
  // NOT calling encoderPlainInputMode() here. It pauses the counter and takes
  // the pins back with pinMode, so leaving it in place meant PCNT was never
  // actually under test - the diagnostic was the reason the counter read zero.
  //
  // The knob's press is NOT readable from this MCU. GPIO0 was the only
  // candidate - the schematic puts a 10K pull-up on it beside the encoder's -
  // and it never moves when the knob is pressed. So selection is a centre tap,
  // which the touch panel already gives us.
  LOGF("haptics: %s", esp32::hapticsBegin() ? "ok" : "NOT RESPONDING");

  const uint32_t seed = fnv1a("first-light");
  g_view.begin(seed);
  g_analyzer.begin(nullptr);  // no microphone yet: I2S pins are unconfirmed
  g_analyzer.setTrack(seed);

  art::makePlaceholderCover(seed, 192, &g_cover);
  if (g_cover.valid()) g_view.setCover(&g_cover);

  // Credentials resolve NVS-first, compiled secrets as the per-field fallback -
  // so a developer board keeps working with no setup step while a gifted one is
  // configured entirely through the portal.
  g_cfg = DeviceConfig::load();
  LOGF("config: wifi=%s spotify=%s",
                g_cfg.wifi_ssid.empty() ? "MISSING" : "set",
                g_cfg.refresh_token.empty() ? "MISSING" : "set");

  if (g_cfg.complete()) {
    static NetWorker net(g_cfg.client_id.c_str(), g_cfg.client_secret.c_str(),
                         g_cfg.refresh_token.c_str());
    g_net = &net;
    // No SD card wired up yet, so artwork will report unavailable rather than
    // silently failing - which is the distinction the ancestor's notes insist on.
    LOGF("library: %s", g_library.begin() ? "lists in PSRAM" : "ALLOC FAILED");
    g_hostlink.configure(MDNS_NAME);
    net.setLibrary(&g_library);
    g_net->start("/sd/art", g_cfg.wifi_ssid.c_str(), g_cfg.wifi_password.c_str());
    LOGF("net: worker started on core 0");
  } else {
    LOGF("net: config incomplete; running visuals only");
  }
}

void loop() {
  const uint64_t t_frame = esp_timer_get_time();
  const float dt = g_clock.tick(millis());
  const uint32_t now = millis();

  AppState st;
  if (g_net) st = g_net->snapshot();

  // Progress is extrapolated locally between polls and resynced ONLY when the
  // publish sequence changes. Copying the published position every frame is the
  // bug this class exists for: it overwrites the extrapolation, so the clock
  // moves in 2 s jumps instead of ticking.
  g_progress.sync(st.publish_seq, st.pb.progress_ms);
  g_progress.advance(static_cast<uint32_t>(dt * 1000.0f), st.pb.is_playing,
                     st.pb.duration_ms);
  const uint32_t shown_progress = g_progress.value();

  // Real artwork, once the net task has decoded it. The cover is borrowed from
  // the net task's PSRAM store, so re-checking every frame is how the view picks
  // it up the moment it lands rather than only on the next track change.
  if (g_net && st.pb.art_path[0]) {
    const art::Image *live = g_net->cover(st.pb.art_path);
    if (live && live != g_shown_cover) {
      g_shown_cover = live;
      g_view.setCover(live);
      LOGF("cover: %dx%d live artwork", live->width(),
                    live->height());
    }
  }

  // A new track reseeds everything derived from it: hue, tempo, particle
  // palette. Deterministic on the id, so a song always looks the same way.
  if (std::strncmp(g_track, st.pb.track_id, ID_LEN) != 0) {
    std::strncpy(g_track, st.pb.track_id, ID_LEN - 1);
    const uint32_t seed = st.pb.track_id[0] ? fnv1a(st.pb.track_id)
                                            : fnv1a("first-light");
    g_view.begin(seed);
    g_analyzer.setTrack(seed);
    // Fall back to the synthetic cover until the real one arrives. The ancestor
    // notes why this matters: "no artwork" text during the second every uncached
    // album spends downloading made a working device look broken.
    g_shown_cover = nullptr;
    if (g_cover.valid()) g_view.setCover(&g_cover);
    LOGF("track: %s - %s",
                  st.pb.artist[0] ? st.pb.artist : "(none)",
                  st.pb.title[0] ? st.pb.title : "(none)");
  }

  // --- input ---
  //
  // Whether the screen was off BEFORE this frame's input, so the input that
  // wakes it can be swallowed the way a phone does - otherwise the tap that
  // wakes the device also pauses the music.
  const bool was_off = g_backlight.state() == core::ScreenState::Off;
  //
  // No pin-level polling here. The burst that proved the encoder pins move -
  // 600 samples at 40us - cost 24 ms of every frame, and PCNT counts in
  // hardware.
  const int detents = esp32::encoderDelta();
  if (detents != 0 && !was_off) {
    esp32::hapticsClick();
    if (g_screen == Screen::Player) {
      if (g_volume < 0) g_volume = st.pb.volume_pct >= 0 ? st.pb.volume_pct : 50;
      g_volume += detents * 2;
      if (g_volume < 0) g_volume = 0;
      if (g_volume > 100) g_volume = 100;
      g_shell.showVolume(g_volume, now);
      if (g_net) {
        // Coalesced: a fast spin sends the final value once rather than forty
        // times, which keeps both the rate limit and the knob happy.
        Command c;
        c.type = CommandType::SetVolume;
        c.arg = g_volume;
        g_net->submit(c);
        g_net->mutate([](AppState &a) { a.settle_volume.arm(millis(), 1200); });
      }
      LOGF("knob: %+d -> volume %d%%", detents, g_volume);
    } else if (g_screen == Screen::Confirm) {
      // Two positions, so any turn in a direction lands on that answer rather
      // than accumulating a count nobody can see.
      g_confirm_yes = detents > 0 ? 1 : 0;
    } else {
      // Clamped rather than wrapped. On a list you cannot see the ends of,
      // wrapping from the last item to the first feels like a glitch.
      const int n = listCount();
      g_sel += detents;
      if (g_sel < 0) g_sel = 0;
      if (g_sel > n - 1) g_sel = n > 0 ? n - 1 : 0;
    }
  } else if (g_screen == Screen::Player && st.pb.volume_pct >= 0 &&
             !g_shell.volumeVisible(now)) {
    g_volume = st.pb.volume_pct;  // resync once the local edit has settled
  }

  int tx = 0, ty = 0;
  const bool touching = esp32::touchRead(&tx, &ty);
  const input::Gesture g = g_gesture.update(touching, tx, ty, now);
  if (g != input::Gesture::None || detents != 0) g_last_input_ms = now;

  g_hostlink.poll(now, st.link == LinkStatus::Online);
  const bool host_asleep = g_hostlink.hostAsleep(now);
  {
    // Logged on change only, so the reason the screen went dark is in the log
    // without the log being mostly this.
    static int last = -1;
    const int nowv = host_asleep ? 1 : 0;
    if (nowv != last) {
      last = nowv;
      LOGF("host: %s (heartbeat %s)", host_asleep ? "asleep/locked" : "awake",
           g_hostlink.everHeard() ? "seen" : "never seen");
    }
  }
  g_backlight.update(now, st.pb.is_playing, g_last_input_ms, host_asleep);
  esp32::panelBacklight(g_backlight.duty());

  // Drift back to the player after a spell of nothing.
  //
  // Being stranded in a list is the one state this device cannot explain: there
  // is no visible way back except a gesture you have to remember. It also means
  // the visuals return on their own, which is what the thing is for.
  if (g_screen != Screen::Player && now - g_last_input_ms > IDLE_RETURN_MS) {
    g_screen = Screen::Player;
    g_sel = 0;
    g_sel_pos = 0.0f;
  }

  if (was_off && (g != input::Gesture::None || detents != 0)) {
    // Woken, and that is all this input does.
    LOGF("woke (input swallowed)");
  } else if (g != input::Gesture::None) {
    LOGF("touch: %s at (%d,%d)", input::gestureName(g), tx, ty);
    Command c;
    bool send = false;

    switch (g_screen) {
      case Screen::Player:
        switch (g) {
          case input::Gesture::Tap:
            // Flip is_playing LOCALLY first, then send.
            //
            // Not merely for a snappy UI: runCommand chooses between /play and
            // /pause by reading this field, on the assumption the UI has already
            // flipped it. Without the flip it sent /play while already playing,
            // and Spotify answered 403 "restriction violated" - so tapping did
            // nothing at all and the log said only 403.
            //
            // The settle window is what stops a poll already in flight from
            // snapping the state back before Spotify has caught up.
            if (g_net) {
              g_net->mutate([](AppState &a) {
                a.pb.is_playing = !a.pb.is_playing;
                a.settle_playing.arm(millis(), 1500);
              });
            }
            c.type = CommandType::PlayPause;
            send = true;
            // The glyph shows the state you are NOW IN, not the button you
            // pressed - the same convention as every transport control.
            g_flash.show(st.pb.is_playing ? shell::Glyph::Pause
                                          : shell::Glyph::Play,
                         now);
            esp32::hapticsClick();
            break;
          case input::Gesture::SwipeLeft:
            c.type = CommandType::Previous;
            send = true;
            g_flash.show(shell::Glyph::Previous, now);
            esp32::hapticsBump();
            break;
          case input::Gesture::SwipeRight:
            c.type = CommandType::Next;
            send = true;
            g_flash.show(shell::Glyph::Next, now);
            esp32::hapticsBump();
            break;
          case input::Gesture::LongPress:
            // Refused, and SAYS SO.
            //
            // This used to queue the command anyway, where runCommand dropped it
            // for having no track id. Two long-presses with nothing playing
            // therefore looked exactly like a device that had stopped listening.
            // A refusal the user cannot see is the same as a bug.
            if (!st.pb.has_track) {
              g_flash.show(shell::Glyph::HeartSlash, now);
              if (g_net)
                g_net->mutate([now](AppState &a) {
                  a.showToast("Nothing playing", now);
                });
              esp32::hapticsBump();
              break;
            }
            // Same reason: runCommand picks PUT or DELETE from `liked`, so the
            // local flip has to happen before the command is queued.
            g_flash.show(st.pb.liked ? shell::Glyph::HeartOutline
                                     : shell::Glyph::HeartFilled,
                         now);
            if (g_net) {
              g_net->mutate([](AppState &a) {
                a.pb.liked = !a.pb.liked;
                a.pb.liked_known = true;
                a.settle_liked.arm(millis(), 2000);
              });
            }
            c.type = CommandType::ToggleLike;
            send = true;
            esp32::hapticsBump();
            break;
          case input::Gesture::SwipeDown:
            // Straight to what is coming up, without going through the chooser.
            g_screen = Screen::Tracks;
            g_sel = 0;
            g_sel_pos = 0.0f;
            c.type = CommandType::FetchQueue;
            send = true;
            g_flash.show(shell::Glyph::ChevronDown, now);
            esp32::hapticsBump();
            break;
          case input::Gesture::SwipeUp:
            // Open the browser and ask for the listing. The list appears
            // immediately and empty rather than after the round trip, so the
            // gesture feels answered.
            g_screen = Screen::Playlists;
            g_sel = 0;
            g_sel_pos = 0.0f;
            c.type = CommandType::FetchPlaylists;
            send = true;
            g_flash.show(shell::Glyph::ChevronUp, now);
            esp32::hapticsBump();
            break;
          default:
            break;
        }
        break;

      case Screen::Playlists:
        if (g == input::Gesture::Tap) {
          const spotify::Entry *e = g_library.playlist(g_sel);
          if (e) {
            // Tapping a playlist PLAYS it, and then shows what is coming up.
            //
            // Not the drill-down this was first built as, because Spotify
            // refuses this app access to /playlists/{id}/tracks - a bare 403 on
            // every playlist, including the user's own. Starting a context and
            // reading the queue both work, so the flow is built out of what the
            // API actually permits rather than out of what would be tidier.
            std::strncpy(g_open_uri, e->uri, sizeof(g_open_uri) - 1);
            std::strncpy(g_open_name, e->name, sizeof(g_open_name) - 1);
            c.type = CommandType::PlayFromContext;
            std::strncpy(c.uri, e->uri, sizeof(c.uri) - 1);
            c.arg = 0;
            send = true;
            esp32::hapticsBump();
            // Straight back to the player. You asked for music, so the answer is
            // the music: a new cover, a new title and a reset ring. Landing in
            // another list instead made a one-gesture request feel like a detour.
            g_screen = Screen::Player;
            g_sel = 0;
            g_sel_pos = 0.0f;
          }
        } else if (g == input::Gesture::SwipeDown) {
          g_screen = Screen::Player;
          esp32::hapticsBump();
        }
        break;

      case Screen::Tracks:
        // Tap asks before it acts.
        //
        // A jump is destructive in a way the other gestures are not - it throws
        // away everything between here and there - and the rows are 33px apart
        // on a surface you are also using to hold the device. So the tap opens a
        // confirmation rather than firing.
        //
        // Row 0 is the track already playing, so it is not a jump and does
        // nothing, rather than asking a question with no meaningful answer.
        if (g == input::Gesture::Tap && g_sel > 0) {
          const spotify::Entry *e = g_library.track(g_sel);
          if (e && e->uri[0]) {
            g_confirm_row = g_sel;
            g_confirm_yes = 0;  // opens on cancel: a mis-tap costs nothing
            g_confirm_pos = 0.0f;
            g_screen = Screen::Confirm;
            esp32::hapticsBump();
          }
        } else if (g == input::Gesture::SwipeDown) {
          // Straight to the player, not back to the chooser. Both browser
          // screens are siblings reached from the player - up for playlists,
          // down for the queue - so one swipe always gets you home.
          g_screen = Screen::Player;
          g_sel = 0;
          g_sel_pos = 0.0f;
          esp32::hapticsBump();
        }
        break;

      case Screen::Confirm:
        if (g == input::Gesture::Tap) {
          const spotify::Entry *e = g_library.track(g_confirm_row);
          if (g_confirm_yes && e && e->uri[0]) {
            c.type = CommandType::PlayQueueItem;
            std::strncpy(c.uri, e->uri, sizeof(c.uri) - 1);
            // Row 0 is the track playing, so the row index IS the number of
            // skips the fallback needs.
            c.arg = g_confirm_row;
            send = true;
            esp32::hapticsClick();
            // Straight to the player, same as picking a playlist: you asked for
            // a song, the answer is the song.
            g_screen = Screen::Player;
            g_sel = 0;
            g_sel_pos = 0.0f;
          } else {
            // Cancelled. Back to the queue with the row still under the
            // selection, so a mis-tap does not also lose your place.
            g_screen = Screen::Tracks;
            esp32::hapticsBump();
          }
        } else if (g == input::Gesture::SwipeDown) {
          // Down still means back, everywhere in the browser.
          g_screen = Screen::Tracks;
          esp32::hapticsBump();
        }
        break;
    }
    if (send && g_net) g_net->submit(c);
  }

  // The marker eases to the answer for the same reason the list does: a snap
  // between two ends reads as a redraw, a glide reads as something you moved.
  {
    const float target = static_cast<float>(g_confirm_yes);
    const float k = 1.0f - std::exp(-dt * 16.0f);
    g_confirm_pos += (target - g_confirm_pos) * k;
    if (std::fabs(target - g_confirm_pos) < 0.002f) g_confirm_pos = target;
  }

  // A fresh listing resets the selection: keeping row 7 selected while the list
  // underneath it changed would point at something the user never chose.
  //
  // Not on Confirm: the jump republishes the queue, and that must not yank the
  // row out from under the question being asked about it.
  if (g_screen == Screen::Playlists || g_screen == Screen::Tracks) {
    const uint32_t gen = g_library.generation();
    if (gen != g_lib_gen) {
      g_lib_gen = gen;
      g_sel = 0;
      g_sel_pos = 0.0f;
    }
    // Ease toward the selection. Frame-rate independent, and clamped so a big
    // jump still arrives rather than crawling.
    const float target = static_cast<float>(g_sel);
    const float k = 1.0f - std::exp(-dt * 14.0f);
    g_sel_pos += (target - g_sel_pos) * k;
    if (std::fabs(target - g_sel_pos) < 0.002f) g_sel_pos = target;
  }
  g_view.setAmbient(g_screen != Screen::Player);

  g_analyzer.update(&g_mod, dt);
  g_mod.progress01 = st.pb.duration_ms
                         ? static_cast<float>(shown_progress) /
                               static_cast<float>(st.pb.duration_ms)
                         : 0.0f;
  g_mod.volume01 = st.pb.volume_pct >= 0 ? st.pb.volume_pct * 0.01f : 0.7f;

  g_view.update(g_mod, dt, g_rng);

  // Measured, truncated and formatted once per frame, not once per band.
  g_flash.prepare(now);
  if (g_screen == Screen::Player) {
    g_nowplaying.prepare(st.pb, shown_progress);
  } else if (g_screen == Screen::Confirm) {
    const spotify::Entry *e = g_library.track(g_confirm_row);
    g_confirm.prepare(e ? e->name : nullptr, g_confirm_pos);
  } else {
    const bool playlists = g_screen == Screen::Playlists;
    const int n = listCount();
    for (int i = 0; i < n; ++i) {
      const spotify::Entry *e =
          playlists ? g_library.playlist(i) : g_library.track(i);
      g_items[i] = e ? e->name : nullptr;
    }
    const char *note = nullptr;
    if (!playlists && n == 0) {
      const int err = g_library.tracksError();
      // 403 and 404 on a playlist's tracks are almost always Spotify refusing
      // an app access to one of its own algorithmic or editorial playlists,
      // not a problem with this device or the token.
      if (err == 403 || err == 404) note = "Spotify blocks this one";
      else if (err) note = "could not load";
    }
    const char *heading =
        playlists ? "PLAYLISTS"
                  : (g_library.tracksOf()[0] ? g_library.tracksOf()
                                             : g_open_name);
    // In UP NEXT the queue's first entry is the track playing now.
    const int current = playlists ? -1 : (n > 0 ? 0 : -1);
    g_list.prepare(g_items, n, g_sel_pos, heading,
                   playlists ? g_library.playlistsTruncated()
                             : g_library.tracksTruncated(),
                   note, current);
  }

  // Nothing is drawn while the backlight is off. Compositing frames nobody can
  // see is pure heat, and it is most of the CPU this device uses.
  if (g_backlight.state() == core::ScreenState::Off) {
    delay(40);
  } else if (g_panel_ok) {
    esp32::panelBeginFrame();
    for (int y = 0; y < gfx::H; y += esp32::PANEL_BAND_H) {
      gfx::Surface s;
      s.px = esp32::panelNextBand();
      s.w = gfx::W;
      s.h = esp32::PANEL_BAND_H;
      s.y0 = y;
      g_view.renderBand(s);
      // The shell draws over the view, in the same band, before it is pushed.
      const uint64_t sh0 = esp_timer_get_time();
      g_shell.render(s, g_mod.progress01, g_view.tint(),
                     g_volume >= 0 ? g_volume : st.pb.volume_pct, now,
                     g_mod.bass);
      const uint64_t sh1 = esp_timer_get_time();
      if (g_screen == Screen::Player) g_nowplaying.render(s, g_view.tint());
      else if (g_screen == Screen::Confirm) g_confirm.render(s, g_view.tint());
      else g_list.render(s, g_view.tint());
      // Last, so the answer is never behind the thing it is answering about.
      g_flash.render(s, g_view.tint());
      const uint64_t sh2 = esp_timer_get_time();
      g_shell_us += sh1 - sh0;
      g_text_us += sh2 - sh1;
      esp32::panelCommitBand();
    }
    esp32::panelEndFrame();
    g_view.endFrame();
  }

  g_total_us += esp_timer_get_time() - t_frame;
  ++g_frames;

  static uint32_t last = 0;
  if (now - last >= 3000) {
    last = now;
    const float fps = g_frames * 1000000.0f / static_cast<float>(g_total_us);
    auto &vt = g_view.timing();
    LOGF(
        "fps %5.1f | link %-13s %s %s | %d%% vol | %lu/%lu ms | parts %4d "
        "| back %5.2f cover %5.2f part %5.2f accum %5.2f shell %5.2f "
        "text %5.2f | heap %lu psram %lu",
        fps, linkName(st.link), st.pb.is_playing ? "play" : "paus",
        st.pb.title[0] ? st.pb.title : "(nothing)", st.pb.volume_pct,
        (unsigned long)shown_progress, (unsigned long)st.pb.duration_ms,
        g_view.particleCount(),
        vt.frames ? vt.backdrop / 1000.0f / vt.frames * 18.0f : 0.0f,
        vt.frames ? vt.cover / 1000.0f / vt.frames * 18.0f : 0.0f,
        vt.frames ? vt.particles / 1000.0f / vt.frames * 18.0f : 0.0f,
        vt.frames ? vt.bloom / 1000.0f / vt.frames * 18.0f : 0.0f,
        g_shell_us / 1000.0f / g_frames, g_text_us / 1000.0f / g_frames,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (g_net && g_net->stalled(now)) LOGF("net: task appears STALLED");
    g_frames = 0;
    g_total_us = 0;
    g_shell_us = 0;
    g_text_us = 0;
    vt = views::CoverLight::Timing();
  }
}
