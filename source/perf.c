#ifdef PERF_TRACE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <vitaGL.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <kubridge.h>
#include <so_util/so_util.h>
#include "perf.h"
#include "util.h"
#include "loader.h"
#include "gfx.h"

#define MAX_THREADS 32

static struct {
    SceUID id;
    char name[12];
    uint64_t last, last_load;
} threads[MAX_THREADS];
static int num_threads;

static unsigned draws, tris;
static uint64_t swap_us, frame_max_us, last_frame;
static int frames, hitches;
static uint64_t window_start;

void perf_thread(SceUID id, const char *name) {
    if (num_threads == MAX_THREADS)
        return;
    threads[num_threads].id = id;
    snprintf(threads[num_threads].name, sizeof(threads[0].name), "%s", name);
    num_threads++;
}

// gl side: uploads and compiles that could stall a frame, plus the cpu cost of the draws themselves
enum { G_TEX, G_BUF, G_SHADER, G_DRAW, G_COUNT };
static const char *gl_names[G_COUNT] = { "tex", "buf", "shader", "draw" };
static uint64_t gl_us[G_COUNT], gl_bytes[G_COUNT];
static unsigned gl_calls[G_COUNT];

#define GL_BEGIN uint64_t t0 = sceKernelGetProcessTimeWide()
static volatile uint64_t gl_total_us[G_COUNT];
static volatile unsigned gl_total_calls[G_COUNT];
#define GL_END(i, n) { uint64_t d = sceKernelGetProcessTimeWide() - t0; gl_us[i] += d; gl_calls[i]++; gl_bytes[i] += (n); gl_total_us[i] += d; gl_total_calls[i]++; }

static void p_glDrawElements(GLenum m, GLsizei c, GLenum t, const void *i) {
    GL_BEGIN;
    draws++;
    tris += c / 3;
    glDrawElements(m, c, t, i);
    GL_END(G_DRAW, 0);
}

static void p_glDrawArrays(GLenum m, GLint f, GLsizei c) {
    GL_BEGIN;
    draws++;
    tris += c / 3;
    glDrawArrays(m, f, c);
    GL_END(G_DRAW, 0);
}

static void p_glTexImage2D(GLenum tg, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt, GLenum t, const void *d) {
    GL_BEGIN;
    glTexImage2D(tg, l, ifmt, w, h, b, fmt, t, d);
    GL_END(G_TEX, w * h * 4);
}

static void p_glTexSubImage2D(GLenum tg, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum t, const void *d) {
    GL_BEGIN;
    glTexSubImage2D(tg, l, x, y, w, h, fmt, t, d);
    GL_END(G_TEX, w * h * 4);
}

static void p_glCompressedTexImage2D(GLenum tg, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei size, const void *d) {
    GL_BEGIN;
    glCompressedTexImage2D(tg, l, ifmt, w, h, b, size, d);
    GL_END(G_TEX, size);
}

static void p_glBufferData(GLenum tg, GLsizeiptr size, const void *d, GLenum usage) {
    GL_BEGIN;
    glBufferData(tg, size, d, usage);
    GL_END(G_BUF, size);
}

static void p_glBufferSubData(GLenum tg, GLintptr off, GLsizeiptr size, const void *d) {
    GL_BEGIN;
    glBufferSubData(tg, off, size, d);
    GL_END(G_BUF, size);
}

static void p_glCompileShader(GLuint sh) {
    GL_BEGIN;
    glCompileShader(sh);
    GL_END(G_SHADER, 0);
}

static void p_glLinkProgram(GLuint prog) {
    GL_BEGIN;
    gl_link_program(prog);
    GL_END(G_SHADER, 0);
}

// loading: file io and zlib, logged once a second by perf_load_thread while anything moves
#include "zfast.h"
#include "bionic.h"
enum { L_FOPEN, L_FREAD, L_FSEEK, L_READ, L_LSEEK, L_MMAP, L_INFLATE, L_UNCOMP, L_COUNT };
static const char *load_names[L_COUNT] = { "fopen", "fread", "fseek", "read", "lseek", "mmap", "inflate", "uncompress" };
static volatile uint64_t load_us[L_COUNT], load_bytes[L_COUNT];
static volatile unsigned load_calls[L_COUNT];

#define LD_BEGIN uint64_t t0 = sceKernelGetProcessTimeWide()
#define LD_END(i, n) load_us[i] += sceKernelGetProcessTimeWide() - t0; load_calls[i]++; load_bytes[i] += (n)

static FILE *p_fopen(const char *path, const char *mode) { LD_BEGIN; FILE *f = fopen_b(path, mode); LD_END(L_FOPEN, 0); return f; }
static size_t p_fread(void *b, size_t sz, size_t n, FILE *f) { LD_BEGIN; size_t r = fread_b(b, sz, n, f); LD_END(L_FREAD, r * sz); return r; }
static int p_fseek(FILE *f, long off, int wh) { LD_BEGIN; int r = fseek_b(f, off, wh); LD_END(L_FSEEK, 0); return r; }
static int p_read(int fd, void *b, size_t n) { LD_BEGIN; int r = read(fd, b, n); LD_END(L_READ, r > 0 ? r : 0); return r; }
static off_t p_lseek(int fd, off_t off, int wh) { LD_BEGIN; off_t r = lseek(fd, off, wh); LD_END(L_LSEEK, 0); return r; }
static void *p_mmap(void *a, size_t len, int prot, int fl, int fd, off_t off) { LD_BEGIN; void *r = mmap_from((uintptr_t)__builtin_return_address(0), a, len, prot, fl, fd, off); LD_END(L_MMAP, len); return r; }
static int p_inflate(z_streamp z, int flush) { uLong out = z->total_out; LD_BEGIN; int r = zf_inflate(z, flush); LD_END(L_INFLATE, z->total_out - out); return r; }
static int p_uncompress(Bytef *d, uLongf *dl, const Bytef *src, uLong sl) { LD_BEGIN; int r = zf_uncompress(d, dl, src, sl); LD_END(L_UNCOMP, *dl); return r; }

// call counts only (plus slept/waited time), to see what a cpu-bound stretch is up to
#include <pthread.h>
#include <sched.h>
#include <malloc.h>
#include <psp2/kernel/sysmem.h>
#include <sys/time.h>
#include <psp2/kernel/clib.h>
enum { C_MALLOC, C_FREE, C_REALLOC, C_MEMCPY, C_MEMSET, C_STRCMP, C_STRLEN, C_PRINTF, C_GETTIME, C_USLEEP, C_YIELD, C_CONDWAIT, C_MUTEX, C_COUNT };
static const char *cnt_names[C_COUNT] = { "malloc", "free", "realloc", "memcpy", "memset", "strcmp", "strlen", "printf", "gettime", "usleep", "yield", "condwait", "mutex" };
static volatile unsigned cnt_calls[C_COUNT];
static volatile uint64_t cnt_extra[C_COUNT]; // bytes for mem*, microseconds for the waits

