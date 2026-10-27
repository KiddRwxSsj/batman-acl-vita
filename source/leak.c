// LEAK_TRACE: who owns the engine's heap. every GMalloc Malloc/Realloc/Free goes through here (vtable
// swap on FMallocAnsi, GCreateMalloc makes a bare one), each live block is kept in a table with the engine function that
// asked for it, and every level load prints the call sites that hold the most and grew the most.
// appMalloc/appRealloc aren't inlined, so the real caller is their own saved lr, read off the stack.
// the engine's direct malloc calls (HVS_Malloc, physx) come in from bionic.c as "direct", the site is
// just the return address there. GMalloc blocks pass through HVS_Malloc too, lk_malloc takes those back
#include "leak.h"
#include "loader.h"
#include "util.h"

#include "bionic.h"
#include "zfast.h"

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#define CAP (1u << 20)
#define SITES 16384
#define MAX_SITE_IDS 8192

typedef void *(*malloc_fn)(void *, unsigned long, unsigned long);
typedef void *(*realloc_fn)(void *, void *, unsigned long, unsigned long);
typedef void (*free_fn)(void *, void *);

static malloc_fn orig_malloc;
static realloc_fn orig_realloc;
static free_fn orig_free;
static uintptr_t app_malloc_ret, app_realloc_ret, text_lo, text_hi;

// open addressing on the block address, linear probing, backward shift on delete
static uint32_t *keys, *sizes;
static uint16_t *owners;
static unsigned used, dropped;

static struct { uint32_t addr; uint32_t live, count, prev; } sites[MAX_SITE_IDS];
static uint16_t site_hash[SITES];
static unsigned num_sites;
static uint64_t live_total, direct_total;

#define DIRECT 0x80000000u

static inline void account(unsigned i, int sign) {
    uint32_t size = sizes[i] & ~DIRECT;
    sites[owners[i]].live += sign * size;
    sites[owners[i]].count += sign;
    if (sizes[i] & DIRECT)
        direct_total += sign * (int64_t)size;
    else
        live_total += sign * (int64_t)size;
}

static SceKernelLwMutexWork lock;
static int ready;

static inline unsigned slot_of(uint32_t k) {
    return (k >> 3) * 2654435761u >> 12;
}

// lock held
static unsigned site_id(uint32_t addr) {
    unsigned h = (addr >> 1) * 2654435761u >> 18;
    while (1) {
        unsigned id = site_hash[h];
        if (!id)
            break;
        if (sites[id].addr == addr)
            return id;
        h = (h + 1) & (SITES - 1);
    }
    if (num_sites + 1 >= MAX_SITE_IDS)
        return 0;
    unsigned id = ++num_sites;
    sites[id].addr = addr;
    site_hash[h] = id;
    return id;
}

static void add(void *p, uint32_t size, uint32_t site, uint32_t direct) {
    if (!p)
        return;
    sceKernelLockLwMutex(&lock, 1, NULL);
    if (used >= CAP - CAP / 8) {
        dropped++;
        sceKernelUnlockLwMutex(&lock, 1);
        return;
    }
    unsigned id = site_id(site);
    uint32_t k = (uint32_t)p;
    unsigned i = slot_of(k);
    while (keys[i] && keys[i] != k)
        i = (i + 1) & (CAP - 1);
    if (!keys[i])
        used++;
    else
        account(i, -1);
    keys[i] = k;
    sizes[i] = (size & ~DIRECT) | direct;
    owners[i] = id;
    account(i, 1);
    sceKernelUnlockLwMutex(&lock, 1);
}

// returns the old size, 0 if the block wasn't known (allocated before the swap)
static uint32_t del(void *p, uint32_t *site) {
    if (!p)
        return 0;
    sceKernelLockLwMutex(&lock, 1, NULL);
    uint32_t k = (uint32_t)p, size = 0;
    unsigned i = slot_of(k);
    while (keys[i] && keys[i] != k)
        i = (i + 1) & (CAP - 1);
    if (keys[i]) {
        size = sizes[i] & ~DIRECT;
        if (site)
            *site = sites[owners[i]].addr;
        account(i, -1);
        keys[i] = 0;
        used--;
        // backward shift so probe chains stay intact
        unsigned j = i;
        while (1) {
            j = (j + 1) & (CAP - 1);
            if (!keys[j])
                break;
            unsigned home = slot_of(keys[j]);
            if ((j > i && (home <= i || home > j)) || (j < i && (home <= i && home > j))) {
                keys[i] = keys[j];
                sizes[i] = sizes[j];
                owners[i] = owners[j];
                keys[j] = 0;
                i = j;
            }
        }
    }
    sceKernelUnlockLwMutex(&lock, 1);
    return size;
}

