#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static const char MESHGEMS_PREFIX[] = u8"\xF0\x9F\x8E\xB6";  // 🎶
static const size_t MESHGEMS_PREFIX_LEN = 4;

const char *meshgems_find_payload(const char *text);
const char *meshgems_dm_payload(const char *text);

bool GemPlayer_tryPlay(const char *payload, bool quiet);
void GemPlayer_queueAdd(const char *payload);
void GemPlayer_loop(bool quiet);
void GemPlayer_stop(void);
bool GemPlayer_skip(void);
bool GemPlayer_togglePlaylist(void);
bool GemPlayer_playlistIsPaused(void);
bool GemPlayer_isPlaying(void);

#ifdef __cplusplus
}
#endif