static void *c_malloc(size_t n) { cnt_calls[C_MALLOC]++; cnt_extra[C_MALLOC] += n; return malloc_b(n); }
static void c_free(void *p) { cnt_calls[C_FREE]++; free_b(p); }
static void *c_realloc(void *p, size_t n) { cnt_calls[C_REALLOC]++; return realloc_b(p, n); }
static void *c_memcpy(void *d, const void *s, size_t n) { cnt_calls[C_MEMCPY]++; cnt_extra[C_MEMCPY] += n; return sceClibMemcpy(d, s, n); }
static void *c_memset(void *d, int v, size_t n) { cnt_calls[C_MEMSET]++; cnt_extra[C_MEMSET] += n; return sceClibMemset(d, v, n); }
static int c_strcmp(const char *a, const char *b) { cnt_calls[C_STRCMP]++; return strcmp(a, b); }
static size_t c_strlen(const char *a) { cnt_calls[C_STRLEN]++; return strlen(a); }
static int c_vsnprintf(char *b, size_t n, const char *f, va_list ap) { cnt_calls[C_PRINTF]++; return vsnprintf(b, n, f, ap); }
static int c_gettimeofday(struct timeval *tv, void *tz) { cnt_calls[C_GETTIME]++; return gettimeofday(tv, tz); }
static int c_usleep(useconds_t us) { cnt_calls[C_USLEEP]++; cnt_extra[C_USLEEP] += us; return usleep(us); }
static int c_sched_yield(void) { cnt_calls[C_YIELD]++; return sched_yield(); }
static int c_cond_wait(int *c, int *m) { LD_BEGIN; int r = pthread_cond_wait_b(c, m); cnt_calls[C_CONDWAIT]++; cnt_extra[C_CONDWAIT] += sceKernelGetProcessTimeWide() - t0; return r; }
static int c_cond_timedwait(int *c, int *m, const struct timespec *t) { LD_BEGIN; int r = pthread_cond_timedwait_b(c, m, t); cnt_calls[C_CONDWAIT]++; cnt_extra[C_CONDWAIT] += sceKernelGetProcessTimeWide() - t0; return r; }
static int c_mutex_lock(int *m) { cnt_calls[C_MUTEX]++; return pthread_mutex_lock_b(m); }

static void ip_dump(void);
static int perf_load_thread(SceSize args, void *argp) {
    uint64_t last = sceKernelGetProcessTimeWide();
    unsigned last_calls[L_COUNT] = { 0 };
    uint64_t last_us[L_COUNT] = { 0 }, last_bytes[L_COUNT] = { 0 };
    for (;;) {
        sceKernelDelayThread(1000000);
        uint64_t now = sceKernelGetProcessTimeWide(), win = now - last;
        last = now;
        char line[1024];
        int len = snprintf(line, sizeof(line), "load:");
        for (int i = 0; i < L_COUNT; i++) {
            unsigned c = load_calls[i] - last_calls[i];
            uint64_t us = load_us[i] - last_us[i], b = load_bytes[i] - last_bytes[i];
            last_calls[i] += c;
            last_us[i] += us;
            last_bytes[i] += b;
            if (c) {
                len += snprintf(line + len, sizeof(line) - len, " %s %ums/%u/%ukB", load_names[i], (unsigned)(us / 1000), c, (unsigned)(b >> 10));
            }
        }
        // libdeflate decodes (time/count/out kB), copies alive, zlib fallbacks, restarts served from a copy
        static zf_stats_t lz;
        zf_stats_t zs = zf_stats;
        if (zs.decodes != lz.decodes || zs.fallbacks != lz.fallbacks || zs.reused != lz.reused || zs.live != lz.live)
            len += snprintf(line + len, sizeof(line) - len, " zf %ums/%u/%ukB live %ukB fb %u re %u",
                (unsigned)((zs.us - lz.us) / 1000), zs.decodes - lz.decodes, (unsigned)((zs.bytes - lz.bytes) >> 10),
                zs.live >> 10, zs.fallbacks - lz.fallbacks, zs.reused - lz.reused);
        lz = zs;
        {
            struct mallinfo mi = mallinfo();
            SceKernelFreeMemorySizeInfo fm = { sizeof(fm) };
            sceKernelGetFreeMemorySize(&fm);
            len += snprintf(line + len, sizeof(line) - len, " heap %u/%uMB free %uMB sys %u/%uMB vmaps %d big %u/%uMB", (unsigned)(mi.uordblks >> 20),
                (unsigned)(mi.arena >> 20), (unsigned)(heap_free() >> 20), fm.size_user >> 20, fm.size_phycont >> 20, vmaps_open, big_count, big_bytes >> 20);
        }
        static unsigned last_cnt[C_COUNT], last_gl[G_COUNT];
        static uint64_t last_cnt_extra[C_COUNT], last_gl_us[G_COUNT];
        for (int i = 0; i < G_COUNT; i++) {
            unsigned c = gl_total_calls[i] - last_gl[i];
            uint64_t us = gl_total_us[i] - last_gl_us[i];
            last_gl[i] += c;
            last_gl_us[i] += us;
            if (c && i != G_DRAW)
                len += snprintf(line + len, sizeof(line) - len, " %s %ums/%u", gl_names[i], (unsigned)(us / 1000), c);
        }
        len += snprintf(line + len, sizeof(line) - len, " #");
        for (int i = 0; i < C_COUNT; i++) {
            unsigned c = cnt_calls[i] - last_cnt[i];
            uint64_t x = cnt_extra[i] - last_cnt_extra[i];
            last_cnt[i] += c;
            last_cnt_extra[i] += x;
            if (c >= 100 || (c && (i == C_USLEEP || i == C_CONDWAIT)))
                len += snprintf(line + len, sizeof(line) - len, x ? " %s %u/%u" : " %s %u", cnt_names[i], c, (unsigned)(i >= C_USLEEP ? x / 1000 : x >> 10));
        }
        len += snprintf(line + len, sizeof(line) - len, " |");
        for (int i = 0; i < num_threads && len < (int)sizeof(line) - 24; i++) {
            SceKernelThreadInfo info;
            info.size = sizeof(info);
            if (sceKernelGetThreadInfo(threads[i].id, &info) < 0)
                continue;
            unsigned pct = (unsigned)((info.runClocks - threads[i].last_load) * 100 / win);
            threads[i].last_load = info.runClocks;
            if (pct)
                len += snprintf(line + len, sizeof(line) - len, " %s %u%%", threads[i].name, pct);
        }
        LOG("%s", line);
        ip_dump();
    }
    return 0;
}

void perf_load_start(void) {
    SceUID t = sceKernelCreateThread("perfload", perf_load_thread, 160, 0x4000, 0, 0, NULL);
    if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
    else
        LOG("perfload thread: %x", t);
}

so_default_dynlib perf_imports[] = {
    { "glDrawElements", (uintptr_t)&p_glDrawElements },
    { "glDrawArrays", (uintptr_t)&p_glDrawArrays },
    { "glTexImage2D", (uintptr_t)&p_glTexImage2D },
    { "glTexSubImage2D", (uintptr_t)&p_glTexSubImage2D },
    { "glCompressedTexImage2D", (uintptr_t)&p_glCompressedTexImage2D },
    { "glBufferData", (uintptr_t)&p_glBufferData },
    { "glBufferSubData", (uintptr_t)&p_glBufferSubData },
    { "glCompileShader", (uintptr_t)&p_glCompileShader },
    { "glLinkProgram", (uintptr_t)&p_glLinkProgram },
    { "fopen", (uintptr_t)&p_fopen },
    { "fread", (uintptr_t)&p_fread },
    { "fseek", (uintptr_t)&p_fseek },
    { "read", (uintptr_t)&p_read },
    { "lseek", (uintptr_t)&p_lseek },
    { "mmap", (uintptr_t)&p_mmap },
    { "inflate", (uintptr_t)&p_inflate },
    { "uncompress", (uintptr_t)&p_uncompress },
    { "malloc", (uintptr_t)&c_malloc },
    { "free", (uintptr_t)&c_free },
    { "realloc", (uintptr_t)&c_realloc },
    { "memcpy", (uintptr_t)&c_memcpy },
    { "memset", (uintptr_t)&c_memset },
    { "strcmp", (uintptr_t)&c_strcmp },
    { "strlen", (uintptr_t)&c_strlen },
    { "vsnprintf", (uintptr_t)&c_vsnprintf },
    { "gettimeofday", (uintptr_t)&c_gettimeofday },
    { "usleep", (uintptr_t)&c_usleep },
    { "sched_yield", (uintptr_t)&c_sched_yield },
    { "pthread_cond_wait", (uintptr_t)&c_cond_wait },
    { "pthread_cond_timedwait", (uintptr_t)&c_cond_timedwait },
    { "pthread_mutex_lock", (uintptr_t)&c_mutex_lock },
};
int perf_imports_size = sizeof(perf_imports);


