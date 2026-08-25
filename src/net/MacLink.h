#pragma once

// The Mac link protocol, as a pure state machine.
//
// HostLink has been half a subsystem since early on: a device-side listener
// whose Mac-side sender was never written, which is why every boot log has said
// "heartbeat never seen". This is the parsing and policy half of the other side,
// kept free of Arduino and sockets so it is host-testable - the same shape as
// Backlight, HostLink and CrashPolicy, and for the same reason.
//
// The wire format is key=value lines, one per line, in BOTH directions. Not
// query parameters, because the field that matters most is the computer name and
// this one is "Wes's MacBook Pro" - spaces and an apostrophe, which would
// mean percent-encoding and a decoder. Not JSON, because HostLinkEsp32
// hand-parses a raw WiFiServer rather than using WebServer, and its comment
// gives the reason: internal SRAM is the binding constraint on this board.
//
// Unknown keys are ignored in both directions, deliberately. The helper runs at
// login and firmware changes when it is flashed, so the two will not move in
// lockstep, and both directions have to degrade rather than break.

#include <cstddef>
#include <cstdint>

namespace net {

struct MacState {
  bool locked = false;
  // True once a good beat has been applied. Until then nothing here is real and
  // no Mac-dependent UI may claim otherwise.
  bool valid = false;
  char host[64] = {};
  // The name Spotify Connect advertises for this machine. On macOS that is the
  // computer name, but it is sent as its own field so the two can diverge
  // without a firmware change.
  char sp_device[64] = {};
  // -1 means the Mac did not report one. Never coerce to 0 - that would draw a
  // confident empty slider for a value we do not have.
  int out_vol = -1;

  // Playback, straight from the Mac's own Spotify via AppleScript. Verified
  // available on the real machine, and it costs no API quota - which is the
  // whole point: the GET /me/player poll that exhausted the quota is exactly
  // the call this replaces.
  //
  // CARRIED ONLY for now. Nothing makes the Mac the authority on playback yet;
  // that needs an answer to what happens when it and the API disagree, and
  // "Mac says playing while the API reports a different active device" is a
  // real state rather than an error.
  bool sp_playing = false;
  char sp_track[128] = {};
  char sp_artist[128] = {};
  char sp_uri[64] = {};
  // Milliseconds. -1 means the Mac did not report them, and a confident 0 would
  // draw a progress ring at the start of a track that is not playing.
  int32_t sp_pos_ms = -1;
  int32_t sp_dur_ms = -1;
};

class MacLink {
 public:
  // How long the device holds the helper's request waiting for a command.
  //
  // Chosen over a fixed fast heartbeat because it is LIGHTER on the Mac, not
  // merely faster: a process blocked on a socket read costs zero CPU and no
  // timer wakeups, where waking an interpreter twice a second is what stops a
  // laptop reaching deep idle. All the resulting complexity is on this side.
  static constexpr uint32_t HOLD_MS = 1000;
  // Silence after which the Mac's reported state is no longer trusted.
  static constexpr uint32_t TIMEOUT_MS = 25000;
  static constexpr int PROTOCOL_V = 1;
  // Bounded because poll() runs in the render loop.
  static constexpr size_t MAX_BODY = 512;
  static constexpr size_t TOKEN_CAP = 64;

  // An EMPTY token disables the command channel entirely, leaving sleep state
  // working. An unconfigured device must not ship a command channel with a
  // blank password.
  void setToken(const char *tok);
  bool commandChannelEnabled() const { return token_[0] != '\0'; }

  // Parses one request body. Returns false on a bad version, a bad token or an
  // oversized body - and then applies NOTHING. Partial application is the
  // dangerous failure: a rejected beat that still moved `locked` would let
  // anything on the LAN darken the screen.
  bool applyBeat(const char *body, uint32_t now_ms);

  // The knob turned by `detents`. ACCUMULATED, not replaced: a delta is a
  // count of clicks, so two clicks while one is in flight is four clicks, where
  // an absolute target would have discarded the first.
  //
  // A delta rather than a target because macOS's volume keys move in discrete
  // steps and a knob produces discrete detents - one maps to the other exactly.
  // It also removes a feedback loop that an absolute target could not avoid: the
  // helper reads the volume BEFORE applying a pending command, so it always
  // reports a value one command stale, and the device resyncing to that made the
  // knob fight itself.
  void requestOutputVolumeDelta(int detents);
  bool hasPending() const { return pending_delta_ != 0; }

  // Clamped so one stuck report cannot spin the volume end to end.
  static constexpr int MAX_DELTA = 16;

  // Writes the response body and CLEARS the pending command, so it is delivered
  // exactly once. Returns the length written, or 0 if it would not fit.
  int buildResponse(char *out, size_t cap);

  const MacState &state() const { return state_; }

  // Wrap-safe, the same convention HostLink::hostAsleep uses. Never-heard-from
  // is NOT stale: failing open means a mechanism nobody uses changes nothing.
  bool stale(uint32_t now_ms) const;

 private:
  MacState state_;
  char token_[TOKEN_CAP] = {};
  int pending_delta_ = 0;
  uint32_t last_beat_ms_ = 0;
};

}  // namespace net
