#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>

#include "audio.h"
#include "bionic.h"
#include "gfx.h"
#include "input.h"
#include "java.h"
#include "loader.h"
#include "obb.h"
#include "util.h"
#include "perf.h"
#include "movie.h"
#include "comic.h"
#include "store.h"
#include "zfast.h"
#include "tilt.h"

// 40 MB less than before, big allocations get kernel memblocks from it instead (bionic.c big_alloc)
int _newlib_heap_size_user = 200 * 1024 * 1024;

static int thiz_storage;
jobject java_thiz = (jobject)&thiz_storage;

native_str_fn NativeCallback_KeyboardFinished;

static const char *system_language(void) {
    static const char *langs[] = {
        "ja", "en", "fr", "es", "de", "it", "nl", "pt", "ru", "ko", "zh", "zh", "fi", "sv", "da", "no", "pl", "pt", "en", "tr",
    };
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_US;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    if (lang < 0 || lang >= (int)(sizeof(langs) / sizeof(langs[0])))
        return "en";
    return langs[lang];
}

#ifdef HANG_DUMP
// no frames, links or reads for 30s, or still no frame after 2 min, means a hang.
// crash on purpose so the vita writes a coredump with every thread's state in it.
// no logging in here, the log path itself might be what's stuck
static int watchdog(SceSize args, void *argp) {
    unsigned last = progress_ticks;
    int idle = 0, up = 0;
    while (1) {
        sceKernelDelayThread(1000 * 1000);
        up++;
        if (progress_ticks != last) {
            last = progress_ticks;
            idle = 0;
        } else {
            idle++;
        }
        if (idle >= 30 || (up >= 120 && !swap_ticks))
            *(volatile int *)(0xdead0000 + (idle >= 30)) = up;
    }
    return 0;
}
#endif

#define CONFIG_PATH DATA_PATH "config.txt"

static const char *default_config =
    "# 1 = on, 0 = off\n"
    "intro_movie 1\n"
    "# 1 = every batsuit unlocked for free, 0 = suits cost 1000 waynetech points each\n"
    "unlock_suits 0\n"
    "# 1 = decode packages with libdeflate (faster loads), 0 = stock zlib\n"
    "fast_inflate 1\n"
    "# 1 = tilt the vita to steer the flying batarang (the left stick works too), 0 = stick only\n"
    "tilt 1\n"
    "# tilt strength in percent, higher = less tilting needed\n"
    "tilt_sensitivity 100\n";

static int intro_movie = 1;

static struct { const char *key; int *value; } options[] = {
    { "intro_movie", &intro_movie },
    { "unlock_suits", &store_free },
    { "fast_inflate", &zf_enabled },
    { "tilt", &tilt_enabled },
    { "tilt_sensitivity", &tilt_sensitivity },
};