// UWorld::Tick bisect. Every GOT slot it calls through gets a wrapper (plain data write, no code
// patching). Calls whose return address is inside UWorld::Tick are checkpoints: time spent in the
// call per call site, plus time between two checkpoints per pair of sites, which covers the inline
// code and the vtable calls in between. Everything else passes straight through
typedef struct { unsigned key; uint64_t us; unsigned n; } bucket;
#define WT_SITES 96
#define WT_GAPS 160
#define WT_CLS 64
static bucket wt_site[WT_SITES], wt_gap[WT_GAPS], wt_cls[WT_CLS];
static int hint_site, hint_gap, hint_cls;
static uintptr_t wt_lo;
static unsigned wt_len, wt_prev;
static volatile int wt_on;
static uint64_t wt_last;

static void bucket_add(bucket *t, int size, int *hint, unsigned key, uint64_t us) {
    int i = *hint;
    if (t[i].key != key || !t[i].n) {
        for (i = 0; i < size && t[i].n && t[i].key != key; i++)
            ;
        if (i == size)
            return;
        *hint = i;
    }
    t[i].key = key;
    t[i].us += us;
    t[i].n++;
}

static uint64_t wt_enter(uintptr_t lr) {
    uint64_t now = sceKernelGetProcessTimeWide();
    bucket_add(wt_gap, WT_GAPS, &hint_gap, wt_prev << 16 | (lr - wt_lo), now - wt_last);
    return now;
}

static void wt_leave(uintptr_t lr, uint64_t t0) {
    uint64_t now = sceKernelGetProcessTimeWide();
    bucket_add(wt_site, WT_SITES, &hint_site, lr - wt_lo, now - t0);
    wt_prev = lr - wt_lo;
    wt_last = now;
}

// per class: c = ConditionalUpdateComponents from the world loop (actor vtable), a/d/x =
// ConditionalAttach/Detach/UpdateTransform during the world tick (component vtable)
static void cls_add(char kind, void *obj, uint64_t us) {
    uintptr_t vt = *(uintptr_t *)obj - ue3_mod.load_addr;
    if (vt < 0x2000000)
        bucket_add(wt_cls, WT_CLS, &hint_cls, (unsigned)kind << 25 | vt, us);
}

