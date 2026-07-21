#include "GemPlayer.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#ifdef PIN_BUZZER
#include <NonBlockingRtttl.h>
#endif

#ifndef MESHGEMS_MAX_PARTS
#define MESHGEMS_MAX_PARTS 8
#endif

#ifndef MESHGEMS_MAX_PART_LEN
#define MESHGEMS_MAX_PART_LEN 160
#endif

#ifndef MESHGEMS_BASE_NOTE
#define MESHGEMS_BASE_NOTE 'i'
#endif

#ifndef MESHGEMS_BASE_FREQ
#define MESHGEMS_BASE_FREQ 440.0f
#endif

#ifndef MESHGEMS_ARPEG_MS
#define MESHGEMS_ARPEG_MS 12
#endif

// silent gap at each tick boundary so repeated notes articulate as
// separate onsets (a continuous tone() at the same freq would merge them)
#ifndef MESHGEMS_TICK_GAP_MS
#define MESHGEMS_TICK_GAP_MS 15
#endif

#ifndef MESHGEMS_PLAYLIST_SLOTS
#define MESHGEMS_PLAYLIST_SLOTS 8
#endif

#ifndef MESHGEMS_PLAYLIST_GAP_MS
#define MESHGEMS_PLAYLIST_GAP_MS 3000
#endif

#ifndef MESHGEMS_MAX_PAYLOAD_LEN
#define MESHGEMS_MAX_PAYLOAD_LEN 160
#endif

const char *meshgems_find_payload(const char *text) {
  if (!text) return NULL;
  const char *found = strstr(text, MESHGEMS_PREFIX);
  if (!found) return NULL;
  return found + MESHGEMS_PREFIX_LEN;
}

const char *meshgems_dm_payload(const char *text) {
  if (!text || strncmp(text, MESHGEMS_PREFIX, MESHGEMS_PREFIX_LEN) != 0) return NULL;
  return text + MESHGEMS_PREFIX_LEN;
}

#ifdef PIN_BUZZER

struct VoiceState {
  const char *part;
  int pos;
  char last_note;
  int ticks_left;
};

static char g_parts[MESHGEMS_MAX_PARTS][MESHGEMS_MAX_PART_LEN];
static int g_num_parts = 0;
static uint16_t g_tick_ms = 200;
static VoiceState g_voices[MESHGEMS_MAX_PARTS];
static bool g_playing = false;
static bool g_quiet = true;
static unsigned long g_tick_start = 0;
static unsigned long g_gap_until = 0;
static unsigned long g_arpeg_next = 0;
static int g_arpeg_idx = 0;
static uint16_t g_active_freqs[MESHGEMS_MAX_PARTS];
static int g_active_count = 0;

// #meshtunes playlist: round-robin loop over queued gem payloads
static char g_playlist[MESHGEMS_PLAYLIST_SLOTS][MESHGEMS_MAX_PAYLOAD_LEN];
static uint8_t g_playlist_count = 0;
static uint8_t g_playlist_write = 0;
static uint8_t g_playlist_next = 0;
static unsigned long g_next_song_at = 0;
static bool g_playlist_paused = false;

static bool is_note_char(char c) {
  return c >= 'H' && c <= 'z';
}

static uint16_t note_to_freq(char note) {
  if (!is_note_char(note)) return 0;
  float semitones = (float)(note - MESHGEMS_BASE_NOTE);
  float freq = MESHGEMS_BASE_FREQ * powf(2.0f, semitones / 12.0f);
  if (freq < 1.0f) return 0;
  if (freq > 65535.0f) return 65535;
  return (uint16_t)(freq + 0.5f);
}

static void voice_reset(VoiceState *voice, const char *part) {
  voice->part = part;
  voice->pos = 0;
  voice->last_note = 0;
  voice->ticks_left = 0;
}