static void load_config(void) {
    void *buf;
    size_t size;
    if (!file_load(CONFIG_PATH, &buf, &size)) {
        file_save(CONFIG_PATH, default_config, strlen(default_config));
        buf = strdup(default_config);
        size = strlen(default_config);
    }
    char *text = realloc(buf, size + 1);
    text[size] = 0;
    for (char *line = strtok(text, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char key[32];
        int value;
        if (line[0] == '#' || sscanf(line, "%31s %d", key, &value) != 2)
            continue;
        for (int i = 0; i < (int)(sizeof(options) / sizeof(options[0])); i++)
            if (!strcmp(key, options[i].key))
                *options[i].value = value;
    }
    free(text);
    LOG("config: intro_movie %d unlock_suits %d fast_inflate %d", intro_movie, store_free, zf_enabled);
}

static jstring jstring_new(const char *s) {
    return jni->NewStringUTF(&jni, s);
}

int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    SceAppUtilInitParam init = {0};
    SceAppUtilBootParam boot = {0};
    sceAppUtilInit(&init, &boot);

    bionic_init();
    mkpath(DATA_PATH);
    log_open();
    LOG("starting %s", BUILD_TAG);
#ifdef PERF_TRACE
    perf_thread(sceKernelGetThreadId(), "main");
#endif
#ifdef HANG_DUMP
    SceUID wd = sceKernelCreateThread("watchdog", watchdog, 64, 0x4000, 0, 0, NULL);
    LOG("watchdog %08x %08x", wd, sceKernelStartThread(wd, 0, NULL));
#endif

    if (obb_open(OBB_PATH) < 0)
        fatal_error("Missing %s\nCopy the expansion file from Android/obb/com.wb.goog.ArkhamCity there.", OBB_PATH);

    load_config();
    loader_init();
    jni_init();
    audio_init();
    input_init();

    int (*JNI_OnLoad)(JavaVM *, void *) = (void *)ue3_sym("JNI_OnLoad");
    native_str_fn SetPackageName = (native_str_fn)ue3_sym("_Z29NativeCallback_SetPackageNameP7_JNIEnvP8_jobjectP8_jstring");
    native_str_fn SetDeviceType = (native_str_fn)ue3_sym("_Z28NativeCallback_SetDeviceTypeP7_JNIEnvP8_jobjectP8_jstring");
    native_str_fn LanguageSet = (native_str_fn)ue3_sym("_Z26NativeCallback_LanguageSetP7_JNIEnvP8_jobjectP8_jstring");
    native_bool_fn InitEGLCallback = (native_bool_fn)ue3_sym("_Z30NativeCallback_InitEGLCallbackP7_JNIEnvP8_jobject");
    native_void_fn SetEGLDirty = (native_void_fn)ue3_sym("_Z26NativeCallback_SetEGLDirtyP7_JNIEnvP8_jobject");
    jboolean (*SystemStats)(JNIEnv *, jobject, int64_t, int) =
        (void *)ue3_sym("_Z26NativeCallback_SystemStatsP7_JNIEnvP8_jobjectxi");
    void (*PostInitUpdate)(JNIEnv *, jobject, int, int) =
        (void *)ue3_sym("_Z29NativeCallback_PostInitUpdateP7_JNIEnvP8_jobjectii");
    jboolean (*Init)(JNIEnv *, jobject, int, int, float, jboolean, jobject, jboolean) =
        (void *)ue3_sym("_Z19NativeCallback_InitP7_JNIEnvP8_jobjectiifhS2_h");
    NativeCallback_KeyboardFinished = (native_str_fn)ue3_sym("_Z31NativeCallback_KeyboardFinishedP7_JNIEnvP8_jobjectP8_jstring");

    LOG("JNI_OnLoad");
    JNI_OnLoad(&jvm, NULL);

    // same order the java activity drives it in: obb, device, memory, egl, surface, language, start
    SetPackageName(&jni, java_thiz, jstring_new("/sdcard/main.2.com.wb.goog.ArkhamCity.obb"));
    SetDeviceType(&jni, java_thiz, jstring_new("PS Vita"));
    SystemStats(&jni, java_thiz, 300LL * 1024 * 1024, 3);

    LOG("InitEGL");
    if (!InitEGLCallback(&jni, java_thiz))
        fatal_error("Engine failed to set up EGL.");
    // android plays this over the loading screen (UE3JavaApp.PostStartup), here it runs before the engine
    if (intro_movie)
        movie_play(OBB_ROOT "Movies/FrozenRain.m4v");
    SetEGLDirty(&jni, java_thiz);
    PostInitUpdate(&jni, java_thiz, SCREEN_W, SCREEN_H);
    LanguageSet(&jni, java_thiz, jstring_new(system_language()));

    LOG("Init");
    // 5" diagonal, close enough to the vita's screen for the touch ui scaling
    if (!Init(&jni, java_thiz, SCREEN_W, SCREEN_H, 5.0f, JNI_TRUE, java_thiz, JNI_FALSE))
        fatal_error("Engine init failed.");

    LOG("engine thread running");
    uint64_t mem_last = uptime_ms();
    unsigned swaps_last = swap_ticks;
    while (1) {
        if (comic_requested)
            comic_run();
        input_poll();
        zf_tick();
        // testers only send log.txt and a dump, so memory and fps go in there all the time
        uint64_t now = uptime_ms();
        if (now - mem_last >= 5000) {
            char what[16];
            snprintf(what, sizeof(what), "%u fps", (unsigned)((swap_ticks - swaps_last) * 1000 / (now - mem_last)));
            mem_log(what);
            mem_last = now;
            swaps_last = swap_ticks;
        }
        sceKernelDelayThread(8000);
    }
    return 0;
}
