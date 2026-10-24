#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <psp2/kernel/processmgr.h>

#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>

#include "java.h"
#include "audio.h"
#include "gfx.h"
#include "util.h"
#include "comic.h"
#include "store.h"
#include "tilt.h"
#include "zfast.h"
#include "bionic.h"
#include "leak.h"
#include <malloc.h>
#include <psp2/kernel/sysmem.h>
extern int _newlib_heap_size_user;

static const char *jstr(jstring s) {
    return s ? jni->GetStringUTFChars(&jni, s, NULL) : "";
}

static void jstr_free(jstring s, const char *c) {
    if (s)
        jni->ReleaseStringUTFChars(&jni, s, (char *)c);
}

#define MAX_VALUES 32

static struct { char *key, *value; } app_values[MAX_VALUES] = {
    { "STORAGE_ROOT", "/sdcard/" },
    { "BASE_DIR", "UnrealEngine3/UDKGame" },
    { "LOCAL_IP", "" },
};
static pthread_mutex_t values_lock = PTHREAD_MUTEX_INITIALIZER;

static int find_value(const char *key) {
    for (int i = 0; i < MAX_VALUES; i++)
        if (app_values[i].key && strcmp(app_values[i].key, key) == 0)
            return i;
    return -1;
}

static jboolean HasAppLocalValue(jmethodID id, va_list args) {
    jstring k = va_arg(args, jstring);
    const char *key = jstr(k);
    pthread_mutex_lock(&values_lock);
    int found = find_value(key) >= 0;
    pthread_mutex_unlock(&values_lock);
    VLOG("HasAppLocalValue %s -> %d", key, found);
    jstr_free(k, key);
    return found;
}

static jobject GetAppLocalValue(jmethodID id, va_list args) {
    jstring k = va_arg(args, jstring);
    const char *key = jstr(k);
    pthread_mutex_lock(&values_lock);
    int i = find_value(key);
    jstring ret = i >= 0 ? jni->NewStringUTF(&jni, app_values[i].value) : NULL;
    pthread_mutex_unlock(&values_lock);
    jstr_free(k, key);
    return ret;
}

static void SetAppLocalValue(jmethodID id, va_list args) {
    jstring k = va_arg(args, jstring);
    jstring v = va_arg(args, jstring);
    const char *key = jstr(k), *value = jstr(v);
    pthread_mutex_lock(&values_lock);
    int i = find_value(key);
    if (i < 0) {
        for (i = 0; i < MAX_VALUES && app_values[i].key; i++);
        if (i < MAX_VALUES)
            app_values[i].key = strdup(key);
    }
    if (i < MAX_VALUES)
        app_values[i].value = strdup(value);
    pthread_mutex_unlock(&values_lock);
    jstr_free(k, key);
    jstr_free(v, value);
}

enum {
    F_VALID = 1, F_SAMPLES, F_RED, F_GREEN, F_BLUE, F_ALPHA, F_DEPTH, F_STENCIL,
    F_ACTION_DOWN, F_ACTION_UP, F_ACTION_POINTER_DOWN, F_ACTION_POINTER_UP,
};

FieldsInt fieldsInt[] = {
    { F_VALID, 0 },
    { F_SAMPLES, 0 },
    { F_RED, 5 },
    { F_GREEN, 6 },
    { F_BLUE, 5 },
    { F_ALPHA, 0 },
    { F_DEPTH, 16 },
    { F_STENCIL, 0 },
    // android.view.MotionEvent, falso hands back 1 for unknown ints which turns every touch into garbage
    { F_ACTION_DOWN, 0 },
    { F_ACTION_UP, 1 },
    { F_ACTION_POINTER_DOWN, 5 },
    { F_ACTION_POINTER_UP, 6 },
};

static void set_field(int fid, int value) {
    for (int i = 0; i < sizeof(fieldsInt) / sizeof(fieldsInt[0]); i++)
        if (fieldsInt[i].id == fid)
            fieldsInt[i].value = value;
}

// the engine fills an EGLConfigParms with what it wants and reads back what it got
static jobject InitEGL(jmethodID id, va_list args) {
    jobject parms = va_arg(args, jobject);
    gfx_init();
    set_field(F_VALID, 1);
    set_field(F_SAMPLES, 0);
    set_field(F_RED, 8);
    set_field(F_GREEN, 8);
    set_field(F_BLUE, 8);
    set_field(F_ALPHA, 8);
    set_field(F_DEPTH, 24);
    set_field(F_STENCIL, 8);
    return parms ? parms : (jobject)0x42424242;
}

static jboolean ret_true(jmethodID id, va_list args) { return JNI_TRUE; }
static jboolean ret_false(jmethodID id, va_list args) { return JNI_FALSE; }
static void nop(jmethodID id, va_list args) {}