#define WT_GOT 56
typedef uint32_t (*fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static fn8 got_orig[WT_GOT];

// up to 8 words passed through, extra stack words are just whatever the caller had there
#define GW(i) static uint32_t gw##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h) {     uintptr_t lr = (uintptr_t)__builtin_return_address(0);     if (!wt_on || lr - wt_lo >= wt_len)         return got_orig[i](a, b, c, d, e, f, g, h);     uint64_t t0 = wt_enter(lr);     uint32_t r = got_orig[i](a, b, c, d, e, f, g, h);     wt_leave(lr, t0);     return r; }
GW(4) GW(5) GW(6) GW(7) GW(8) GW(9) GW(10) GW(11) GW(12) GW(13) GW(14) GW(15) GW(16) GW(17) GW(18) GW(19) GW(20) GW(21) GW(22) GW(23) GW(24) GW(25) GW(26) GW(27) GW(28) GW(29) GW(30) GW(31) GW(32) GW(33) GW(34) GW(35) GW(36) GW(37) GW(38) GW(39) GW(40) GW(41) GW(42) GW(43) GW(44) GW(45) GW(46) GW(47) GW(48) GW(49) GW(50) GW(51) GW(52) GW(53) GW(54) GW(55)

static uint32_t gw_cuc(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h) {
    uintptr_t lr = (uintptr_t)__builtin_return_address(0);
    if (!wt_on || lr - wt_lo >= wt_len)
        return got_orig[0](a, b, c, d, e, f, g, h);
    uint64_t t0 = wt_enter(lr);
    uint32_t r = got_orig[0](a, b, c, d, e, f, g, h);
    wt_leave(lr, t0);
    cls_add('c', (void *)a, wt_last - t0);
    return r;
}

#define GC(i, kind) static uint32_t gw##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h) {     if (!wt_on)         return got_orig[i](a, b, c, d, e, f, g, h);     uint64_t t0 = sceKernelGetProcessTimeWide();     uint32_t r = got_orig[i](a, b, c, d, e, f, g, h);     cls_add(kind, (void *)a, sceKernelGetProcessTimeWide() - t0);     return r; }
GC(1, 'a') GC(2, 'd') GC(3, 'x')

// slot offsets from the load base (0x98000000 on hw, somewhere else under vita3k), checked against the symbol before swapping
static const struct { uint32_t slot; const char *sym; } wt_got[WT_GOT] = {
    { 0x1383350, "_ZN6AActor27ConditionalUpdateComponentsEj" },
    { 0x1381c28, "_ZN15UActorComponent17ConditionalAttachEP15FSceneInterfaceP6AActorRK7FMatrix" },
    { 0x137f9e8, "_ZN15UActorComponent17ConditionalDetachEj" },
    { 0x137daec, "_ZN15UActorComponent26ConditionalUpdateTransformERK7FMatrix" },
    { 0x13817b8, "_ZN6UWorld13TickNetServerEf" },
    { 0x13825b4, "_ZN14TFieldIteratorI14UFloatPropertyE13IterateToNextEv" },
    { 0x1382b0c, "_ZN7UObject14CollectGarbageEyj" },
    { 0x1383944, "_Z10TickActorsIN17FDeferredTickList31FActorEffectsUpdateWorkIteratorEEvP6UWorldf10ELevelTickRS0_" },
    { 0x1384a88, "_ZN6UWorld16TickDemoPlaybackEf" },
    { 0x13855a4, "_ZN22FTickableActorIteratorC1Ev" },
    { 0x138656c, "_ZNK6UWorld12HasBegunPlayEv" },
    { 0x13866a8, "_ZN15UActorComponent21BeginDeferredReattachEv" },
    { 0x1386714, "_ZN10UNetDriver22ClearLocalVoicePacketsEv" },
    { 0x1386964, "_Z10TickActorsIN17FDeferredTickList28FActorPostUpdateWorkIteratorEEvP6UWorldf10ELevelTickRS0_" },
    { 0x1371ec8, "_ZN17FDeferredTickList5ResetEv" },
    { 0x13876f0, "_ZN6TArrayI7FString17FDefaultAllocatorE5EmptyEi" },
    { 0x1387dc4, "_Z10TickActorsIN17FDeferredTickList29FActorDuringAsyncWorkIteratorEEvP6UWorldf10ELevelTickRS0_" },
    { 0x138a678, "_Z20WaitPhysCompartmentsP12FRBPhysScene" },
    { 0x138abf0, "_ZN13FStringNoInitaSEPKc" },
    { 0x138b400, "_ZN6TArrayIc17FDefaultAllocatorED2Ev" },
    { 0x138cde4, "_ZNK6UWorld18GetFirstControllerEv" },
    { 0x138e0cc, "_Z13LocalizeErrorPKcS0_S0_" },
    { 0x138ef44, "_ZN12UPlayerInput16eventPlayerInputEf" },
    { 0x138f0bc, "_ZN6UWorld17TickHostMigrationEf" },
    { 0x138fcc8, "_ZN6UWorld13TickNetClientEf" },
    { 0x1390918, "_ZN6UWorld17VerifyPackageInfoER12FPackageInfo" },
    { 0x1391f28, "_ZN7UObject19ProcessAsyncLoadingEjf5FName" },
    { 0x139291c, "_ZN20FParticleDataManager17UpdateDynamicDataEv" },
    { 0x13929a0, "_ZN6UWorld16WaitForAsyncWorkEv" },
    { 0x13936b4, "_ZN6UWorld28ProcessLevelStreamingVolumesEP7FVector" },
    { 0x1372b94, "_Z4CastI11UGameEngineEPT_P7UObject" },
    { 0x1372be4, "_ZN6TArrayIN19ULineBatchComponent5FLineE17FDefaultAllocatorE5EmptyEi" },
    { 0x1394100, "_ZN15UActorComponent15UpdateComponentEP15FSceneInterfaceP6AActorRK7FMatrixj" },
    { 0x1394410, "_ZN6UWorld8IsPausedEv" },
    { 0x13944c4, "_Z25DeferredRBResourceCleanupP12FRBPhysScenej" },
    { 0x1394750, "_ZN22FTickableActorIteratorppEv" },
    { 0x13962c8, "_Z10TickActorsIN17FDeferredTickList20FGlobalActorIteratorEEvP6UWorldf10ELevelTickRS0_" },
    { 0x1396570, "_Z22TickDeferredComponentsIN17FDeferredTickList33FComponentDuringAsyncWorkIteratorEEvfRS0_" },
    { 0x137373c, "_ZN20FParticleDataManager5ClearEv" },
    { 0x1370bc0, "_ZN7ACamera17eventUpdateCameraEf" },
    { 0x1373c40, "_Z27DispatchRBCollisionNotifiesP12FRBPhysScene" },
    { 0x1373f08, "_ZN17APlayerController21eventConditionalPauseEj" },
    { 0x1375fe4, "_ZN6UWorld13TickAsyncWorkEf" },
    { 0x1376994, "_ZNK6UWorld12GetWorldInfoEj" },
    { 0x1376e4c, "_Z22TickDeferredComponentsIN17FDeferredTickList35FComponentEffectsUpdateWorkIteratorEEvfRS0_" },
    { 0x1377910, "_ZN7UObject23IncrementalPurgeGarbageEjf" },
    { 0x1377c94, "_ZN6UWorld24PerformGarbageCollectionEv" },
    { 0x1377f30, "_ZNK6UWorld10GetNetModeEv" },
    { 0x1379958, "_ZNK6UWorld14GetTimeSecondsEv" },
    { 0x137a368, "_ZN14UDemoRecDriver14UpdateDemoTimeEPff" },
    { 0x137ac74, "_ZN7UObject25IsIncrementalPurgePendingEv" },
    { 0x137afac, "_ZN6UWorld14TickDemoRecordEf" },
    { 0x137e824, "_Z22TickDeferredComponentsIN17FDeferredTickList32FComponentPostUpdateWorkIteratorEEvfRS0_" },
    { 0x137ecd4, "_Z22TickDeferredComponentsIN17FDeferredTickList31FComponentPostAsyncWorkIteratorEEvfRS0_" },
    { 0x137f1f4, "_Z12GInitRunawayv" },
    { 0x137fb48, "_Z10TickActorsIN17FDeferredTickList27FActorPostAsyncWorkIteratorEEvP6UWorldf10ELevelTickRS0_" },
};
static fn8 const gw_fns[WT_GOT] = { gw_cuc, gw1, gw2, gw3, gw4, gw5, gw6, gw7, gw8, gw9, gw10, gw11, gw12, gw13, gw14, gw15, gw16, gw17, gw18, gw19, gw20, gw21, gw22, gw23, gw24, gw25, gw26, gw27, gw28, gw29, gw30, gw31, gw32, gw33, gw34, gw35, gw36, gw37, gw38, gw39, gw40, gw41, gw42, gw43, gw44, gw45, gw46, gw47, gw48, gw49, gw50, gw51, gw52, gw53, gw54, gw55 };

static void wt_hooks(void) {
    wt_lo = ue3_sym("_ZN6UWorld4TickE10ELevelTickf") & ~1;
    wt_len = 3816; // st_size
    int n = 0;
    for (int i = 0; i < WT_GOT; i++) {
        uintptr_t *slot = (uintptr_t *)(ue3_mod.load_addr + wt_got[i].slot);
        uintptr_t want = ue3_sym(wt_got[i].sym);
        if (!want || *slot != want) {
            LOG("wt: got %x has %x, not %s", (unsigned)wt_got[i].slot, (unsigned)*slot, wt_got[i].sym);
            continue;
        }
        got_orig[i] = (fn8)*slot;
        *slot = (uintptr_t)gw_fns[i];
        n++;
    }
    uint64_t t0 = sceKernelGetProcessTimeWide();
    for (int i = 0; i < 1000; i++)
        sceKernelGetProcessTimeWide();
    LOG("wt: %d/%d got slots wrapped, timer %u ns/call", n, WT_GOT, (unsigned)(sceKernelGetProcessTimeWide() - t0));
}

static int site_str(char *s, unsigned site) {
    if (site == 0)
        return sprintf(s, "beg");
    if (site == 0xffff)
        return sprintf(s, "end");
    return sprintf(s, "%x", (unsigned)((wt_lo - ue3_mod.load_addr + site) & ~1));
}

// top entries over min_us per frame, then clears the table
static void wt_dump(const char *name, bucket *t, int size, int frames, unsigned min_us, int kind) {
    char line[1024];
    int len = snprintf(line, sizeof(line), "%s", name), start = len;
    for (int k = 0; k < 16 && len < 900; k++) {
        int best = -1;
        for (int i = 0; i < size && t[i].n; i++)
            if (t[i].us / frames >= min_us && (best < 0 || t[i].us > t[best].us))
                best = i;
        if (best < 0)
            break;
        line[len++] = ' ';
        if (kind == 0) {
            len += site_str(line + len, t[best].key >> 16);
            line[len++] = '>';
            len += site_str(line + len, t[best].key & 0xffff);
        } else if (kind == 1) {
            len += site_str(line + len, t[best].key);
        } else {
            len += sprintf(line + len, "%c%x", t[best].key >> 25, t[best].key & 0x1ffffff);
        }
        len += sprintf(line + len, " %u.%u/%u", (unsigned)(t[best].us / frames / 1000), (unsigned)(t[best].us / frames / 100 % 10), t[best].n);
        t[best].us = 0;
    }
    memset(t, 0, sizeof(bucket) * size);
    if (len > start)
        LOG("%s", line);
}


// engine init bisect, same trick as the world tick one: every got slot PreInit/Init/appInit/
// UGameEngine::Init call through gets a wrapper; calls coming straight from one of them are
// timed per call site (cumulative), plus the time between two such calls within the same owner
#define IP_GOT 65
static const struct { uint32_t slot; const char *sym; } ip_got[IP_GOT] = {
    { 0x13708d4, "_ZN18FAsyncIOSystemBaseC2Ev" },
    { 0x1372214, "_Z14appSetGameNamev" },
    { 0x13727e0, "_ZN7UObject15StaticLoadClassEP6UClassPS_PKcS4_mP11UPackageMap" },
    { 0x1372be8, "_Z14InitGameRBPhysv" },
    { 0x1372e98, "_Z22CreateConsoleVariablesv" },
    { 0x1373120, "_Z10ParseTokenRPKcj" },
    { 0x13738c0, "_Z17appGetLanguageExtv" },
    { 0x1373c04, "_ZN7UObject20StaticAllocateObjectEP6UClassPS_5FNameyS2_P13FOutputDeviceS2_S2_P22FObjectInstancingGraph" },
    { 0x1374368, "_ZN11UGameEngine24InitGameSingletonObjectsEv" },
    { 0x13747e0, "_Z18GetGlobalShaderMap15EShaderPlatform" },
    { 0x1378474, "_ZN24FStreamingManagerTexture26AddTextureStreamingHandlerEP28FStreamingHandlerTextureBase" },
    { 0x1378a14, "_ZN6UClass16GetDefaultObjectEj" },
    { 0x137934c, "_Z7RHIInitj" },
    { 0x13794e4, "_ZN9FViewport23SetGameRenderingEnabledEji" },
    { 0x1379694, "_ZNK6UWorld11GetGameInfoEv" },
    { 0x137b4e4, "_ZN12TSparseArrayIN4TSetIN8TMapBaseI7FStringP16IConsoleVariableLj0E20FDefaultSetAllocatorE5FPairENS6_8KeyFuncsES5_E8FElementE21TSparseArrayAllocatorI17FDefaultAllocator25FDefaultBitArrayAllocatorEEC1Ev" },
    { 0x137be74, "_ZN4FURL13LoadURLConfigEPKcS1_" },
    { 0x137c4c4, "_Z10appPreExitv" },
    { 0x137c9b0, "_ZN27FStreamingManagerCollection19AddStreamingManagerEP21FStreamingManagerBase" },
    { 0x137ca98, "_ZN7USystemC1Ev" },
    { 0x137d0e4, "_ZNK7FStringplEPKc" },
    { 0x137d35c, "_ZN20TConstSetBitIteratorI25FDefaultBitArrayAllocatorEC1ERK9TBitArrayIS0_Ei" },
    { 0x137e360, "_Z7appInitPKcP13FOutputDeviceP20FOutputDeviceConsoleP18FOutputDeviceErrorP16FFeedbackContextP12FFileManagerP22FCallbackEventObserverP20FCallbackQueryDevicePFP15FConfigCacheInivE" },
    { 0x137e438, "_ZN10UGuidCache14CreateInstanceEPKc" },
    { 0x137e7d0, "_Z19appSystemTimeStringv" },
    { 0x1380114, "_ZN10FColorList14CreateColorMapEv" },
    { 0x1380ea4, "_ZN15FSystemSettings10InitializeEj" },
    { 0x13813c8, "_Z10ParseTokenRPKcPcij" },
    { 0x138160c, "_Z12appEngineDirv" },
    { 0x1382410, "_Z16appGameConfigDirv" },
    { 0x1383070, "_ZN4FURLC1ERKS_" },
    { 0x1384c5c, "_ZN7UObject21StaticConstructObjectEP6UClassPS_5FNameyS2_P13FOutputDeviceS2_P22FObjectInstancingGraph" },
    { 0x1386d04, "_Z37InitializeRegistrantsAndRegisterNamesv" },
    { 0x1387334, "_ZN10FIOManagerC1Ev" },
    { 0x13874cc, "_Z10appStrncpyPcPKci" },
    { 0x1387770, "_ZN7UObject19GetTransientPackageEv" },
    { 0x1387974, "_Z21CheckNativeClassSizesv" },
    { 0x1387fb4, "_ZN22FCallbackEventObserverC1Ev" },
    { 0x1389178, "_Z18appPlatformPreInitv" },
    { 0x1389ad0, "_Z15appPlatformInitv" },
    { 0x1389b2c, "_ZN7UEngine4InitEv" },
    { 0x138a48c, "_ZN20FMapPackageFileCacheC2Ev" },
    { 0x138a6d4, "_ZN24FStreamingManagerTextureC1Ev" },
    { 0x138a75c, "_ZN7UObject10StaticInitEv" },
    { 0x138b470, "_ZN4FURLC1EPKc" },
    { 0x138d480, "_Z17appCleanFileCachev" },
    { 0x138d818, "_ZN7UObject10LoadConfigEP6UClassPKcmP9UProperty" },
    { 0x138d864, "_ZN29FShaderCompilingThreadManagerC2Ev" },
    { 0x138d910, "_ZN4FURLD2Ev" },
    { 0x138dbfc, "_Z19LoadStartupPackagesv" },
    { 0x138dd18, "_Z20ShaderPlatformToText15EShaderPlatformjj" },
    { 0x138dd1c, "_Z27MarkObjectsToDisregardForGCv" },
    { 0x138e0cc, "_Z13LocalizeErrorPKcS0_S0_" },
    { 0x1390c90, "_ZN7UObject9AddToRootEv" },
    { 0x1391f5c, "_Z20ValidatePixelFormatsv" },
    { 0x1392670, "_ZN9AGameInfo22eventOnEngineHasLoadedEv" },
    { 0x1392674, "_ZN7UObject11SetLanguageEPKcj" },
    { 0x1393410, "_ZN27FStreamingManagerCollectionC2Ev" },
    { 0x13940fc, "_ZN15FConfigCacheIni17GetSectionPrivateEPKcjjS1_" },
    { 0x1395874, "_ZN17FObjectPropagator7UnpauseEv" },
    { 0x13960ec, "_ZN15FConfigCacheIni7GetBoolEPKcS1_RjS1_" },
    { 0x1396b40, "_Z31UploadHardwareSurveyIfNecessaryv" },
    { 0x13970b0, "_ZN4FURLC2EPS_PKc11ETravelType" },
    { 0x1398ce0, "_ZN19UGameViewportClient9eventInitER7FString" },
    { 0x1398f8c, "_Z13appSocketInitj" },
};
static const struct { const char *sym; unsigned len; } ip_owner_syms[] = {
    { "_ZN11FEngineLoop7PreInitEPKc", 2672 },
    { "_ZN11FEngineLoop4InitEv", 1412 },
    { "_Z7appInitPKcP13FOutputDeviceP20FOutputDeviceConsoleP18FOutputDeviceErrorP16FFeedbackContextP12FFileManagerP22FCallbackEventObserverP20FCallbackQueryDevicePFP15FConfigCacheInivE", 3308 },
    { "_ZN11UGameEngine4InitEv", 2176 },
};
#define IP_OWNERS 4
static struct { uintptr_t lo; unsigned len, prev; uint64_t last; } ip_owner[IP_OWNERS];
static bucket ip_site[128], ip_gap[192];
static int hint_isite, hint_igap;
static volatile int ip_changed;
// 16 words: StaticConstructObject and friends take more than 8
typedef uint32_t (*fn16)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                         uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static fn16 ip_orig[IP_GOT];

static int ip_find(uintptr_t lr) {
    for (int i = 0; i < IP_OWNERS; i++)
        if (lr - ip_owner[i].lo < ip_owner[i].len)
            return i;
    return -1;
}

#define IW(i) static uint32_t iw##i(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t i2, uint32_t j, uint32_t k, uint32_t l, uint32_t m, uint32_t n2, uint32_t o2, uint32_t p) {     uintptr_t lr = (uintptr_t)__builtin_return_address(0);     int o = ip_find(lr);     if (o < 0)         return ip_orig[i](a, b, c, d, e, f, g, h, i2, j, k, l, m, n2, o2, p);     unsigned site = lr - ue3_mod.load_addr;     uint64_t t0 = sceKernelGetProcessTimeWide();     if (ip_owner[o].last)         bucket_add(ip_gap, 192, &hint_igap, (ip_owner[o].prev & 0xffff) << 16 | (site & 0xffff), t0 - ip_owner[o].last);     uint32_t r = ip_orig[i](a, b, c, d, e, f, g, h, i2, j, k, l, m, n2, o2, p);     uint64_t t1 = sceKernelGetProcessTimeWide();     bucket_add(ip_site, 128, &hint_isite, site, t1 - t0);     ip_owner[o].prev = site;     ip_owner[o].last = t1;     ip_changed = 1;     return r; }
IW(0) IW(1) IW(2) IW(3) IW(4) IW(5) IW(6) IW(7) IW(8) IW(9) IW(10) IW(11) IW(12) IW(13) IW(14) IW(15) IW(16) IW(17) IW(18) IW(19) IW(20) IW(21) IW(22) IW(23) IW(24) IW(25) IW(26) IW(27) IW(28) IW(29) IW(30) IW(31) IW(32) IW(33) IW(34) IW(35) IW(36) IW(37) IW(38) IW(39) IW(40) IW(41) IW(42) IW(43) IW(44) IW(45) IW(46) IW(47) IW(48) IW(49) IW(50) IW(51) IW(52) IW(53) IW(54) IW(55) IW(56) IW(57) IW(58) IW(59) IW(60) IW(61) IW(62) IW(63) IW(64)
static fn16 const ip_fns[IP_GOT] = { iw0, iw1, iw2, iw3, iw4, iw5, iw6, iw7, iw8, iw9, iw10, iw11, iw12, iw13, iw14, iw15, iw16, iw17, iw18, iw19, iw20, iw21, iw22, iw23, iw24, iw25, iw26, iw27, iw28, iw29, iw30, iw31, iw32, iw33, iw34, iw35, iw36, iw37, iw38, iw39, iw40, iw41, iw42, iw43, iw44, iw45, iw46, iw47, iw48, iw49, iw50, iw51, iw52, iw53, iw54, iw55, iw56, iw57, iw58, iw59, iw60, iw61, iw62, iw63, iw64 };

static void ip_hooks(void) {
    for (int i = 0; i < IP_OWNERS; i++) {
        ip_owner[i].lo = ue3_sym(ip_owner_syms[i].sym) & ~1;
        ip_owner[i].len = ip_owner_syms[i].len;
    }
    int n = 0;
    for (int i = 0; i < IP_GOT; i++) {
        uintptr_t *slot = (uintptr_t *)(ue3_mod.load_addr + ip_got[i].slot);
        uintptr_t want = ue3_sym(ip_got[i].sym);
        if (!want || *slot != want)
            continue;
        ip_orig[i] = (fn16)*slot;
        *slot = (uintptr_t)ip_fns[i];
        n++;
    }
    LOG("ip: %d/%d got slots wrapped", n, IP_GOT);
}

// cumulative, so never cleared: sites are full .so offsets, gaps use the low 16 bits
static void ip_dump(void) {
    if (!ip_changed)
        return;
    ip_changed = 0;
    for (int pass = 0; pass < 2; pass++) {
        bucket *t = pass ? ip_gap : ip_site;
        int size = pass ? 192 : 128;
        char line[1024];
        int len = snprintf(line, sizeof(line), pass ? "ip gap:" : "ip call:");
        uint64_t floor = ~0ull;
        for (int k = 0; k < 14; k++) {
            int best = -1;
            for (int i = 0; i < size && t[i].n; i++)
                if (t[i].us >= 50000 && t[i].us < floor && (best < 0 || t[i].us > t[best].us))
                    best = i;
            if (best < 0)
                break;
            floor = t[best].us;
            if (pass)
                len += snprintf(line + len, sizeof(line) - len, " %x>%x %u/%u", t[best].key >> 16, t[best].key & 0xffff, (unsigned)(t[best].us / 1000), t[best].n);
            else
                len += snprintf(line + len, sizeof(line) - len, " %x %u/%u", t[best].key, (unsigned)(t[best].us / 1000), t[best].n);
        }
        LOG("%s", line);
    }
}

// inclusive time of some game thread functions, all hooked with SO_CONTINUE, so only
// single-thread functions here. floats travel as raw bits
enum { P_ENGINE, P_WORLD, P_INTERP, P_SKELPOSE, P_COND, P_STREAM, P_SOUND, P_ANIM, P_DIR, P_FACEFX, P_FMAT, P_VMAT, P_EVENT, P_VIS, P_PTCL, P_HEAD,
       P_GC, P_PSCTICK, P_PSCINIT, P_PSCACT, P_PSCDYN, P_SCENE, P_POST, P_FLUSH, P_VPDRAW,
       P_PHYS, P_PHYSWAIT, P_RBBONES, P_BLENDPHYS, P_INITART, P_ACTOR, P_CTRL, P_COMP, P_LINE,
       P_TG_ALL, P_TG_DURING, P_TG_POST, P_TG_UPDATE, P_TG_FX, P_KISMET, P_GFXADV, P_PCTICK, P_PAWNSPEC, P_NODESPEC,
       P_ASYNCLOAD, P_PURGE, P_STREAMVOL, P_DC_POST, P_DC_UPDATE, P_DC_DURING, P_DC_FX, P_ASYNCWORK, P_WAITASYNC, P_PERFGC, P_RBNOTIFY, P_PDMDYN, P_INPUT, P_CAMERA, P_COUNT };
static const char *prof_names[P_COUNT] = { "eng", "world", "interp", "skelpose", "tracks", "stream", "sound", "anim", "dir", "facefx", "fmat", "vmat", "event", "vis", "ptcl", "head",
                                           "gc", "psctick", "pscinit", "pscact", "pscdyn", "scene", "post", "flush", "vpdraw",
                                           "phys", "physwait", "rbbones", "blendphys", "initart", "actor", "ctrl", "comp", "line",
                                           "tgall", "tgduring", "tgpost", "tgupdate", "tgfx", "kismet", "gfxadv", "pctick", "pawnspec", "nodespec",
                                           "asyncload", "purge", "streamvol", "dcpost", "dcupdate", "dcduring", "dcfx", "asyncwork", "waitasync", "perfgc", "rbnotify", "pdmdyn", "input", "camera" };
static so_hook prof_hook[P_COUNT];
static uint64_t prof_us[P_COUNT];
static unsigned prof_calls[P_COUNT];

#define PROF_BEGIN uint64_t t0 = sceKernelGetProcessTimeWide()
#define PROF_END(i) prof_us[i] += sceKernelGetProcessTimeWide() - t0; prof_calls[i]++

static void h_engine(void *a, uint32_t dt) { PROF_BEGIN; SO_CONTINUE(int, prof_hook[P_ENGINE], a, dt); PROF_END(P_ENGINE); }
static void h_world(void *a, int t, uint32_t dt) {
    PROF_BEGIN;
    wt_prev = 0;
    wt_last = t0;
    wt_on = 1;
    SO_CONTINUE(int, prof_hook[P_WORLD], a, t, dt);
    wt_on = 0;
    uint64_t now = sceKernelGetProcessTimeWide();
    bucket_add(wt_gap, WT_GAPS, &hint_gap, wt_prev << 16 | 0xffff, now - wt_last);
    prof_us[P_WORLD] += now - t0;
    prof_calls[P_WORLD]++;
}
static void h_skelpose(void *a, uint32_t dt, int b) { PROF_BEGIN; SO_CONTINUE(int, prof_hook[P_SKELPOSE], a, dt, b); PROF_END(P_SKELPOSE); }
// (this, float, uint) and UpdateTrack's (this, float, inst, uint)
#define H3(name, i) static void name(void *a, uint32_t f, int b) { PROF_BEGIN; SO_CONTINUE(int, prof_hook[i], a, f, b); PROF_END(i); }
#define H4(name, i) static void name(void *a, uint32_t f, void *b, int c) { PROF_BEGIN; SO_CONTINUE(int, prof_hook[i], a, f, b, c); PROF_END(i); }
H3(h_interp, P_INTERP)
H3(h_stream, P_STREAM)
// time per track class, keyed by vtable (map back with _ZTV symbols)
static struct { void *vt; uint64_t us; unsigned calls; } by_vt[32];

H4(h_cond, P_COND)

static void by_vt_add(void *a, uint64_t dt) {
    void *vt = *(void **)a;
    for (int i = 0; i < 32; i++) {
        if (by_vt[i].vt == vt || !by_vt[i].vt) {
            by_vt[i].vt = vt;
            by_vt[i].us += dt;
            by_vt[i].calls++;
            break;
        }
    }
}

static so_hook fuc_hook, fuc_skel_hook;
static void h_fuc(void *a, int coll, int xform) {
    PROF_BEGIN;
    SO_CONTINUE(int, fuc_hook, a, coll, xform);
    by_vt_add(a, sceKernelGetProcessTimeWide() - t0);
}

static void h_fuc_skel(void *a, int coll, int xform) {
    PROF_BEGIN;
    SO_CONTINUE(int, fuc_skel_hook, a, coll, xform);
    by_vt_add(a, sceKernelGetProcessTimeWide() - t0);
}
H4(h_sound, P_SOUND)
H4(h_anim, P_ANIM)
H4(h_dir, P_DIR)
H4(h_facefx, P_FACEFX)
H4(h_fmat, P_FMAT)
H4(h_vmat, P_VMAT)
H4(h_event, P_EVENT)
H4(h_vis, P_VIS)
H4(h_ptcl, P_PTCL)
H4(h_head, P_HEAD)

// anything with up to 4 word args and no stack args, floats are raw bits anyway
#define HW(name, i) static uint32_t name(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { PROF_BEGIN; uint32_t r = SO_CONTINUE(uint32_t, prof_hook[i], a, b, c, d); PROF_END(i); return r; }
HW(h_gc, P_GC)
HW(h_psctick, P_PSCTICK)
HW(h_pscinit, P_PSCINIT)
HW(h_pscact, P_PSCACT)
HW(h_pscdyn, P_PSCDYN)
HW(h_scene, P_SCENE)
HW(h_post, P_POST)
HW(h_flush, P_FLUSH)
HW(h_vpdraw, P_VPDRAW)
HW(h_phys, P_PHYS)
HW(h_physwait, P_PHYSWAIT)
HW(h_blendphys, P_BLENDPHYS)
HW(h_initart, P_INITART)
HW(h_ctrl, P_CTRL)
HW(h_kismet, P_KISMET)
HW(h_gfxadv, P_GFXADV)
HW(h_pctick, P_PCTICK)
HW(h_pawnspec, P_PAWNSPEC)
HW(h_nodespec, P_NODESPEC)

// these two take more than 4 words, the extra ones are on the stack and get passed through as-is
static uint32_t h_rbbones(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    PROF_BEGIN;
    uint32_t r = SO_CONTINUE(uint32_t, prof_hook[P_RBBONES], a, b, c, d, e);
    PROF_END(P_RBBONES);
    return r;
}

static uint32_t h_line(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h) {
    PROF_BEGIN;
    uint32_t r = SO_CONTINUE(uint32_t, prof_hook[P_LINE], a, b, c, d, e, f, g, h);
    PROF_END(P_LINE);
    return r;
}

// FTickableObject::TickableObjects gets ticked from UWorld::Tick through vtable +8, swap that slot
// per class for a timing wrapper (plain data write, no code patching)
#define MAX_TICKABLE 16
static struct { void **vt; void (*orig)(void *, uint32_t); uint64_t us; unsigned calls; } tickable[MAX_TICKABLE];
static int num_tickable;

#define TW(i) static void tw##i(void *o, uint32_t dt) { uint64_t t0 = sceKernelGetProcessTimeWide(); \
    tickable[i].orig(o, dt); tickable[i].us += sceKernelGetProcessTimeWide() - t0; tickable[i].calls++; }