static void voice_advance(VoiceState *voice) {
  while (voice->ticks_left <= 0) {
    char c = voice->part[voice->pos];
    if (c == '\0') {
      voice->last_note = 0;
      voice->ticks_left = 0;
      return;
    }

    if (c == '0') {
      voice->last_note = 0;
      voice->ticks_left = 1;
      voice->pos++;
      return;
    }

    if (c >= '1' && c <= '9') {
      voice->last_note = 0;
      voice->ticks_left = c - '0';
      voice->pos++;
      return;
    }

    if (c == '-') {
      voice->pos++;
      int hold = 1;
      if (voice->part[voice->pos] >= '1' && voice->part[voice->pos] <= '9') {
        hold = voice->part[voice->pos] - '0';
        voice->pos++;
      }
      voice->ticks_left = hold;
      return;
    }

    if (is_note_char(c)) {
      voice->last_note = c;
      voice->ticks_left = 1;
      voice->pos++;
      return;
    }

    voice->pos++;
  }
}

static void refresh_active_freqs(void) {
  g_active_count = 0;
  for (int i = 0; i < g_num_parts; i++) {
    if (g_voices[i].last_note != 0) {
      uint16_t freq = note_to_freq(g_voices[i].last_note);
      if (freq > 0 && g_active_count < MESHGEMS_MAX_PARTS) {
        g_active_freqs[g_active_count++] = freq;
      }
    }
  }
}

static bool all_voices_done(void) {
  for (int i = 0; i < g_num_parts; i++) {
    VoiceState *voice = &g_voices[i];
    if (voice->ticks_left > 0) return false;
    if (voice->part[voice->pos] != '\0') return false;
  }
  return true;
}

static void tick_step(void) {
  for (int i = 0; i < g_num_parts; i++) {
    VoiceState *voice = &g_voices[i];
    if (voice->ticks_left > 0) {
      voice->ticks_left--;
    }
    while (voice->ticks_left <= 0 && voice->part[voice->pos] != '\0') {
      voice_advance(voice);
    }
  }
}

static void arpeggiate(unsigned long now) {
  if (g_active_count == 0 || now < g_gap_until) {
    noTone(PIN_BUZZER);
    return;
  }
  if (now < g_arpeg_next) return;
  g_arpeg_next = now + MESHGEMS_ARPEG_MS;
  uint16_t freq = g_active_freqs[g_arpeg_idx % g_active_count];
  g_arpeg_idx++;
  tone(PIN_BUZZER, freq);
}

static bool parse_payload(const char *payload) {
  if (!payload || !*payload) return false;

  const char *body = payload;
  g_tick_ms = 200;

  const char *colon = strchr(payload, ':');
  if (colon && colon > payload) {
    bool all_digits = true;
    for (const char *p = payload; p < colon; p++) {
      if (*p < '0' || *p > '9') {
        all_digits = false;
        break;
      }
    }
    if (all_digits) {
      long tick = strtol(payload, NULL, 10);
      if (tick > 0 && tick <= 2000) {
        g_tick_ms = (uint16_t)tick;
      }
      body = colon + 1;
    }
  }

  g_num_parts = 0;

  const char *start = body;
  for (const char *p = body;; p++) {
    if (*p == '|' || *p == '\0') {
      int len = (int)(p - start);
      if (len > 0 && g_num_parts < MESHGEMS_MAX_PARTS) {
        if (len >= MESHGEMS_MAX_PART_LEN) {
          len = MESHGEMS_MAX_PART_LEN - 1;
        }
        memcpy(g_parts[g_num_parts], start, len);
        g_parts[g_num_parts][len] = '\0';
        g_num_parts++;
      }
      if (*p == '\0') break;
      start = p + 1;
    }
  }

  return g_num_parts > 0;
}

static void begin_playback(void) {
  for (int i = 0; i < g_num_parts; i++) {
    voice_reset(&g_voices[i], g_parts[i]);
    voice_advance(&g_voices[i]);
  }
  g_arpeg_idx = 0;
  g_tick_start = millis();
  g_gap_until = g_tick_start;
  g_arpeg_next = g_tick_start;
  refresh_active_freqs();
}

void GemPlayer_stop(void) {
  if (!g_playing) return;
  g_playing = false;
  noTone(PIN_BUZZER);
}