static jboolean Swap(jmethodID id, va_list args) {
    gfx_swap();
    return JNI_TRUE;
}

static void ShutDownApp(jmethodID id, va_list args) {
    sceKernelExitProcess(0);
}

static void PlaySong(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *name = jstr(s);
    audio_play_song(name);
    jstr_free(s, name);
}

static void StopSong(jmethodID id, va_list args) {
    audio_stop_song();
}

static void UpdateMusicVolume(jmethodID id, va_list args) {
    audio_music_volume((float)va_arg(args, double));
}

static void UpdateEffectVolume(jmethodID id, va_list args) {
    audio_effect_volume((float)va_arg(args, double));
}

static jint LoadSoundFile(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *name = jstr(s);
    int ret = audio_load_sound(name);
    jstr_free(s, name);
    return ret;
}

static jint PlaySound(jmethodID id, va_list args) {
    int sound = va_arg(args, int);
    int loop = va_arg(args, int);
    int priority = va_arg(args, int);
    return audio_play_sound(sound, loop, priority);
}

static void ToggleSensors(jmethodID id, va_list args) {
    tilt_toggle(va_arg(args, int));
}

static void StopSound(jmethodID id, va_list args) {
    audio_stop_sound(va_arg(args, int));
}

static void SetVolume(jmethodID id, va_list args) {
    int stream = va_arg(args, int);
    audio_set_volume(stream, (float)va_arg(args, double));
}

static void StartVideo(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *name = jstr(s);
    LOG("skipping movie %s", name);
    jstr_free(s, name);
    // only the loading screen movie goes through here
    mem_log("load start");
#ifdef LEAK_TRACE
    leak_report("load start");
#endif
}

// the only local "page" is comic.html, which android shows in its own comic reader
static void ShowWebPage(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *url = jstr(s);
    LOG("ShowWebPage %s", url);
    if (!strstr(url, "http"))
        comic_requested = 1;
    jstr_free(s, url);
}

static jboolean IAPEnabled(jmethodID id, va_list args) {
    return store_free;
}

// suits never get here (store.c swaps the native that calls this), anything else needs google play
static jboolean RequestPurchase(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *item = jstr(s);
    LOG("RequestPurchase %s", item);
    jstr_free(s, item);
    return JNI_FALSE;
}

static void ShowKeyBoard(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    if (NativeCallback_KeyboardFinished)
        NativeCallback_KeyboardFinished(&jni, java_thiz, s);
}

static void ShowToast(jmethodID id, va_list args) {
    jstring s = va_arg(args, jstring);
    const char *msg = jstr(s);
    LOG("toast: %s", msg);
    jstr_free(s, msg);
}

