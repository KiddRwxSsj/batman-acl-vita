#include "obb.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>

// only stored entries matter here, everything in the expansion file is uncompressed
typedef struct {
    char *name;
    uint32_t header;
    uint32_t size;
    uint32_t data;
} entry;

static SceUID fd = -1;
static entry *entries;
static int count;

static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const entry *)a)->name, ((const entry *)b)->name);
}

int obb_read(uint32_t offset, void *buf, uint32_t size) {
    return sceIoPread(fd, buf, size, offset);
}

int obb_open(const char *path) {
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0)
        return -1;

    uint32_t file_size = sceIoLseek(fd, 0, SCE_SEEK_END);
    uint32_t tail_size = file_size < 0x10000 ? file_size : 0x10000;
    uint8_t *tail = malloc(tail_size);
    sceIoPread(fd, tail, tail_size, file_size - tail_size);

    int eocd = -1;
    for (int i = tail_size - 22; i >= 0; i--) {
        if (rd32(tail + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        free(tail);
        return -1;
    }

    count = rd16(tail + eocd + 10);
    uint32_t cd_size = rd32(tail + eocd + 12);
    uint32_t cd_off = rd32(tail + eocd + 16);
    free(tail);

    uint8_t *cd = malloc(cd_size);
    sceIoPread(fd, cd, cd_size, cd_off);
    entries = calloc(count, sizeof(entry));

    const uint8_t *p = cd;
    for (int i = 0; i < count; i++) {
        uint16_t name_len = rd16(p + 28);
        uint16_t extra_len = rd16(p + 30);
        uint16_t comment_len = rd16(p + 32);
        entries[i].size = rd32(p + 24);
        entries[i].header = rd32(p + 42);
        entries[i].name = malloc(name_len + 1);
        memcpy(entries[i].name, p + 46, name_len);
        entries[i].name[name_len] = 0;
        p += 46 + name_len + extra_len + comment_len;
    }
    free(cd);

    qsort(entries, count, sizeof(entry), cmp_entry);
    LOG("obb: %d entries", count);
    return 0;
}

int obb_find(const char *name, uint32_t *offset, uint32_t *size) {
    entry key = { .name = (char *)name };
    entry *e = bsearch(&key, entries, count, sizeof(entry), cmp_entry);
    if (!e)
        return -1;

    // local header extra field can differ from the central one
    if (!e->data) {
        uint8_t hdr[30];
        obb_read(e->header, hdr, sizeof(hdr));
        e->data = e->header + 30 + rd16(hdr + 26) + rd16(hdr + 28);
    }
    *offset = e->data;
    *size = e->size;
    return 0;
}

// which entry a page-aligned range of the obb holds: the biggest one whose local header sits in it
const char *obb_name_at(uint32_t off, uint32_t len) {
    entry *best = NULL;
    uint32_t lo = off > 0x2000 ? off - 0x2000 : 0;
    for (int i = 0; i < count; i++)
        if (entries[i].header >= lo && entries[i].header < off + len && (!best || entries[i].size > best->size))
            best = &entries[i];
    if (!best)
        return "?";
    const char *slash = strrchr(best->name, '/');
    return slash ? slash + 1 : best->name;
}

void *obb_load(const char *name, uint32_t *size) {
    uint32_t off, len;
    if (obb_find(name, &off, &len) < 0)
        return NULL;
    void *buf = malloc(len);
    if (buf && obb_read(off, buf, len) != (int)len) {
        free(buf);
        return NULL;
    }
    *size = len;
    return buf;
}
