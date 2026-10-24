#include "audio.h"
#include "obb.h"
#include "util.h"
#include "perf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb/stb_vorbis.c>

#define MIX_RATE   44100
#define GRAIN      512
#define MAX_DEFS   2048
#define MAX_VOICES 24
#define MUSIC      MAX_VOICES
#define CACHE_MAX  (16 * 1024 * 1024)

enum { V_FREE, V_PENDING, V_PLAYING };

typedef struct {
    char *name;
    uint8_t *ogg;
    uint32_t size;
    uint64_t used;
} sound_def;

typedef struct {
    int state;
    int id;
    int def;
    int loop;
    int priority;
    float vol;
    stb_vorbis *vb;
    uint8_t *own;
    uint32_t step;
    uint32_t frac;
    short cur[2], next[2];
    short buf[GRAIN * 2];
    int buf_len, buf_pos;
} voice;

static sound_def defs[MAX_DEFS];
static int def_count;
static uint32_t cache_size;

static voice voices[MAX_VOICES + 1];
static int next_id = 1;
static float effect_vol = 1.0f, music_vol = 1.0f;
static int music_is_vox;
static char music_name[128];

static SceKernelLwMutexWork mix_lock;
static SceUID load_sema;

static void lock(void) { sceKernelLockLwMutex(&mix_lock, 1, NULL); }
static void unlock(void) { sceKernelUnlockLwMutex(&mix_lock, 1); }

static void voice_release(voice *v) {
    if (v->vb)
        stb_vorbis_close(v->vb);
    free(v->own);
    v->vb = NULL;
    v->own = NULL;
    v->state = V_FREE;
    v->id = 0;
}

static void evict(uint32_t need) {
    while (cache_size + need > CACHE_MAX) {
        int oldest = -1;
        for (int i = 0; i < def_count; i++) {
            if (!defs[i].ogg)
                continue;
            int busy = 0;
            for (int j = 0; j < MAX_VOICES; j++)
                if (voices[j].state != V_FREE && voices[j].def == i)
                    busy = 1;
            if (!busy && (oldest < 0 || defs[i].used < defs[oldest].used))
                oldest = i;
        }
        if (oldest < 0)
            return;
        free(defs[oldest].ogg);
        defs[oldest].ogg = NULL;
        cache_size -= defs[oldest].size;
    }
}

static stb_vorbis *open_def(int def) {
    sound_def *d = &defs[def];
    if (!d->ogg) {
        char path[256];
        uint32_t size;
        snprintf(path, sizeof(path), OBB_ROOT "Audio/%s.ogg", d->name);
        uint8_t *data = obb_load(path, &size);
        if (!data) {
            LOG("sound %s not in obb", path);
            return NULL;
        }
        lock();
        evict(size);
        d->ogg = data;
        d->size = size;
        cache_size += size;
        unlock();
    }
    d->used = uptime_ms();
    return stb_vorbis_open_memory(d->ogg, d->size, NULL, NULL);
}

static void start_voice(voice *v, stb_vorbis *vb) {
    stb_vorbis_info info = stb_vorbis_get_info(vb);
    v->vb = vb;
    v->step = (uint32_t)(((uint64_t)info.sample_rate << 16) / MIX_RATE);
    v->frac = 0;
    v->buf_len = v->buf_pos = 0;
    v->cur[0] = v->cur[1] = v->next[0] = v->next[1] = 0;
    v->state = V_PLAYING;
}

static int loader_thread(SceSize args, void *argp) {
    while (1) {
        sceKernelWaitSema(load_sema, 1, NULL);

        for (int i = 0; i <= MAX_VOICES; i++) {
            lock();
            voice *v = &voices[i];
            if (v->state != V_PENDING) {
                unlock();
                continue;
            }
            int id = v->id, def = v->def;
            char song[128];
            strcpy(song, music_name);
            unlock();

            stb_vorbis *vb = NULL;
            uint8_t *own = NULL;
            if (i == MUSIC) {
                char path[256];
                uint32_t size;
                snprintf(path, sizeof(path), OBB_ROOT "Music/%s.ogg", song);
                own = obb_load(path, &size);
                if (own)
                    vb = stb_vorbis_open_memory(own, size, NULL, NULL);
                else
                    LOG("song %s not in obb", path);
            } else {
                vb = open_def(def);
            }

            lock();
            if (v->state == V_PENDING && v->id == id && vb) {
                v->own = own;
                start_voice(v, vb);
                own = NULL;
                vb = NULL;
            } else if (v->state == V_PENDING && v->id == id) {
                v->state = V_FREE;
                v->id = 0;
            }
            unlock();
            if (vb)
                stb_vorbis_close(vb);
            free(own);
        }
    }
    return 0;
}

static int fill(voice *v) {
    int n = stb_vorbis_get_samples_short_interleaved(v->vb, 2, v->buf, GRAIN * 2);
    if (n == 0 && v->loop) {
        stb_vorbis_seek_start(v->vb);
        n = stb_vorbis_get_samples_short_interleaved(v->vb, 2, v->buf, GRAIN * 2);
    }
    v->buf_len = n;
    v->buf_pos = 0;
    return n;
}

