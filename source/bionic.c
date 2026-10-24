#include "bionic.h"
#include "zfast.h"
#include "util.h"
#include "loader.h"
#include "obb.h"
#ifdef LEAK_TRACE
#include "leak.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <malloc.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>

char bionic_sF[3][84];

static char ctype_tab[257];
static short toupper_tab[257];
const char *bionic_ctype = ctype_tab;
const short *bionic_toupper_tab = toupper_tab;

static void big_init(void);

void bionic_init(void) {
    big_init();
    // BSD style table, index 0 is EOF
    for (int c = 0; c < 256; c++) {
        char f = 0;
        if (c < 128) {
            if (isupper(c)) f |= 0x01;
            if (islower(c)) f |= 0x02;
            if (isdigit(c)) f |= 0x04;
            if (isspace(c)) f |= 0x08;
            if (ispunct(c)) f |= 0x10;
            if (iscntrl(c)) f |= 0x20;
            if (isxdigit(c) && !isdigit(c)) f |= 0x40;
            if (c == ' ') f |= 0x80;
        }
        ctype_tab[c + 1] = f;
        toupper_tab[c + 1] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    toupper_tab[0] = -1;
}

// android absolute paths land under DATA_PATH with their layout intact,
// except the sdcard root which is DATA_PATH itself
static const char *fix_path(const char *path, char *buf, size_t size) {
    if (!path || path[0] != '/')
        return path;
    if (strncmp(path, "/sdcard/", 8) == 0)
        snprintf(buf, size, DATA_PATH "%s", path + 8);
    else if (strncmp(path, "/mnt/sdcard/", 12) == 0)
        snprintf(buf, size, DATA_PATH "%s", path + 12);
    else
        snprintf(buf, size, DATA_PATH "%s", path + 1);
    return buf;
}

#define FIX(p) char p##_buf[512]; p = fix_path(p, p##_buf, sizeof(p##_buf))

static void stat_to_bionic(const struct stat *s, struct bionic_stat *b) {
    memset(b, 0, sizeof(*b));
    b->st_dev = s->st_dev;
    b->__st_ino = s->st_ino;
    b->st_ino = s->st_ino;
    b->st_mode = s->st_mode;
    b->st_nlink = s->st_nlink;
    b->st_size = s->st_size;
    b->st_blksize = 512;
    b->st_blocks = (s->st_size + 511) / 512;
    b->st_atim.tv_sec = s->st_atime;
    b->st_mtim.tv_sec = s->st_mtime;
    b->st_ctim.tv_sec = s->st_ctime;
}

int stat_b(const char *path, struct bionic_stat *buf) {
    FIX(path);
    struct stat st;
    int ret = stat(path, &st);
    if (ret == 0)
        stat_to_bionic(&st, buf);
    VLOG("stat %s -> %d", path, ret);
    return ret;
}

int fstat_b(int fd, struct bionic_stat *buf) {
    struct stat st;
    int ret = fstat(fd, &st);
    if (ret == 0)
        stat_to_bionic(&st, buf);
    return ret;
}

int open_b(const char *path, int flags, ...) {
    FIX(path);
    int mode = 0666;
    if (flags & 0100) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    int f = flags & 3;
    if (flags & 0100)   f |= O_CREAT;
    if (flags & 0200)   f |= O_EXCL;
    if (flags & 01000)  f |= O_TRUNC;
    if (flags & 02000)  f |= O_APPEND;
    if (flags & 04000)  f |= O_NONBLOCK;
    int ret = open(path, f, mode);
    VLOG("open %s %x -> %d", path, flags, ret);
    return ret;
}

typedef struct {
    DIR *dir;
    struct bionic_dirent ent;
} dir_wrap;

DIR *opendir_b(const char *path) {
    FIX(path);
    DIR *d = opendir(path);
    if (!d)
        return NULL;
    dir_wrap *w = calloc(1, sizeof(*w));
    w->dir = d;
    return (DIR *)w;
}

struct bionic_dirent *readdir_b(DIR *d) {
    dir_wrap *w = (dir_wrap *)d;
    struct dirent *e = readdir(w->dir);
    if (!e)
        return NULL;
    memset(&w->ent, 0, sizeof(w->ent));
    snprintf(w->ent.d_name, sizeof(w->ent.d_name), "%s", e->d_name);
    w->ent.d_reclen = sizeof(w->ent);
    w->ent.d_type = SCE_S_ISDIR(e->d_stat.st_mode) ? 4 : 8;
    return &w->ent;
}

int closedir_b(DIR *d) {
    dir_wrap *w = (dir_wrap *)d;
    int ret = closedir(w->dir);
    free(w);
    return ret;
}

int mkdir_b(const char *path, mode_t mode) {
    FIX(path);
    return mkdir(path, mode);
}

int access_b(const char *path, int mode) {
    FIX(path);
    return access(path, mode);
}

int rmdir_b(const char *path) { FIX(path); return rmdir(path); }
int unlink_b(const char *path) { FIX(path); return unlink(path); }
int remove_b(const char *path) { FIX(path); return remove(path); }
int chdir_b(const char *path) { FIX(path); return chdir(path); }
int chmod_b(const char *path, mode_t mode) { return 0; }

int rename_b(const char *from, const char *to) {
    FIX(from);
    FIX(to);
    return rename(from, to);
}

// the .so indexes bionic's __sF with its own sizeof(FILE), so std streams
// show up as pointers into bionic_sF and have to be swapped for newlib's
static FILE *fp(FILE *f) {
    if ((char *)f == bionic_sF[0]) return stdin;
    if ((char *)f == bionic_sF[1]) return stdout;
    if ((char *)f == bionic_sF[2]) return stderr;
    return f;
}

FILE *fopen_b(const char *path, const char *mode) {
    FIX(path);
    FILE *f = fopen(path, mode);
    VLOG("fopen %s %s -> %p", path, mode, f);
    return f;
}

int fclose_b(FILE *f) { return fclose(fp(f)); }
int fflush_b(FILE *f) { return f ? fflush(fp(f)) : 0; }
char *fgets_b(char *s, int n, FILE *f) { return fgets(s, n, fp(f)); }
size_t fread_b(void *p, size_t sz, size_t n, FILE *f) {
    progress_ticks++;
    return fread(p, sz, n, fp(f));
}
int fseek_b(FILE *f, long off, int whence) { return fseek(fp(f), off, whence); }
long ftell_b(FILE *f) { return ftell(fp(f)); }
void rewind_b(FILE *f) { rewind(fp(f)); }

size_t fwrite_b(const void *p, size_t sz, size_t n, FILE *f) {
    f = fp(f);
    if (f == stdout || f == stderr) {
        log_write(p, sz * n);
        return n;
    }
    return fwrite(p, sz, n, f);
}

int fprintf_b(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret;
    f = fp(f);
    if (f == stdout || f == stderr) {
        char buf[1024];
        ret = vsnprintf(buf, sizeof(buf), fmt, ap);
        log_printf("%s", buf);
    } else {
        ret = vfprintf(f, fmt, ap);
    }
    va_end(ap);
    return ret;
}

#define MAX_MAPS 256

static struct { void *addr; size_t len; } maps[MAX_MAPS];
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

// no real mmap on the vita, file mappings get read into the heap instead.
// everything the engine maps is PROT_READ so nobody expects writes to stick
// big blocks (the engine's own >= 1 MB allocations, inflate copies, the odd real mapping) get their
// own kernel memblocks. those don't need one contiguous run of the heap, and heap fragmentation was
// what kept crashing long sessions (87 MB free in 4884 pieces, 16 MB wanted). plain user ram while
// 6 MB stay free (thread stacks come from there), then the phycont the intro movie gave back, else
// the heap. main.c keeps the heap 40 MB smaller and gfx.c keeps vitagl off that 40 MB for this
#define BIG_MIN (1 << 20)
#define USER_MARGIN (6 << 20)
#define MAX_BIGS 4096

static struct { void *addr; SceUID uid; size_t len, size; } bigs[MAX_BIGS];
static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t heap_lo, heap_hi;
volatile unsigned big_count, big_bytes;
static unsigned spills;

static void big_init(void) {
    void *p = malloc(16);
    SceKernelMemBlockInfo info = { sizeof(info) };
    if (sceKernelGetMemBlockInfoByAddr(p, &info) >= 0) {
        heap_lo = (uintptr_t)info.mappedBase;
        heap_hi = heap_lo + info.mappedSize;
    }
    free(p);
}

static void *big_block(SceKernelMemBlockType type, size_t size, size_t len) {
    SceUID uid = sceKernelAllocMemBlock("big", type, size, NULL);
    if (uid < 0)
        return NULL;
    void *p = NULL;
    sceKernelGetMemBlockBase(uid, &p);
    pthread_mutex_lock(&big_lock);
    for (int i = 0; i < MAX_BIGS; i++) {
        if (!bigs[i].addr) {
            bigs[i].addr = p;
            bigs[i].uid = uid;
            bigs[i].len = len;
            bigs[i].size = size;
            big_count++;
            big_bytes += size;
            pthread_mutex_unlock(&big_lock);
            return p;
        }
    }
    pthread_mutex_unlock(&big_lock);
    sceKernelFreeMemBlock(uid);
    return NULL;
}

void *big_alloc(size_t len, int heap) {
    void *p = NULL;
    if (len >= BIG_MIN) {
        SceKernelFreeMemorySizeInfo info = { sizeof(info) };
        sceKernelGetFreeMemorySize(&info);
        size_t u = (len + 0xfff) & ~0xfff, c = (len + 0xfffff) & ~0xfffff;
        if ((size_t)info.size_user >= u + USER_MARGIN)
            p = big_block(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, u, len);
        if (!p && (size_t)info.size_phycont >= c)
            p = big_block(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, c, len);
    }
    if (!p && heap)
        p = memalign(4096, len);
    return p;
}

// big_lock held. heap pointers never scan the table
static int big_find(void *p) {
    if (!p || ((uintptr_t)p >= heap_lo && (uintptr_t)p < heap_hi))
        return -1;
    for (int i = 0; i < MAX_BIGS; i++)
        if (bigs[i].addr == p)
            return i;
    return -1;
}

void big_free(void *p) {
    if (!p)
        return;
    SceUID uid = -1;
    pthread_mutex_lock(&big_lock);
    int i = big_find(p);
    if (i >= 0) {
        uid = bigs[i].uid;
        big_count--;
        big_bytes -= bigs[i].size;
        bigs[i].addr = NULL;
    }
    pthread_mutex_unlock(&big_lock);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
    else
        free(p);
}

// one line with everything that matters for the out-of-memory crashes, cheap enough for release
void mem_log(const char *what) {
    struct mallinfo mi = mallinfo();
    SceKernelFreeMemorySizeInfo fm = { sizeof(fm) };
    sceKernelGetFreeMemorySize(&fm);
    LOG("mem %s: heap %u/%uMB free %uMB (%u chunks) sys %u/%uMB vmaps %d big %u/%uMB zf %ukB spill %u zfix %u", what,
        (unsigned)(mi.uordblks >> 20), (unsigned)(mi.arena >> 20), (unsigned)(heap_free() >> 20), (unsigned)mi.ordblks,
        fm.size_user >> 20, fm.size_phycont >> 20, vmaps_open, big_count, big_bytes >> 20, zf_stats.live >> 10, spills, zf_reclaimed);
}

// heap full: the block gets a memblock of its own. wasteful for small ones, so only as a last resort.
// a 2 h tester session ran the 200 MB heap full (slow leak per level load) with 51 MB free outside it

static void *spill_alloc(size_t n) {
    SceKernelFreeMemorySizeInfo info = { sizeof(info) };
    sceKernelGetFreeMemorySize(&info);
    size_t u = (n + 0xfff) & ~0xfff, c = (n + 0xfffff) & ~0xfffff;
    if (!u)
        u = 0x1000;
    void *p = NULL;
    if ((size_t)info.size_user >= u + USER_MARGIN)
        p = big_block(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, u, n);
    if (!p && n >= (256 << 10) && (size_t)info.size_phycont >= c)
        p = big_block(SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, c, n);
    if (p && (++spills == 1 || spills % 100 == 0))
        LOG("heap full, %u bytes went to a memblock (%u so far, sys %u/%uMB)", n, spills,
            info.size_user >> 20, info.size_phycont >> 20);
    return p;
}

// the engine's malloc/realloc/free. it retries a failed malloc for 5 s and then crashes, so big ones
// go to memblocks and on a failure the inflate copies get dropped first
static void *malloc_i(size_t n) {
    void *p = n >= BIG_MIN ? big_alloc(n, 0) : NULL;
    if (!p)
        p = malloc(n);
    if (!p && n && zf_trim())
        p = malloc(n);
    if (!p)
        p = spill_alloc(n);
    return p;
}

static void *realloc_i(void *old, size_t n) {
    if (!old)
        return malloc_i(n);
    pthread_mutex_lock(&big_lock);
    int i = big_find(old);
    size_t old_len = i >= 0 ? bigs[i].len : 0;
    pthread_mutex_unlock(&big_lock);
    if (i >= 0 || n >= BIG_MIN) {
        if (i < 0)
            old_len = malloc_usable_size(old);
        void *p = malloc_i(n);
        if (p) {
            sceClibMemcpy(p, old, old_len < n ? old_len : n);
            big_free(old);
        }
        return p;
    }
    void *p = realloc(old, n);
    if (!p && n && zf_trim())
        p = realloc(old, n);
    if (!p && n) {
        p = spill_alloc(n);
        if (p) {
            size_t old_len = malloc_usable_size(old);
            sceClibMemcpy(p, old, old_len < n ? old_len : n);
            free(old);
        }
    }
    return p;
}

// LEAK_TRACE: the engine's own malloc calls (HVS_Malloc, physx), GMalloc's blocks get re-tagged by leak.c
void *malloc_b(size_t n) {
    void *p = malloc_i(n);
#ifdef LEAK_TRACE
    leak_direct(p, n, (uintptr_t)__builtin_return_address(0));
#endif
    return p;
}

void free_b(void *p) {
#ifdef LEAK_TRACE
    leak_direct(p, 0, 0);
#endif
    big_free(p);
}

void *realloc_b(void *old, size_t n) {
#ifdef LEAK_TRACE
    leak_direct(old, 0, 0);
#endif
    void *p = realloc_i(old, n);
#ifdef LEAK_TRACE
    leak_direct(p ? p : n ? old : NULL, n, (uintptr_t)__builtin_return_address(0));
#endif
    return p;
}

// HVS_Zip::vInitZLib maps every compressed package it opens in full (up to 19 MB, and the engine
// keeps dozens of them open over a few levels) and only ever hands that mapping to inflate. those
// get a fake address range instead and zfast reads the bytes from the obb when inflate needs them.
// the range is handed out in 1 MB units, nothing lives at 0x10000000-0x60000000 in a vita app
#define VMAP_BASE 0x10000000u
#define VMAP_UNIT (1u << 20)
#define VMAP_UNITS 1280
#define MAX_VMAPS 1024
#define VINITZLIB_RET 0x9108c0 // return address of vInitZLib's mmap call

static struct { uintptr_t base; size_t len; uint32_t off; } vmaps[MAX_VMAPS];
static uint8_t vmap_units[VMAP_UNITS];
int vmaps_open;

static int vmap_find(const void *p) {
    uintptr_t a = (uintptr_t)p;
    if (a < VMAP_BASE || a >= VMAP_BASE + VMAP_UNITS * VMAP_UNIT)
        return -1;
    for (int i = 0; i < MAX_VMAPS; i++)
        if (vmaps[i].base && a >= vmaps[i].base && a - vmaps[i].base <= vmaps[i].len)
            return i;
    return -1;
}

int vmap_has(const void *p) {
    return vmap_find(p) >= 0;
}

// bytes read or -1
int vmap_read(const void *p, void *dst, size_t n) {
    int i = vmap_find(p);
    if (i < 0)
        return -1;
    uint32_t pos = (uintptr_t)p - vmaps[i].base;
    if (pos + n > vmaps[i].len)
        n = vmaps[i].len - pos;
    return obb_read(vmaps[i].off + pos, dst, n);
}

// map_lock held
static void *vmap_new(size_t len, uint32_t off) {
    int units = (len + VMAP_UNIT - 1) / VMAP_UNIT, slot = -1;
    for (int i = 0; i < MAX_VMAPS && slot < 0; i++)
        if (!vmaps[i].base)
            slot = i;
    for (int u = 0; slot >= 0 && u + units <= VMAP_UNITS; u++) {
        int k = 0;
        while (k < units && !vmap_units[u + k])
            k++;
        if (k == units) {
            memset(vmap_units + u, 1, units);
            vmaps[slot].base = VMAP_BASE + u * VMAP_UNIT;
            vmaps[slot].len = len;
            vmaps[slot].off = off;
            vmaps_open++;
            return (void *)vmaps[slot].base;
        }
        u += k;
    }
    LOG("vmap: none left for %u bytes (%d open)", len, vmaps_open);
    return NULL;
}

static void vmap_free(int i) {
    int u = (vmaps[i].base - VMAP_BASE) / VMAP_UNIT, units = (vmaps[i].len + VMAP_UNIT - 1) / VMAP_UNIT;
    memset(vmap_units + u, 0, units);
    vmaps[i].base = 0;
    vmaps_open--;
}

static void *mmap_b2(void *addr, size_t len, int prot, int flags, int fd, off_t off);

void *mmap_from(uintptr_t lr, void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    if (fd >= 0 && (lr & ~1) - ue3_mod.load_addr == VINITZLIB_RET) {
        pthread_mutex_lock(&map_lock);
        void *p = vmap_new(len, off);
        int open = vmaps_open;
        pthread_mutex_unlock(&map_lock);
        // which packages a level pulls in, a crash log then says what was loading. shaders and the
        // like come through here too, hundreds of them, only .xxx packages are worth a line
        const char *name = obb_name_at(off, len), *ext = strrchr(name, '.');
        if (ext && !strcmp(ext, ".xxx"))
            LOG("pkg %s %ukB (%d open)", name, len >> 10, open);
        if (p)
            return p;
    }
    return mmap_b2(addr, len, prot, flags, fd, off);
}

void *mmap_b(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    return mmap_from((uintptr_t)__builtin_return_address(0), addr, len, prot, flags, fd, off);
}

static void *mmap_b2(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    void *mem = big_alloc(len, 1);
    if (!mem && zf_trim())
        mem = big_alloc(len, 1);
    if (!mem) {
        LOG("mmap: no memory for %u bytes", len);
        return (void *)-1;
    }

    if (fd >= 0) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        lseek(fd, off, SEEK_SET);
        size_t done = 0;
        while (done < len) {
            int r = read(fd, (char *)mem + done, len - done);
            if (r <= 0)
                break;
            done += r;
        }
        if (done < len)
            memset((char *)mem + done, 0, len - done);
        lseek(fd, cur, SEEK_SET);
    } else {
        memset(mem, 0, len);
    }

    pthread_mutex_lock(&map_lock);
    for (int i = 0; i < MAX_MAPS; i++) {
        if (!maps[i].addr) {
            maps[i].addr = mem;
            maps[i].len = len;
            break;
        }
    }
    pthread_mutex_unlock(&map_lock);
    VLOG("mmap fd %d off %x len %x -> %p", fd, (int)off, len, mem);
    return mem;
}

