#include "NowPlaying.h"

#include <cstdio>
#include <cstring>

#include "gfx/Color.h"
#include "gfx/Font.h"
#include "gfx/Geometry.h"
#include "gfx/fonts/Fonts.h"

namespace shell {
namespace {

// mm:ss. An absurd duration renders as --:-- rather than as a plausible-looking
// wrong number.
void formatTime(char *out, size_t cap, uint32_t ms) {
  if (ms > 24u * 3600u * 1000u) {
    std::snprintf(out, cap, "--:--");
    return;
  }
  const uint32_t total = ms / 1000u;
  std::snprintf(out, cap, "%u:%02u", total / 60u, total % 60u);
}

// Fits `src` into `max_w`, truncating on a CHARACTER boundary with an ellipsis.
//
// Character boundary, not byte: cutting a multi-byte sequence in half renders
// its tail as a fallback glyph, so a trimmed accented name would end in stray
// punctuation.
void fitInto(char *dst, size_t cap, const gfx::GFXfont &f, const char *src,
             int max_w) {
  dst[0] = '\0';
  if (!src || !src[0] || max_w <= 0) return;
  if (gfx::textWidth(f, src) <= max_w) {
    std::snprintf(dst, cap, "%s", src);
    return;
  }

  const int ell = gfx::textWidth(f, "...");
  const int budget = max_w - ell;
  int used = 0, w = 0;
  const char *p = src;
  while (*p) {
    // One character at a time, measured as it goes.
    const char *start = p;
    int adv = 0;
    {
      // Step one character by measuring the prefix difference: keeps all UTF-8
      // decoding in one place rather than duplicating it here.
      char one[8] = {};
      int n = 1;
      const unsigned char c = static_cast<unsigned char>(*p);
      if ((c & 0xE0) == 0xC0) n = 2;
      else if ((c & 0xF0) == 0xE0) n = 3;
      else if ((c & 0xF8) == 0xF0) n = 4;
      for (int i = 0; i < n && start[i]; ++i) one[i] = start[i];
      adv = gfx::textWidth(f, one);
      p += n;
      if (!*start) break;
    }
    if (w + adv > budget) break;
    const int nbytes = static_cast<int>(p - start);
    if (used + nbytes + 4 >= static_cast<int>(cap)) break;
    std::memcpy(dst + used, start, static_cast<size_t>(nbytes));
    used += nbytes;
    w += adv;
  }
  dst[used] = '\0';
  std::snprintf(dst + used, cap - used, "...");
}

}  // namespace

void NowPlaying::prepare(const PlaybackState &pb, uint32_t shown_ms,
                         bool suppress_times) {
  have_track_ = pb.has_track;
  have_times_ = false;
  // Only claim a saved-state for a track that exists. Carrying the last track's
  // heart across a gap would be a confident answer about the wrong song.
  liked_known_ = pb.has_track && pb.liked_known;
  liked_ = pb.liked;

  if (!have_track_) {
    // Says so rather than leaving the area blank: a blank strip is
    // indistinguishable from a rendering failure.
    const int w = gfx::halfChordAt(TITLE_BASELINE, MARGIN) * 2;
    fitInto(title_, sizeof(title_), gfx::fontArtist(), "nothing playing", w);
    title_x_ = gfx::CX - gfx::textWidth(gfx::fontArtist(), title_) / 2;
    artist_[0] = '\0';
    return;
  }

  const int tw = gfx::halfChordAt(TITLE_BASELINE, MARGIN) * 2;
  fitInto(title_, sizeof(title_), gfx::fontTitle(), pb.title, tw);
  title_x_ = gfx::CX - gfx::textWidth(gfx::fontTitle(), title_) / 2;

  const int aw = gfx::halfChordAt(ARTIST_BASELINE, MARGIN) * 2;
  fitInto(artist_, sizeof(artist_), gfx::fontArtist(), pb.artist, aw);
  artist_x_ = gfx::CX - gfx::textWidth(gfx::fontArtist(), artist_) / 2;

  // Elapsed and remaining, flanking the centre. Remaining rather than total,
  // because "how much is left" is the question a listener actually has.
  const int half = gfx::halfChordAt(TIME_BASELINE, MARGIN);
  if (half > 50 && pb.duration_ms > 0) {
    formatTime(elapsed_, sizeof(elapsed_), shown_ms);
    char rbuf[14];
    formatTime(rbuf, sizeof(rbuf),
               pb.duration_ms > shown_ms ? pb.duration_ms - shown_ms : 0);
    std::snprintf(remaining_, sizeof(remaining_), "-%s", rbuf);
    elapsed_x_ = gfx::CX - half;
    remaining_x_ =
        gfx::CX + half - gfx::textWidth(gfx::fontSmall(), remaining_);
    have_times_ = !suppress_times;
  }
}

void NowPlaying::render(gfx::Surface &s, uint16_t tint) const {
  const uint16_t white = gfx::rgb565(235, 235, 240);
  const uint16_t grey = gfx::rgb565(150, 150, 162);

  // Saved-state. Nothing is drawn while it is unknown - not a hollow heart,
  // which would read as a confident "no". A filled heart takes the album tint
  // so it belongs to the artwork; an outline stays grey so the two states differ
  // in shape as well as in colour, which is what makes them readable at 26px.
  if (liked_known_) {
    drawGlyph(s, liked_ ? Glyph::HeartFilled : Glyph::HeartOutline, gfx::CX,
              HEART_CY, HEART_HALF, liked_ ? tint : grey, 256);
  }

  if (!have_track_) {
    if (title_[0])
      gfx::drawText(s, gfx::fontArtist(), title_x_, TITLE_BASELINE, title_,
                    grey);
    return;
  }

  if (title_[0])
    gfx::drawText(s, gfx::fontTitle(), title_x_, TITLE_BASELINE, title_, white);
  if (artist_[0])
    gfx::drawText(s, gfx::fontArtist(), artist_x_, ARTIST_BASELINE, artist_,
                  grey);
  if (have_times_) {
    gfx::drawText(s, gfx::fontSmall(), elapsed_x_, TIME_BASELINE, elapsed_,
                  tint);
    gfx::drawText(s, gfx::fontSmall(), remaining_x_, TIME_BASELINE, remaining_,
                  tint);
  }
}

}  // namespace shell