TW(0) TW(1) TW(2) TW(3) TW(4) TW(5) TW(6) TW(7) TW(8) TW(9) TW(10) TW(11) TW(12) TW(13) TW(14) TW(15)
static void (*const tick_wrappers[MAX_TICKABLE])(void *, uint32_t) = {
    tw0, tw1, tw2, tw3, tw4, tw5, tw6, tw7, tw8, tw9, tw10, tw11, tw12, tw13, tw14, tw15,
};

static void wrap_tickables(void) {
    static struct { void ***data; int num; } *list;
    static int last_num = -1, last_wrapped = -1;
    if (!list)
        list = (void *)ue3_sym("_ZN15FTickableObject15TickableObjectsE");
    for (int i = 0; list && i < list->num; i++) {
        void **vt = *list->data[i];
        if ((uintptr_t)vt < ue3_mod.load_addr || (uintptr_t)vt >= ue3_mod.load_addr + 0x2000000)
            continue;
        int known = 0;
        for (int j = 0; j < num_tickable; j++)
            known |= tickable[j].vt == vt;
        if (known || num_tickable == MAX_TICKABLE)
            continue;
        tickable[num_tickable].vt = vt;
        tickable[num_tickable].orig = vt[2];
        vt[2] = tick_wrappers[num_tickable];
        num_tickable++;
    }
    // only log when something changed
    int n = list ? list->num : -1;
    if (n == last_num && num_tickable == last_wrapped)
        return;
    last_num = n;
    last_wrapped = num_tickable;
    char line[400];
    int len = snprintf(line, sizeof(line), "tickables: list %p objs %d wrapped %d vt:", (void *)list, n, num_tickable);
    for (int i = 0; i < num_tickable && len < (int)sizeof(line) - 12; i++)
        len += snprintf(line + len, sizeof(line) - len, " %x", (unsigned)((uintptr_t)tickable[i].vt - ue3_mod.load_addr));
    LOG("%s", line);
}