bool GemPlayer_tryPlay(const char *payload, bool quiet) {
  g_quiet = quiet;
  if (g_quiet) return false;
  if (!parse_payload(payload)) return false;

#ifdef PIN_BUZZER
  if (rtttl::isPlaying()) {
    rtttl::stop();
  }
#endif

  begin_playback();
  g_playing = true;
  return true;
}

bool GemPlayer_isPlaying(void) {
  return g_playing;
}

bool GemPlayer_skip(void) {
  if (!g_playing) return false;
  GemPlayer_stop();
  if (g_playlist_count > 0 && g_next_song_at != 0) {
    g_next_song_at = millis() + 300;  // brief pause, then next playlist song
  }
  return true;
}

void GemPlayer_queueAdd(const char *payload) {
  if (!payload || !*payload) return;

  size_t len = strlen(payload);
  if (len >= MESHGEMS_MAX_PAYLOAD_LEN) len = MESHGEMS_MAX_PAYLOAD_LEN - 1;

  // skip exact duplicates (channel messages can be re-heard via flood)
  for (int i = 0; i < g_playlist_count; i++) {
    if (strncmp(g_playlist[i], payload, len) == 0 && g_playlist[i][len] == '\0') {
      return;
    }
  }

  memcpy(g_playlist[g_playlist_write], payload, len);
  g_playlist[g_playlist_write][len] = '\0';
  g_playlist_write = (g_playlist_write + 1) % MESHGEMS_PLAYLIST_SLOTS;
  if (g_playlist_count < MESHGEMS_PLAYLIST_SLOTS) g_playlist_count++;

  if (!g_playing && g_next_song_at == 0 && !g_playlist_paused) {
    g_next_song_at = millis();  // start the loop on first queued song
  }
}

bool GemPlayer_togglePlaylist(void) {
  if (g_playlist_count == 0) return false;

  g_playlist_paused = !g_playlist_paused;
  if (g_playlist_paused) {
    GemPlayer_stop();
    g_next_song_at = 0;
  } else {
    g_next_song_at = millis();
  }
  return true;
}

bool GemPlayer_playlistIsPaused(void) {
  return g_playlist_paused;
}

static void playlist_service(unsigned long now, bool quiet) {
  if (g_playlist_paused || g_playing || g_playlist_count == 0 || g_next_song_at == 0) return;
  if (now < g_next_song_at) return;

  if (quiet) {  // stay armed; resume when unmuted
    g_next_song_at = now + MESHGEMS_PLAYLIST_GAP_MS;
    return;
  }

  if (g_playlist_next >= g_playlist_count) g_playlist_next = 0;
  if (parse_payload(g_playlist[g_playlist_next])) {
    begin_playback();
    g_playing = true;
  }
  g_playlist_next = (g_playlist_next + 1) % g_playlist_count;
}

void GemPlayer_loop(bool quiet) {
  unsigned long now = millis();

  if (!g_playing) {
    playlist_service(now, quiet);
    if (!g_playing) return;
  }

  arpeggiate(now);

  if (now - g_tick_start >= (unsigned long)g_tick_ms) {
    tick_step();
    if (all_voices_done()) {
      GemPlayer_stop();
      if (!g_playlist_paused && g_playlist_count > 0 && g_next_song_at != 0) {
        g_next_song_at = now + MESHGEMS_PLAYLIST_GAP_MS;
      }
      return;
    }
    g_tick_start = now;
    g_gap_until = now + MESHGEMS_TICK_GAP_MS;
    g_arpeg_idx = 0;
    g_arpeg_next = now;
    refresh_active_freqs();
  }
}

#else

bool GemPlayer_tryPlay(const char *payload, bool quiet) {
  (void)payload;
  (void)quiet;
  return false;
}

void GemPlayer_queueAdd(const char *payload) {
  (void)payload;
}

void GemPlayer_stop(void) {}

bool GemPlayer_skip(void) {
  return false;
}

bool GemPlayer_togglePlaylist(void) {
  return false;
}

bool GemPlayer_playlistIsPaused(void) {
  return false;
}

bool GemPlayer_isPlaying(void) {
  return false;
}

void GemPlayer_loop(bool quiet) {
  (void)quiet;
}

#endif