static uint32_t caller(uint32_t lr, uint32_t sp) {
    uint32_t site = lr;
    if (lr == app_malloc_ret)
        site = *(uint32_t *)(sp + 0x14); // push {r4, r5, lr}; sub sp, #0xc
    else if (lr == app_realloc_ret)
        site = *(uint32_t *)(sp + 0x1c); // push {r4-r7, lr}; sub sp, #0xc
    if (site < text_lo || site >= text_hi)
        site = lr;
    return site;
}

void *lk_malloc(void *self, unsigned long n, unsigned long align, uint32_t sp, uint32_t lr) {
    void *p = orig_malloc(self, n, align);
    if (p)
        del(((void **)p)[-1], NULL); // FMallocAnsi keeps the raw HVS_Malloc pointer right below
    add(p, n, caller(lr, sp), 0);
    return p;
}

void *lk_realloc(void *self, void *old, unsigned long n, unsigned long align, uint32_t sp, uint32_t lr) {
    uint32_t old_site = 0;
    uint32_t old_size = del(old, &old_site);
    void *p = orig_realloc(self, old, n, align);
    if (p)
        add(p, n, caller(lr, sp), 0);
    else if (n && old_size)
        add(old, old_size, old_site, 0);
    return p;
}

void lk_free(void *self, void *p) {
    del(p, NULL);
    orig_free(self, p);
}

// from bionic's malloc_b/realloc_b/free_b, site 0 = gone
void leak_direct(void *p, size_t n, uintptr_t lr) {
    if (!ready || !p)
        return;
    if (!lr)
        del(p, NULL);
    else
        add(p, n, lr, DIRECT);
}

// entry sp and lr go to the C side as extra args, the engine's frame sits right above entry sp
__asm__(
    "    .text\n"
    "    .syntax unified\n"
    "    .thumb\n"
    "    .align 2\n"
    "    .global lk_malloc_entry\n"
    "    .type lk_malloc_entry, %function\n"
    "    .thumb_func\n"
    "lk_malloc_entry:\n"
    "    mov r3, sp\n"
    "    sub sp, #8\n"
    "    str lr, [sp]\n"
    "    ldr ip, 1f\n"
    "    blx ip\n"
    "    ldr lr, [sp]\n"
    "    add sp, #8\n"
    "    bx lr\n"
    "    .align 2\n"
    "1:  .word lk_malloc\n"
    "    .global lk_realloc_entry\n"
    "    .type lk_realloc_entry, %function\n"
    "    .thumb_func\n"
    "lk_realloc_entry:\n"
    "    mov ip, sp\n"
    "    sub sp, #8\n"
    "    str ip, [sp]\n"
    "    str lr, [sp, #4]\n"
    "    ldr ip, 2f\n"
    "    blx ip\n"
    "    ldr lr, [sp, #4]\n"
    "    add sp, #8\n"
    "    bx lr\n"
    "    .align 2\n"
    "2:  .word lk_realloc\n");

void lk_malloc_entry(void);
void lk_realloc_entry(void);