void perf_hooks(void) {
    perf_load_start();
    ip_hooks();
    static const struct { int i; const char *sym; void *fn; } list[] = {
        { P_ENGINE, "_ZN11UGameEngine4TickEf", h_engine },
        { P_WORLD, "_ZN6UWorld4TickE10ELevelTickf", h_world },
        { P_INTERP, "_ZN14USeqAct_Interp10StepInterpEfj", h_interp },
        { P_STREAM, "_ZN14USeqAct_Interp28UpdateStreamingForCameraCutsEfj", h_stream },
        { P_SOUND, "_ZN17UInterpTrackSound11UpdateTrackEfP16UInterpTrackInstj", h_sound },
        { P_ANIM, "_ZN23UInterpTrackAnimControl11UpdateTrackEfP16UInterpTrackInstj", h_anim },
        { P_DIR, "_ZN20UInterpTrackDirector11UpdateTrackEfP16UInterpTrackInstj", h_dir },
        { P_FACEFX, "_ZN18UInterpTrackFaceFX11UpdateTrackEfP16UInterpTrackInstj", h_facefx },
        { P_FMAT, "_ZN30UInterpTrackFloatMaterialParam11UpdateTrackEfP16UInterpTrackInstj", h_fmat },
        { P_VMAT, "_ZN31UInterpTrackVectorMaterialParam11UpdateTrackEfP16UInterpTrackInstj", h_vmat },
        { P_EVENT, "_ZN17UInterpTrackEvent11UpdateTrackEfP16UInterpTrackInstj", h_event },
        { P_VIS, "_ZN22UInterpTrackVisibility11UpdateTrackEfP16UInterpTrackInstj", h_vis },
        { P_PTCL, "_ZN26UInterpTrackParticleReplay11UpdateTrackEfP16UInterpTrackInstj", h_ptcl },
        { P_HEAD, "_ZN24UInterpTrackHeadTracking11UpdateTrackEfP16UInterpTrackInstj", h_head },
        { P_GC, "_ZN7UObject14CollectGarbageEyj", h_gc },
        { P_PSCINIT, "_ZN24UParticleSystemComponent16InitializeSystemEv", h_pscinit },
        { P_PSCACT, "_ZN24UParticleSystemComponent14ActivateSystemEj", h_pscact },
        { P_PSCDYN, "_ZN24UParticleSystemComponent17UpdateDynamicDataEv", h_pscdyn },
        { P_SCENE, "_ZN14FSceneRenderer6RenderEv", h_scene },
        { P_POST, "_ZN14FSceneRenderer24RenderPostProcessEffectsEjj", h_post },
        { P_FLUSH, "_Z22FlushRenderingCommandsv", h_flush },
        { P_VPDRAW, "_ZN19UGameViewportClient4DrawEP9FViewportP7FCanvas", h_vpdraw },
        { P_PHYS, "_Z15TickRBPhysSceneP12FRBPhysScenef", h_phys },
        { P_PHYSWAIT, "_Z15WaitRBPhysSceneP12FRBPhysScene", h_physwait },
        { P_RBBONES, "_ZN22USkeletalMeshComponent27UpdateRBBonesFromSpaceBasesERK7FMatrixjj", h_rbbones },
        { P_BLENDPHYS, "_ZN22USkeletalMeshComponent13BlendInPhysicsEv", h_blendphys },
        { P_INITART, "_ZN22USkeletalMeshComponent15InitArticulatedEj", h_initart },
        { P_CTRL, "_ZN11AController4TickEf10ELevelTick", h_ctrl },
        { P_KISMET, "_ZN9USequence16ExecuteActiveOpsEfi", h_kismet },
        { P_GFXADV, "_ZN15UGFxMoviePlayer7AdvanceEf", h_gfxadv },
        { P_PCTICK, "_ZN17APlayerController4TickEf10ELevelTick", h_pctick },
        { P_PAWNSPEC, "_ZN15AArkhamGamePawn11TickSpecialEf", h_pawnspec },
        { P_NODESPEC, "_ZN16AArkhamEnemyNode11TickSpecialEf", h_nodespec },
    };
    wt_hooks();
    fuc_hook = hook_addr(ue3_sym("_ZN6AActor21ForceUpdateComponentsEjj"), (uintptr_t)h_fuc);
    fuc_skel_hook = hook_addr(ue3_sym("_ZN18ASkeletalMeshActor21ForceUpdateComponentsEjj"), (uintptr_t)h_fuc_skel);
    for (int n = 0; n < (int)(sizeof(list) / sizeof(list[0])); n++) {
        uintptr_t addr = ue3_sym(list[n].sym);
        if (addr)
            prof_hook[list[n].i] = hook_addr(addr, (uintptr_t)list[n].fn);
        else
            LOG("perf: no %s", list[n].sym);
    }
}


