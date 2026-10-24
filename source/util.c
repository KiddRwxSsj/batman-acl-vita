#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/common_dialog.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

extern int gfx_ready;

void fatal_error(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    LOG("FATAL: %s", msg);

    if (!gfx_ready)
        vglInit(0);

    SceMsgDialogUserMessageParam user = {0};
    user.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    user.msg = (SceChar8 *)msg;

    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &user;

    if (sceMsgDialogInit(&param) >= 0) {
        while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
            glClear(GL_COLOR_BUFFER_BIT);
            vglSwapBuffers(GL_TRUE);
        }
        sceMsgDialogTerm();
    }
    sceKernelExitProcess(0);
    while (1);
}

bool file_exists(const char *path) {
    SceIoStat st;
    return sceIoGetstat(path, &st) >= 0;
}

bool file_load(const char *path, void **buf, size_t *size) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return false;
    size_t len = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    void *data = malloc(len);
    if (!data || sceIoRead(fd, data, len) != (int)len) {
        free(data);
        sceIoClose(fd);
        return false;
    }
    sceIoClose(fd);
    *buf = data;
    *size = len;
    return true;
}

bool file_save(const char *path, const void *buf, size_t size) {
    SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return false;
    bool ok = sceIoWrite(fd, buf, size) == (int)size;
    sceIoClose(fd);
    return ok;
}

void mkpath(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    for (char *p = strchr(tmp, '/'); p; p = strchr(p + 1, '/')) {
        *p = 0;
        sceIoMkdir(tmp, 0777);
        *p = '/';
    }
}

uint64_t hash64(const void *data, size_t len) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ULL;
    while (len--)
        h = (h ^ *p++) * 0x100000001b3ULL;
    return h;
}

uint64_t uptime_ms(void) {
    return sceKernelGetProcessTimeWide() / 1000;
}

volatile unsigned progress_ticks, swap_ticks;

#define LOG_PATH DATA_PATH "log.txt"
#define LOG_PREV_PATH DATA_PATH "log_prev.txt"
#define RING_SIZE 65536

static int log_ready;

// last 64k of log text. a vita coredump keeps every thread's stack but none of the bss, so it lives
// on the log thread's stack where a dump always has it (tools/scripts/dump_log.py pulls it out)
struct log_ring {
    char magic[16];
    volatile unsigned pos;
    char text[RING_SIZE];
};
static struct log_ring *ring;
static unsigned log_flushed;
static SceKernelLwMutexWork log_lock;

// file io takes ms on ux0, so it happens here instead of on whatever engine thread logged.
// reopened every batch so the file is complete even if the game gets killed (vita3k never
// flushed a kept-open fd)
static int log_thread(SceSize args, void *argp) {
    struct log_ring r;
    memset(&r, 0, sizeof(r));
    strcpy(r.magic, "acl log ring v1");
    ring = &r;
    while (1) {
        unsigned end = r.pos;
        if (end == log_flushed) {
            sceKernelDelayThread(50 * 1000);
            continue;
        }
        if (end - log_flushed > RING_SIZE)
            log_flushed = end - RING_SIZE;
        SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_APPEND, 0777);
        while (log_flushed != end) {
            unsigned off = log_flushed % RING_SIZE;
            unsigned n = end - log_flushed;
            if (n > RING_SIZE - off)
                n = RING_SIZE - off;
            if (fd >= 0)
                sceIoWrite(fd, r.text + off, n);
            log_flushed += n;
        }
        if (fd >= 0)
            sceIoClose(fd);
    }
    return 0;
}

void log_open(void) {
    // keep the last session's log, a tester relaunching after a crash would wipe it otherwise
    sceIoRemove(LOG_PREV_PATH);
    sceIoRename(LOG_PATH, LOG_PREV_PATH);
    SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
        return;
    sceIoClose(fd);
    sceKernelCreateLwMutex(&log_lock, "log", 0, 0, NULL);
    SceUID t = sceKernelCreateThread("log", log_thread, 160, sizeof(struct log_ring) + 0x4000, 0, 0, NULL);
    if (sceKernelStartThread(t, 0, NULL) < 0)
        return;
    while (!ring)
        sceKernelDelayThread(1000);
    log_flushed = 0;
    log_ready = 1;
}

void log_write(const char *s, int len) {
    sceClibPrintf("%.*s", len, s);
    if (!log_ready)
        return;
    sceKernelLockLwMutex(&log_lock, 1, NULL);
    unsigned pos = ring->pos;
    for (int i = 0; i < len; i++)
        ring->text[(pos + i) % RING_SIZE] = s[i];
    ring->pos = pos + len;
    sceKernelUnlockLwMutex(&log_lock, 1);
}

void log_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len > (int)sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    if (len > 0)
        log_write(buf, len);
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