static int next_frame(voice *v) {
    if (v->buf_pos >= v->buf_len && fill(v) == 0)
        return 0;
    v->cur[0] = v->next[0];
    v->cur[1] = v->next[1];
    v->next[0] = v->buf[v->buf_pos * 2];
    v->next[1] = v->buf[v->buf_pos * 2 + 1];
    v->buf_pos++;
    return 1;
}

static void mix_voice(voice *v, int *out, float gain) {
    int g = (int)(gain * v->vol * 256.0f);
    for (int i = 0; i < GRAIN; i++) {
        // 16.16 fixed point position, pull source frames until we pass the integer part
        while (v->frac >= 0x10000) {
            v->frac -= 0x10000;
            if (!next_frame(v)) {
                v->state = V_FREE;
                return;
            }
        }
        int t = v->frac >> 8;
        int l = v->cur[0] + (((v->next[0] - v->cur[0]) * t) >> 8);
        int r = v->cur[1] + (((v->next[1] - v->cur[1]) * t) >> 8);
        out[i * 2] += (l * g) >> 8;
        out[i * 2 + 1] += (r * g) >> 8;
        v->frac += v->step;
    }
}

static int mixer_thread(SceSize args, void *argp) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, MIX_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) {
        LOG("audio port failed %08x", port);
        return 0;
    }
    static int acc[GRAIN * 2];
    static short out[2][GRAIN * 2];
    int cur = 0;

    while (1) {
        memset(acc, 0, sizeof(acc));
        // lock per voice so a PlaySound from the engine never waits on a whole grain of decoding
        for (int i = 0; i <= MAX_VOICES; i++) {
            voice *v = &voices[i];
            if (v->state != V_PLAYING)
                continue;
            lock();
            if (v->state == V_PLAYING) {
                float gain = i == MUSIC ? (music_is_vox ? effect_vol : music_vol) : effect_vol;
                mix_voice(v, acc, gain);
                if (v->state == V_FREE)
                    voice_release(v);
            }
            unlock();
        }

        for (int i = 0; i < GRAIN * 2; i++) {
            int s = acc[i];
            out[cur][i] = s > 32767 ? 32767 : s < -32768 ? -32768 : s;
        }
        sceAudioOutOutput(port, out[cur]);
        cur ^= 1;
    }
    return 0;
}

void audio_init(void) {
    sceKernelCreateLwMutex(&mix_lock, "audio", 2, 0, NULL);
    load_sema = sceKernelCreateSema("audio_load", 0, 0, 1024, NULL);

    SceUID t = sceKernelCreateThread("mixer", mixer_thread, 0x10000100, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
#ifdef PERF_TRACE
    perf_thread(t, "mixer");
#endif
    t = sceKernelCreateThread("audio_load", loader_thread, 0x10000101, 0x20000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
#ifdef PERF_TRACE
    perf_thread(t, "aload");
#endif
}

int audio_load_sound(const char *name) {
    lock();
    for (int i = 0; i < def_count; i++) {
        if (strcmp(defs[i].name, name) == 0) {
            unlock();
            return i + 1;
        }
    }
    if (def_count == MAX_DEFS) {
        unlock();
        return 0;
    }
    defs[def_count].name = strdup(name);
    int id = ++def_count;
    unlock();
    return id;
}

int audio_play_sound(int sound, int loop, int priority) {
    if (sound < 1 || sound > def_count)
        return 0;

    lock();
    voice *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].state == V_FREE) {
            v = &voices[i];
            break;
        }
    }
    if (!v) {
        // out of voices, steal the lowest priority one-shot
        for (int i = 0; i < MAX_VOICES; i++) {
            if (!voices[i].loop && (!v || voices[i].priority < v->priority))
                v = &voices[i];
        }
        if (!v || v->priority > priority) {
            unlock();
            return 0;
        }
        voice_release(v);
    }
    v->state = V_PENDING;
    v->id = next_id++;
    v->def = sound - 1;
    v->loop = loop;
    v->priority = priority;
    v->vol = 1.0f;
    int id = v->id;
    unlock();

    sceKernelSignalSema(load_sema, 1);
    return id;
}

void audio_stop_sound(int stream) {
    if (stream < 1)
        return;
    lock();
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].id == stream) {
            voice_release(&voices[i]);
            break;
        }
    }
    unlock();
}

void audio_set_volume(int stream, float vol) {
    lock();
    for (int i = 0; i < MAX_VOICES; i++) {
        if (voices[i].id == stream) {
            voices[i].vol = vol;
            break;
        }
    }
    unlock();
}

void audio_effect_volume(float vol) {
    effect_vol = vol;
}

void audio_music_volume(float vol) {
    music_vol = vol;
}

void audio_play_song(const char *name) {
    lock();
    voice *v = &voices[MUSIC];
    voice_release(v);
    snprintf(music_name, sizeof(music_name), "%s", name);
    char *ext = strstr(music_name, ".ogg");
    if (ext)
        *ext = 0;
    // "_flmx" tracks are cutscene voice-over mixed as effects and never loop
    music_is_vox = strstr(music_name, "_flmx") != NULL;
    v->state = V_PENDING;
    v->id = next_id++;
    v->loop = !music_is_vox;
    v->vol = 1.0f;
    unlock();
    sceKernelSignalSema(load_sema, 1);
}

void audio_stop_song(void) {
    lock();
    voice_release(&voices[MUSIC]);
    unlock();
}
