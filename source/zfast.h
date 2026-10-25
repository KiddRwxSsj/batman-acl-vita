#ifndef ZFAST_H
#define ZFAST_H

#include <stdint.h>
#include <zlib.h>

typedef struct {
    uint64_t us, bytes, served;
    unsigned decodes, reused, fallbacks; // reused = restarts of a stream that has a slot
    unsigned live; // bytes held in decoded copies right now
} zf_stats_t;

extern int zf_enabled;
extern volatile zf_stats_t zf_stats;
extern volatile unsigned zf_reclaimed;

int zf_inflate(z_streamp z, int flush);
int zf_inflateInit_(z_streamp z, const char *version, int stream_size);
int zf_inflateEnd(z_streamp z);
int zf_uncompress(Bytef *dst, uLongf *dst_len, const Bytef *src, uLong src_len);
void zf_tick(void);
size_t zf_trim(void);
size_t heap_free(void);

#endif
