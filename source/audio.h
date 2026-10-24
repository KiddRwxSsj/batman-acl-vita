#ifndef AUDIO_H
#define AUDIO_H

void audio_init(void);

int audio_load_sound(const char *name);
int audio_play_sound(int sound, int loop, int priority);
void audio_stop_sound(int stream);
void audio_set_volume(int stream, float vol);
void audio_effect_volume(float vol);

void audio_play_song(const char *name);
void audio_stop_song(void);
void audio_music_volume(float vol);

#endif
