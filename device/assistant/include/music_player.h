#ifndef MUSIC_PLAYER_H
#define MUSIC_PLAYER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MUSIC_STATE_STOPPED = 0,
    MUSIC_STATE_BUFFERING,
    MUSIC_STATE_DEFERRED,
    MUSIC_STATE_PLAYING,
    MUSIC_STATE_PAUSED
} music_state_t;

int music_player_init(void);
void music_player_destroy(void);

int music_player_play_url(const char *url, const char *title, bool defer_start);
int music_player_pause(void);
int music_player_resume(bool defer_start);
int music_player_stop(void);

void music_player_on_idle(void);
void music_player_duck(int duration_ms);
bool music_player_is_playing(void);
bool music_player_has_deferred(void);
bool music_player_take_natural_end(void);
void music_player_mark_resume_after_tts(void);
bool music_player_take_resume_after_tts(void);
music_state_t music_player_get_state(void);
void music_player_get_status(char *buf, size_t size);

#endif