static void *table_block(size_t size) {
    size = (size + 0xfffff) & ~0xfffff;
    SceUID uid = sceKernelAllocMemBlock("leak", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_RW, size, NULL);
    if (uid < 0)
        uid = sceKernelAllocMemBlock("leak", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
    if (uid < 0)
        return NULL;
    void *p = NULL;
    sceKernelGetMemBlockBase(uid, &p);
    memset(p, 0, size);
    return p;
}

void leak_init(void) {
    uintptr_t base = ue3_mod.load_addr;
    uintptr_t m = ue3_sym("_ZN11FMallocAnsi6MallocEmm");
    uintptr_t r = ue3_sym("_ZN11FMallocAnsi7ReallocEPvmm");
    uintptr_t f = ue3_sym("_ZN11FMallocAnsi4FreeEPv");
    // vtable for FMallocAnsi, Malloc/Realloc/Free at +8/+0xc/+0x10 past the address point
    uintptr_t *vt = (uintptr_t *)(base + 0x1343fe8 + 8);
    if (!m || vt[2] != m || vt[3] != r || vt[4] != f) {
        LOG("leak: FMallocAnsi vtable has %x %x %x", (unsigned)vt[2], (unsigned)vt[3], (unsigned)vt[4]);
        return;
    }
    keys = table_block(CAP * 4);
    sizes = table_block(CAP * 4);
    owners = table_block(CAP * 2);
    if (!keys || !sizes || !owners) {
        LOG("leak: no memory for the tables");
        return;
    }
    app_malloc_ret = ue3_sym("_Z9appMallocmm") + 0x1c;
    app_realloc_ret = ue3_sym("_Z10appReallocPvmm") + 0x20;
    text_lo = base;
    text_hi = base + 0x1287994; // end of the .so text
    sceKernelCreateLwMutex(&lock, "leak", 0, 0, NULL);
    orig_malloc = (malloc_fn)m;
    orig_realloc = (realloc_fn)r;
    orig_free = (free_fn)f;
    vt[2] = (uintptr_t)&lk_malloc_entry;
    vt[3] = (uintptr_t)&lk_realloc_entry;
    vt[4] = (uintptr_t)&lk_free;
    ready = 1;
    LOG("leak: tracing GMalloc");
}

static int cmp_live(const void *a, const void *b) {
    uint32_t x = sites[*(const uint16_t *)a].live, y = sites[*(const uint16_t *)b].live;
    return x < y ? 1 : x > y ? -1 : 0;
}

static int cmp_growth(const void *a, const void *b) {
    int x = (int)(sites[*(const uint16_t *)a].live - sites[*(const uint16_t *)a].prev);
    int y = (int)(sites[*(const uint16_t *)b].live - sites[*(const uint16_t *)b].prev);
    return x < y ? 1 : x > y ? -1 : 0;
}

// site = return address as a .so offset (thumb bit set), decode with tools/scripts/leaksites.py
void leak_report(const char *why) {
    static uint16_t order[MAX_SITE_IDS];
    static uint64_t last_ms;
    if (!ready || uptime_ms() - last_ms < 3000)
        return;
    last_ms = uptime_ms();
    sceKernelLockLwMutex(&lock, 1, NULL);
    unsigned n = num_sites;
    for (unsigned i = 0; i < n; i++)
        order[i] = i + 1;
    // heap used + memblocks - engine = ours, vitagl's, falsojni's and anything allocated before the swap
    struct mallinfo mi = mallinfo();
    LOG("leak %s: engine live %uMB in %u blocks, direct %ukB, rest %dMB, %u sites, dropped %u, zfix %u", why,
        (unsigned)(live_total >> 20), used, (unsigned)(direct_total >> 10),
        (int)(((int64_t)mi.uordblks + big_bytes - (int64_t)live_total - (int64_t)direct_total) >> 20), n, dropped, zf_reclaimed);
    qsort(order, n, sizeof(order[0]), cmp_live);
    for (unsigned i = 0; i < n && i < 200 && sites[order[i]].live >= 128 * 1024; i++)
        LOG("leak top %x %ukB %u", (unsigned)(sites[order[i]].addr - ue3_mod.load_addr), sites[order[i]].live >> 10, sites[order[i]].count);
    qsort(order, n, sizeof(order[0]), cmp_growth);
    for (unsigned i = 0; i < n && i < 30; i++) {
        int d = (int)(sites[order[i]].live - sites[order[i]].prev);
        if (d < 64 * 1024)
            break;
        LOG("leak grew %x +%dkB now %ukB %u", (unsigned)(sites[order[i]].addr - ue3_mod.load_addr), d >> 10,
            sites[order[i]].live >> 10, sites[order[i]].count);
    }
    for (unsigned i = 1; i <= n; i++)
        sites[i].prev = sites[i].live;
    sceKernelUnlockLwMutex(&lock, 1);
}