static void OutOfMemory(jmethodID id, va_list args) {
    int want = va_arg(args, int);
    struct mallinfo mi = mallinfo();
    // biggest block still there, full heap and fragmented heap look different
    size_t lo = 0, hi = 256 << 20;
    while (hi - lo > (64 << 10)) {
        size_t mid = (lo + hi) / 2;
        void *p = malloc(mid);
        if (p) {
            free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    SceKernelFreeMemorySizeInfo fm = { sizeof(fm) };
    sceKernelGetFreeMemorySize(&fm);
    LOG("engine out of memory, wanted %d bytes. heap arena %uMB used %uMB free chunks %uMB (%u), never used %uMB, biggest block %ukB, zf live %ukB, sys user %uMB phycont %uMB",
        want, mi.arena >> 20, mi.uordblks >> 20, mi.fordblks >> 20, mi.ordblks,
        (unsigned)((_newlib_heap_size_user - mi.arena) >> 20), (unsigned)(lo >> 10), zf_stats.live >> 10,
        fm.size_user >> 20, fm.size_phycont >> 20);
    mem_log("oom");
}

enum {
    M_MAKE_CURRENT = 1, M_UNMAKE_CURRENT, M_INIT_EGL, M_HAS_VALUE, M_GET_VALUE, M_SET_VALUE,
    M_PLAY_SONG, M_STOP_SONG, M_START_VIDEO, M_STOP_VIDEO, M_IS_MOVIE_PLAYING,
    M_SHOW_KEYBOARD, M_HIDE_KEYBOARD, M_HIDE_SPLASH, M_SHOW_SPLASH,
    M_MUSIC_VOLUME, M_EFFECT_VOLUME, M_SHOW_WEB_PAGE, M_SHOW_ACHIEVEMENTS, M_HIDE_ACHIEVEMENTS,
    M_FIXED_SIZE_SCALE, M_CLOSE_DIALOG, M_SHOW_ADS, M_HIDE_ADS, M_SHUTDOWN,
    M_IAP_ENABLED, M_REQUEST_PURCHASE, M_RESTORE_PURCHASES, M_TOAST,
    M_SET_ACHIEVEMENT, M_CLEAR_ACHIEVEMENTS, M_MEDIA_SCAN, M_SWAP, M_SURFACE_CREATED,
    M_TOGGLE_SENSORS, M_NETWORK_STATUS, M_WINDOW_FOCUS, M_OUT_OF_MEMORY,
    M_LOAD_SOUND, M_UNLOAD_SOUND, M_PLAY_SOUND, M_STOP_SOUND, M_SET_VOLUME,
};

NameToMethodID nameToMethodId[] = {
    { M_MAKE_CURRENT, "JavaCallback_MakeCurrent", METHOD_TYPE_BOOLEAN },
    { M_UNMAKE_CURRENT, "JavaCallback_UnmakeCurrent", METHOD_TYPE_BOOLEAN },
    { M_INIT_EGL, "JavaCallback_InitEGL", METHOD_TYPE_OBJECT },
    { M_HAS_VALUE, "JavaCallback_HasAppLocalValue", METHOD_TYPE_BOOLEAN },
    { M_GET_VALUE, "JavaCallback_GetAppLocalValue", METHOD_TYPE_OBJECT },
    { M_SET_VALUE, "JavaCallback_SetAppLocalValue", METHOD_TYPE_VOID },
    { M_PLAY_SONG, "JavaCallback_PlaySong", METHOD_TYPE_VOID },
    { M_STOP_SONG, "JavaCallback_StopSong", METHOD_TYPE_VOID },
    { M_START_VIDEO, "JavaCallback_StartVideo", METHOD_TYPE_VOID },
    { M_STOP_VIDEO, "JavaCallback_StopVideo", METHOD_TYPE_VOID },
    { M_IS_MOVIE_PLAYING, "JavaCallback_IsMoviePlaying", METHOD_TYPE_BOOLEAN },
    { M_SHOW_KEYBOARD, "JavaCallback_ShowKeyBoard", METHOD_TYPE_VOID },
    { M_HIDE_KEYBOARD, "JavaCallback_HideKeyBoard", METHOD_TYPE_VOID },
    { M_HIDE_SPLASH, "JavaCallback_HideSplash", METHOD_TYPE_VOID },
    { M_SHOW_SPLASH, "JavaCallback_ShowSplash", METHOD_TYPE_VOID },
    { M_MUSIC_VOLUME, "JavaCallback_UpdateMusicVolume", METHOD_TYPE_VOID },
    { M_EFFECT_VOLUME, "JavaCallback_UpdateEffectVolume", METHOD_TYPE_VOID },
    { M_SHOW_WEB_PAGE, "JavaCallback_ShowWebPage", METHOD_TYPE_VOID },
    { M_SHOW_ACHIEVEMENTS, "JavaCallback_ShowAchievements", METHOD_TYPE_VOID },
    { M_HIDE_ACHIEVEMENTS, "JavaCallback_HideAchievements", METHOD_TYPE_VOID },
    { M_FIXED_SIZE_SCALE, "JavaCallback_SetFixedSizeScale", METHOD_TYPE_VOID },
    { M_CLOSE_DIALOG, "JavaCallback_ShowCloseDialog", METHOD_TYPE_VOID },
    { M_SHOW_ADS, "JavaCallback_ShowAds", METHOD_TYPE_VOID },
    { M_HIDE_ADS, "JavaCallback_HideAds", METHOD_TYPE_VOID },
    { M_SHUTDOWN, "JavaCallback_ShutDownApp", METHOD_TYPE_VOID },
    { M_IAP_ENABLED, "JavaCallback_bIsInAppPurchasingEnabled", METHOD_TYPE_BOOLEAN },
    { M_REQUEST_PURCHASE, "JavaCallback_RequestPurchase", METHOD_TYPE_BOOLEAN },
    { M_RESTORE_PURCHASES, "JavaCallback_RestorePurchases", METHOD_TYPE_BOOLEAN },
    { M_TOAST, "JavaCallback_ShowToast", METHOD_TYPE_VOID },
    { M_SET_ACHIEVEMENT, "JavaCallback_SetAchievementInfo", METHOD_TYPE_VOID },
    { M_CLEAR_ACHIEVEMENTS, "JavaCallback_ClearAchievementInfo", METHOD_TYPE_VOID },
    { M_MEDIA_SCAN, "JavaCallback_StartMediaScan", METHOD_TYPE_VOID },
    { M_SWAP, "JavaCallback_Swap", METHOD_TYPE_BOOLEAN },
    { M_SURFACE_CREATED, "JavaCallback_bIsSurfaceCreated", METHOD_TYPE_BOOLEAN },
    { M_TOGGLE_SENSORS, "JavaCallback_ToggleSensors", METHOD_TYPE_VOID },
    { M_NETWORK_STATUS, "JavaCallback_vUpdateNetworkStatus", METHOD_TYPE_VOID },
    { M_WINDOW_FOCUS, "JavaCallback_bIsUE3WindowInFocus", METHOD_TYPE_BOOLEAN },
    { M_OUT_OF_MEMORY, "JavaCallback_OutOfMemory", METHOD_TYPE_VOID },
    { M_LOAD_SOUND, "JavaCallback_LoadSoundFile", METHOD_TYPE_INT },
    { M_UNLOAD_SOUND, "JavaCallback_UnloadSoundID", METHOD_TYPE_VOID },
    { M_PLAY_SOUND, "JavaCallback_PlaySound", METHOD_TYPE_INT },
    { M_STOP_SOUND, "JavaCallback_StopSound", METHOD_TYPE_VOID },
    { M_SET_VOLUME, "JavaCallback_SetVolume", METHOD_TYPE_VOID },
};

MethodsBoolean methodsBoolean[] = {
    { M_MAKE_CURRENT, ret_true },
    { M_UNMAKE_CURRENT, ret_true },
    { M_HAS_VALUE, HasAppLocalValue },
    { M_IS_MOVIE_PLAYING, ret_false },
    { M_IAP_ENABLED, IAPEnabled },
    { M_REQUEST_PURCHASE, RequestPurchase },
    { M_RESTORE_PURCHASES, ret_false },
    { M_SWAP, Swap },
    { M_SURFACE_CREATED, ret_true },
    { M_WINDOW_FOCUS, ret_true },
};

MethodsObject methodsObject[] = {
    { M_INIT_EGL, InitEGL },
    { M_GET_VALUE, GetAppLocalValue },
};

MethodsInt methodsInt[] = {
    { M_LOAD_SOUND, LoadSoundFile },
    { M_PLAY_SOUND, PlaySound },
};

MethodsVoid methodsVoid[] = {
    { M_SET_VALUE, SetAppLocalValue },
    { M_PLAY_SONG, PlaySong },
    { M_STOP_SONG, StopSong },
    { M_START_VIDEO, StartVideo },
    { M_STOP_VIDEO, nop },
    { M_SHOW_KEYBOARD, ShowKeyBoard },
    { M_HIDE_KEYBOARD, nop },
    { M_HIDE_SPLASH, nop },
    { M_SHOW_SPLASH, nop },
    { M_MUSIC_VOLUME, UpdateMusicVolume },
    { M_EFFECT_VOLUME, UpdateEffectVolume },
    { M_SHOW_WEB_PAGE, ShowWebPage },
    { M_SHOW_ACHIEVEMENTS, nop },
    { M_HIDE_ACHIEVEMENTS, nop },
    { M_FIXED_SIZE_SCALE, nop },
    { M_CLOSE_DIALOG, nop },
    { M_SHOW_ADS, nop },
    { M_HIDE_ADS, nop },
    { M_SHUTDOWN, ShutDownApp },
    { M_TOAST, ShowToast },
    { M_SET_ACHIEVEMENT, nop },
    { M_CLEAR_ACHIEVEMENTS, nop },
    { M_MEDIA_SCAN, nop },
    { M_TOGGLE_SENSORS, ToggleSensors },
    { M_NETWORK_STATUS, nop },
    { M_OUT_OF_MEMORY, OutOfMemory },
    { M_UNLOAD_SOUND, nop },
    { M_STOP_SOUND, StopSound },
    { M_SET_VOLUME, SetVolume },
};

MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsLong methodsLong[] = {};
MethodsShort methodsShort[] = {};

NameToFieldID nameToFieldId[] = {
    { F_VALID, "validConfig", FIELD_TYPE_INT },
    { F_SAMPLES, "sampleBuffers", FIELD_TYPE_INT },
    { F_RED, "redSize", FIELD_TYPE_INT },
    { F_GREEN, "greenSize", FIELD_TYPE_INT },
    { F_BLUE, "blueSize", FIELD_TYPE_INT },
    { F_ALPHA, "alphaSize", FIELD_TYPE_INT },
    { F_DEPTH, "depthSize", FIELD_TYPE_INT },
    { F_STENCIL, "stencilSize", FIELD_TYPE_INT },
    { F_ACTION_DOWN, "ACTION_DOWN", FIELD_TYPE_INT },
    { F_ACTION_UP, "ACTION_UP", FIELD_TYPE_INT },
    { F_ACTION_POINTER_DOWN, "ACTION_POINTER_DOWN", FIELD_TYPE_INT },
    { F_ACTION_POINTER_UP, "ACTION_POINTER_UP", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {};
FieldsObject fieldsObject[] = {};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
