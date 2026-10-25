#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <malloc.h>
#include <libdeflate.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>

#include "zfast.h"
#include "util.h"
#include "bionic.h"

// HVS_Zip gives inflate() a whole package (one zlib stream) at once and pulls it out 80 KB at a time
// with Z_SYNC_FLUSH, a seek backwards restarts the stream from zero. on the first call the whole
// package gets decoded with libdeflate and every call after that is a memcpy out of the copy, so
// restarts while it's being read cost nothing (zlib inflates everything again). if libdeflate says no
// the stream goes to zlib untouched.
// a finished copy is kept a few seconds because the scaleform packages get read again right away.
// a restart after that goes to zlib, those second passes often only read the start. a copy nobody
// reads for a while gets dropped too, zlib catches up if the engine ever comes back for the rest

int zf_enabled = 1;
volatile zf_stats_t zf_stats;

#define MAX_STREAMS 128 // finished streams keep their slot (no copy) until inflateEnd
#define MAX_PACKAGE (48 << 20)
#define MAX_LIVE (64 << 20)
#define KEEP_TOTAL (24 << 20)
#define KEEP_US 3000000
#define HEAP_MARGIN (32 << 20) // no copy unless this much heap stays free after it
#define IDLE_US 10000000

typedef struct {
    z_streamp z;
    const Bytef *in;
    uInt in_len;
    size_t in_used;
    uint8_t *out;
    size_t out_len, size;
    uLong adler;
    size_t maxpos;
    int starts;
    int done;   // fully handed out, copy kept until done_at + KEEP_US
    int zlib;   // restarted after the copy was freed, zlib does this pass
    int catchup; // copy dropped mid-pass, zlib has to redo the part already handed out
    int busy;
    uint64_t done_at, last_use;
} zcopy;