void perf_swap_begin(void) {
    uint64_t now = sceKernelGetProcessTimeWide();
    if (last_frame) {
        uint64_t dt = now - last_frame;
        if (dt > frame_max_us)
            frame_max_us = dt;
        if (dt > 34000)
            hitches++;
    }
    last_frame = now;
}

// called right after the swap returns, dumps a line per second
void perf_swap_end(uint64_t swap_start) {
    uint64_t now = sceKernelGetProcessTimeWide();
    swap_us += now - swap_start;
    frames++;
    if (!window_start)
        window_start = now;
    uint64_t win = now - window_start;
    if (win < 1000000)
        return;

    char line[1024];
    int len = snprintf(line, sizeof(line), "perf %d fps max %ums hitch %d draws %u tris %u swap %uus |",
                       (int)(frames * 1000000ull / win), (unsigned)(frame_max_us / 1000), hitches, draws / frames,
                       tris / frames, (unsigned)(swap_us / frames));
    for (int i = 0; i < num_threads && len < (int)sizeof(line) - 24; i++) {
        SceKernelThreadInfo info;
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(threads[i].id, &info) < 0)
            continue;
        uint64_t run = info.runClocks;
        unsigned pct = (unsigned)((run - threads[i].last) * 100 / win);
        threads[i].last = run;
        if (pct)
            len += snprintf(line + len, sizeof(line) - len, " %s %u%%@%x", threads[i].name, pct, info.currentCpuAffinityMask >> 16);
    }
    LOG("%s", line);

    len = snprintf(line, sizeof(line), "prof ms/frame:");
    for (int i = 0; i < P_COUNT; i++) {
        if (prof_calls[i])
            len += snprintf(line + len, sizeof(line) - len, " %s %u.%u/%u", prof_names[i], (unsigned)(prof_us[i] / frames / 1000),
                            (unsigned)(prof_us[i] / frames / 100 % 10), prof_calls[i]);
        prof_us[i] = 0;
        prof_calls[i] = 0;
        if (len > 700) {
            LOG("%s", line);
            len = snprintf(line, sizeof(line), "prof+");
        }
    }
    LOG("%s", line);

    len = snprintf(line, sizeof(line), "gl ms/frame:");
    for (int i = 0; i < G_COUNT; i++) {
        if (gl_calls[i])
            len += snprintf(line + len, sizeof(line) - len, " %s %u.%u/%u/%uk", gl_names[i], (unsigned)(gl_us[i] / frames / 1000),
                            (unsigned)(gl_us[i] / frames / 100 % 10), gl_calls[i], (unsigned)(gl_bytes[i] >> 10));
        gl_us[i] = gl_bytes[i] = 0;
        gl_calls[i] = 0;
    }
    LOG("%s", line);

    wt_dump("wt gap:", wt_gap, WT_GAPS, frames, 300, 0);
    wt_dump("wt call:", wt_site, WT_SITES, frames, 300, 1);
    wt_dump("wt cls:", wt_cls, WT_CLS, frames, 200, 2);

    wrap_tickables();
    len = snprintf(line, sizeof(line), "tick:");
    for (int i = 0; i < num_tickable; i++) {
        if (tickable[i].us / frames >= 100)
            len += snprintf(line + len, sizeof(line) - len, " %x %u.%u/%u", (unsigned)((uintptr_t)tickable[i].vt - ue3_mod.load_addr),
                            (unsigned)(tickable[i].us / frames / 1000), (unsigned)(tickable[i].us / frames / 100 % 10), tickable[i].calls);
        tickable[i].us = 0;
        tickable[i].calls = 0;
    }
    if (len > 6)
        LOG("%s", line);

    len = snprintf(line, sizeof(line), "fuc:");
    for (int i = 0; i < 32 && by_vt[i].vt && len < (int)sizeof(line) - 32; i++) {
        if (by_vt[i].us / frames >= 100)
            len += snprintf(line + len, sizeof(line) - len, " %x %u.%u/%u", (unsigned)((uintptr_t)by_vt[i].vt - ue3_mod.load_addr),
                            (unsigned)(by_vt[i].us / frames / 1000), (unsigned)(by_vt[i].us / frames / 100 % 10), by_vt[i].calls / frames);
        by_vt[i].us = 0;
        by_vt[i].calls = 0;
    }
    if (len > 8)
        LOG("%s", line);

    draws = tris = 0;
    swap_us = frame_max_us = 0;
    frames = hitches = 0;
    window_start = now;
}
#endif