int munmap_b(void *addr, size_t len) {
    pthread_mutex_lock(&map_lock);
    int v = vmap_find(addr);
    if (v >= 0) {
        vmap_free(v);
        pthread_mutex_unlock(&map_lock);
        return 0;
    }
    for (int i = 0; i < MAX_MAPS; i++) {
        if (maps[i].addr == addr) {
            maps[i].addr = NULL;
            big_free(addr);
            break;
        }
    }
    pthread_mutex_unlock(&map_lock);
    return 0;
}

long sysconf_b(int name) {
    switch (name) {
    case 0x27: return 4096;     // _SC_PAGESIZE
    case 0x60:                  // _SC_NPROCESSORS_CONF
    case 0x61: return 3;        // _SC_NPROCESSORS_ONLN
    case 0x62: return 0x14000;  // _SC_PHYS_PAGES
    }
    LOG("sysconf(%x)", name);
    return -1;
}

// 1..34 are the same on newlib and linux, which covers everything UE3 checks
int *errno_b(void) {
    return &errno;
}

void assert_b(const char *file, int line, const char *expr) {
    LOG("assert %s:%d %s", file, line, expr);
}

void abort_b(void) {
    LOG("abort() from %p", __builtin_return_address(0));
    sceKernelExitDeleteThread(0);
}

void stack_chk_fail_b(void) {
    LOG("stack smashed in %p", __builtin_return_address(0));
    sceKernelExitDeleteThread(0);
}

int pause_b(void) {
    while (1)
        sceKernelDelayThread(1000000);
    return 0;
}

int utimes_b(const char *path, const void *times) {
    return 0;
}

int gethostname_b(char *name, size_t len) {
    strncpy(name, "psvita", len);
    return 0;
}

uint32_t inet_addr_b(const char *cp) {
    unsigned a, b, c, d;
    if (sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return 0xffffffff;
    return a | (b << 8) | (c << 16) | (d << 24);
}

int android_log_print(int prio, const char *tag, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_printf("[%s] %s\n", tag, buf);
    return 0;
}

void *dlopen_b(const char *name, int flags) {
    return (void *)1;
}

void *dlsym_b(void *handle, const char *sym) {
    void *p = vglGetProcAddress(sym);
    LOG("dlsym %s -> %p", sym, p);
    return p;
}

int dlclose_b(void *handle) {
    return 0;
}

int isfinitef_b(float f) { return isfinite(f); }
int isinff_b(float f) { return isinf(f); }