static zcopy copies[MAX_STREAMS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile size_t kept;

static zcopy *find(z_streamp z) {
    for (int i = 0; i < MAX_STREAMS; i++)
        if (copies[i].z == z)
            return &copies[i];
    return NULL;
}

// everything below runs with the lock held
static void release(zcopy *c) {
    if (c->done)
        kept -= c->size;
    c->done = 0;
    if (c->out) {
        big_free(c->out);
        c->out = NULL;
        zf_stats.live -= c->size;
    }
}

static void drop(zcopy *c) {
#ifdef PERF_TRACE
    if (c->maxpos < c->out_len || c->starts > 1)
        LOG("zf: closed %ukB after reading %ukB, %d starts", (unsigned)(c->out_len >> 10), (unsigned)(c->maxpos >> 10), c->starts);
#endif
    release(c);
    memset(c, 0, sizeof(*c));
}

static int evict_oldest(void) {
    zcopy *old = NULL;
    for (int i = 0; i < MAX_STREAMS; i++)
        if (copies[i].done && (!old || copies[i].done_at < old->done_at))
            old = &copies[i];
    if (old)
        release(old);
    return old != NULL;
}

extern int _newlib_heap_size_user;

// space past the arena that was never handed out plus the free chunks inside it
size_t heap_free(void) {
    struct mallinfo mi = mallinfo();
    return (size_t)_newlib_heap_size_user - mi.arena + mi.fordblks;
}

// next_in points into a package that's only mapped on paper (bionic vmap): feed zlib from the obb a
// chunk at a time, the stream fields end up as if the whole input had been there
#define CHUNK 0x10000
static int inflate_src(z_streamp z, int flush) {
    if (!z->next_in || !vmap_has(z->next_in))
        return inflate(z, flush);
    uint8_t *buf = malloc(CHUNK);
    if (!buf)
        return Z_MEM_ERROR;
    const Bytef *next = z->next_in;
    uInt avail = z->avail_in;
    int r = Z_OK, progress = 0;
    for (;;) {
        uInt n = avail < CHUNK ? avail : CHUNK;
        if (n && vmap_read(next, buf, n) != (int)n) {
            r = Z_DATA_ERROR;
            break;
        }
        uInt out = z->avail_out;
        z->next_in = buf;
        z->avail_in = n;
        r = inflate(z, flush);
        uInt used = n - z->avail_in;
        next += used;
        avail -= used;
        progress |= used || out != z->avail_out;
        if (!(r == Z_OK || (r == Z_BUF_ERROR && used)) || !z->avail_out || !avail || !used)
            break;
    }
    z->next_in = (Bytef *)next;
    z->avail_in = avail;
    free(buf);
    if (r == Z_BUF_ERROR && progress && flush != Z_FINISH)
        r = Z_OK;
    return r;
}

static zcopy *decode(z_streamp z) {
    // only HVS_Zip::nDecompressToBuffer calls inflate, z_stream is at FileEntry+0x13c and the
    // uncompressed size at +0x108
    size_t size = *(uint32_t *)((uint8_t *)z - 0x34);
    if (!z->next_in || !z->next_out || z->avail_in < 6 || !size || size > MAX_PACKAGE) {
#ifdef PERF_TRACE
        LOG("zf: zlib for %ukB -> %ukB", (unsigned)(z->avail_in >> 10), (unsigned)(size >> 10));
#endif
        zf_stats.fallbacks++;
        return NULL;
    }

    pthread_mutex_lock(&lock);
    while (zf_stats.live + size > MAX_LIVE && evict_oldest())
        ;
    zcopy *c = zf_stats.live + size <= MAX_LIVE ? find(NULL) : NULL;
    if (c) {
        c->z = z;
        c->size = size;
        zf_stats.live += size;
    }
    pthread_mutex_unlock(&lock);
    if (!c) {
#ifdef PERF_TRACE
        LOG("zf: no room for %ukB (live %ukB)", (unsigned)(size >> 10), (unsigned)(zf_stats.live >> 10));
#endif
        zf_stats.fallbacks++;
        return NULL;
    }

    uint64_t t0 = sceKernelGetProcessTimeWide();
    // the copy only goes in the heap when that leaves plenty free
    uint8_t *out = big_alloc(size, heap_free() >= size + HEAP_MARGIN);
    // a package that's only mapped on paper gets read in for the decode and dropped right after
    const Bytef *src = z->next_in;
    uint8_t *packed = NULL;
    if (out && vmap_has(src)) {
        packed = big_alloc(z->avail_in, heap_free() >= z->avail_in + HEAP_MARGIN);
        if (packed && vmap_read(src, packed, z->avail_in) == (int)z->avail_in)
            src = packed;
        else
            src = NULL;
    }
    struct libdeflate_decompressor *d = out && src ? libdeflate_alloc_decompressor() : NULL;
    size_t in_used = 0, out_len = 0;
    int r = d ? libdeflate_zlib_decompress_ex(d, src, z->avail_in, out, size, &in_used, &out_len) : -1;
    libdeflate_free_decompressor(d);
    uLong adler = 0;
    if (r == LIBDEFLATE_SUCCESS) {
        const uint8_t *t = src + in_used - 4;
        adler = (uLong)t[0] << 24 | t[1] << 16 | t[2] << 8 | t[3];
    }
    big_free(packed);
    uint64_t us = sceKernelGetProcessTimeWide() - t0;

    pthread_mutex_lock(&lock);
    if (r != LIBDEFLATE_SUCCESS) {
        LOG("zf: libdeflate %d on %ukB -> %ukB, zlib does it", r, (unsigned)(z->avail_in >> 10), (unsigned)(size >> 10));
        big_free(out);
        zf_stats.live -= size;
        memset(c, 0, sizeof(*c));
        zf_stats.fallbacks++;
        pthread_mutex_unlock(&lock);
        return NULL;
    }
    c->in = z->next_in;
    c->in_len = z->avail_in;
    c->in_used = in_used;
    c->out = out;
    c->out_len = out_len;
    c->adler = adler;
    zf_stats.us += us;
    zf_stats.bytes += out_len;
    zf_stats.decodes++;
    pthread_mutex_unlock(&lock);
#ifdef PERF_TRACE
    if (out_len >= 256 << 10)
        LOG("zf: %ukB -> %ukB in %ums", (unsigned)(in_used >> 10), (unsigned)(out_len >> 10), (unsigned)(us / 1000));
#endif
    return c;
}

// zlib's state is still where the engine's inflateInit_ left it, run it up to the engine's position
static int catch_up(z_streamp z) {
    uLong pos = z->total_out;
    Bytef *next_out = z->next_out;
    uInt avail_out = z->avail_out;
    uint8_t *tmp = malloc(0x10000);
    int r = tmp ? Z_OK : Z_MEM_ERROR;
    z->total_out = 0;
    while (r == Z_OK && z->total_out < pos) {
        uLong left = pos - z->total_out;
        z->next_out = tmp;
        z->avail_out = left < 0x10000 ? left : 0x10000;
        r = inflate_src(z, Z_SYNC_FLUSH);
    }
    free(tmp);
    z->next_out = next_out;
    z->avail_out = avail_out;
#ifdef PERF_TRACE
    LOG("zf: zlib caught up to %ukB (%d)", (unsigned)(pos >> 10), r);
#endif
    return z->total_out == pos ? Z_OK : Z_DATA_ERROR;
}

static int serve(zcopy *c, z_streamp z) {
    size_t pos = z->total_out;
    // calls after the end never touch the copy, it may be gone already
    if (pos == c->out_len && z->total_in)
        return Z_STREAM_END;
    if (!z->next_out)
        return Z_STREAM_ERROR;
    if (pos > c->out_len || !c->out)
        return Z_DATA_ERROR;
    if (!pos && !z->total_in)
        c->starts++;
    size_t n = c->out_len - pos;
    if (n > z->avail_out)
        n = z->avail_out;
    sceClibMemcpy(z->next_out, c->out + pos, n);
    z->next_out += n;
    z->avail_out -= n;
    z->total_out += n;
    zf_stats.served += n;
    if (pos + n > c->maxpos)
        c->maxpos = pos + n;
    // the input stays untouched until the end, HVS_Zip only looks at avail_in != 0 to call again
    if (pos + n < c->out_len)
        return n ? Z_OK : Z_BUF_ERROR;
    z->next_in += c->in_used;
    z->avail_in -= c->in_used;
    z->total_in = c->in_used;
    z->adler = c->adler;
    z->data_type = 64; // what zlib leaves after the last block
    pthread_mutex_lock(&lock);
    if (c->size <= KEEP_TOTAL) {
        c->done = 1;
        c->done_at = sceKernelGetProcessTimeWide();
        kept += c->size;
        while (kept > KEEP_TOTAL && evict_oldest())
            ;
    } else {
        release(c);
    }
    pthread_mutex_unlock(&lock);
    return Z_STREAM_END;
}

int zf_inflate(z_streamp z, int flush) {
    int fresh = !z->total_in && !z->total_out;
    pthread_mutex_lock(&lock);
    zcopy *c = find(z);
    if (c && fresh) {
        zf_stats.reused++;
        if (z->next_in != c->in || z->avail_in != c->in_len) {
            // same z_stream restarted on other data
            drop(c);
            c = NULL;
        } else if (c->out) {
            if (c->done)
                kept -= c->size;
            c->done = 0;
        } else {
            c->zlib = 1;
            c->catchup = 0;
        }
    }
    int use_zlib = c && c->zlib, catchup = 0;
    if (use_zlib) {
        catchup = c->catchup;
        c->catchup = 0;
    } else if (c) {
        c->busy = 1;
    }
    pthread_mutex_unlock(&lock);
    if (use_zlib) {
        if (catchup && catch_up(z) != Z_OK)
            return Z_DATA_ERROR;
        return inflate_src(z, flush);
    }
    if (!c && zf_enabled && fresh) {
        c = decode(z);
        if (c)
            c->busy = 1;
    }
    if (!c)
        return inflate_src(z, flush);
    int r = serve(c, z);
    pthread_mutex_lock(&lock);
    c->busy = 0;
    c->last_use = sceKernelGetProcessTimeWide();
    pthread_mutex_unlock(&lock);
    return r;
}

// HVS_Zip::vInitZLib restarts a stream with memset + inflateInit_ and no inflateEnd, and vCloseFile
// skips inflateEnd when nothing was read yet. every replay of a level restarts all its packages
// (they stay open), so each one leaked a zlib state (7 KB, +32 KB window when zlib did the pass).
// remember what inflateInit_ handed out per z_stream and end the old one on the next init
#define MAX_ZSTATES 2048

static struct { z_streamp z; void *state; alloc_func zalloc; free_func zfree; voidpf opaque; } zstates[MAX_ZSTATES];
volatile unsigned zf_reclaimed;

static int zstate_find(z_streamp z) {
    for (int i = 0; i < MAX_ZSTATES; i++)
        if (zstates[i].z == z)
            return i;
    return -1;
}

int zf_inflateInit_(z_streamp z, const char *version, int stream_size) {
    pthread_mutex_lock(&lock);
    int i = zstate_find(z);
    if (i >= 0 && !z->state) {
        // engine zeroed the stream, put back what inflateEnd needs to find the old state
        z->state = zstates[i].state;
        z->zalloc = zstates[i].zalloc;
        z->zfree = zstates[i].zfree;
        z->opaque = zstates[i].opaque;
        if (inflateEnd(z) == Z_OK)
            zf_reclaimed++;
        memset(z, 0, sizeof(*z));
    }
    if (i >= 0)
        zstates[i].z = NULL;
    int r = inflateInit_(z, version, stream_size);
    if (r == Z_OK && (i = zstate_find(NULL)) >= 0) {
        zstates[i].z = z;
        zstates[i].state = z->state;
        zstates[i].zalloc = z->zalloc;
        zstates[i].zfree = z->zfree;
        zstates[i].opaque = z->opaque;
    }
    pthread_mutex_unlock(&lock);
    return r;
}

int zf_inflateEnd(z_streamp z) {
    pthread_mutex_lock(&lock);
    zcopy *c = find(z);
    if (c)
        drop(c);
    int i = zstate_find(z);
    if (i >= 0)
        zstates[i].z = NULL;
    pthread_mutex_unlock(&lock);
    return inflateEnd(z);
}

// main loop: frees finished copies nobody came back for and copies left half read
void zf_tick(void) {
    if (!zf_stats.live)
        return;
    uint64_t now = sceKernelGetProcessTimeWide();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        zcopy *c = &copies[i];
        if (c->done && now - c->done_at > KEEP_US) {
            release(c);
        } else if (c->out && !c->done && !c->busy && c->last_use && now - c->last_use > IDLE_US) {
#ifdef PERF_TRACE
            LOG("zf: dropping %ukB copy left at %ukB", (unsigned)(c->out_len >> 10), (unsigned)(c->maxpos >> 10));
#endif
            release(c);
            c->zlib = 1;
            c->catchup = 1;
        }
    }
    pthread_mutex_unlock(&lock);
}

// a malloc failed somewhere: give back every copy that isn't being read this instant.
// half read ones go to zlib, which catches up when the engine comes back for them
size_t zf_trim(void) {
    size_t freed = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_STREAMS; i++) {
        zcopy *c = &copies[i];
        if (!c->out || c->busy)
            continue;
        int mid = !c->done;
        freed += c->size;
        release(c);
        if (mid) {
            c->zlib = 1;
            c->catchup = 1;
        }
    }
    pthread_mutex_unlock(&lock);
    if (freed)
        LOG("zf: malloc failed, dropped %ukB of copies", (unsigned)(freed >> 10));
    return freed;
}

int zf_uncompress(Bytef *dst, uLongf *dst_len, const Bytef *src, uLong src_len) {
    if (zf_enabled && dst && src && *dst_len) {
        struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
        size_t got = 0;
        int r = d ? libdeflate_zlib_decompress(d, src, src_len, dst, *dst_len, &got) : -1;
        libdeflate_free_decompressor(d);
        if (r == LIBDEFLATE_SUCCESS) {
            *dst_len = got;
            return Z_OK;
        }
    }
    return uncompress(dst, dst_len, src, src_len);
}
