#include <kubridge.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include "loader.h"
#include "util.h"
#include "perf.h"
#include "store.h"
#include "leak.h"

#ifdef INPUT_TRACE
static so_hook hud_touch_hook;
static int (*world_is_paused)(void *);
static void **gworld;

// logs what the in-game hud gets from touches and whether it takes them
static int hud_touch(uint8_t *hud, unsigned type, float *loc) {
    int paused = *gworld ? world_is_paused(*gworld) : -1;
    int ret = SO_CONTINUE(int, hud_touch_hook, hud, type, loc);
    LOG("hud touch type %u at %d,%d paused %d flag %02x screen %p -> %d", type, (int)loc[0], (int)loc[1],
        paused, hud[0x60d], *(void **)(hud + 0x784), ret);
    return ret;
}

static so_hook pc_touch_hook, pc_tap_hook, pc_swipe_hook;
static int (*input_enabled)(void *);

static void pc_touch(uint8_t *pc, unsigned handle, unsigned type, float *loc) {
    int en = input_enabled(pc), st = pc[0x633];
    SO_CONTINUE(int, pc_touch_hook, pc, handle, type, loc);
    LOG("pc touch %u type %u at %d,%d enabled %d (%p %p %02x) mode %u state %d -> %d", handle, type, (int)loc[0], (int)loc[1],
        en, *(void **)(pc + 0x5cc), *(void **)(pc + 0x5d4), pc[0x610], pc[0x634], st, pc[0x633]);
}

static void pc_tap(uint8_t *pc, float *loc, void *pawn) {
    LOG("pc tap at %d,%d mode %u pawn %p", (int)loc[0], (int)loc[1], pc[0x634], pawn);
    SO_CONTINUE(int, pc_tap_hook, pc, loc, pawn);
}

// the float goes through as raw bits so the unprototyped call doesn't promote it
static void pc_swipe(uint8_t *pc, float *from, float *to, unsigned dir, uint32_t len, void *pawn) {
    LOG("pc swipe %d,%d -> %d,%d dir %u mode %u", (int)from[0], (int)from[1], (int)to[0], (int)to[1], dir, pc[0x634]);
    SO_CONTINUE(int, pc_swipe_hook, pc, from, to, dir, len, pawn);
}
#endif

static void poke16(uint16_t *addr, uint16_t val) {
    *addr = val;
#ifdef USE_KUBRIDGE
    kuKernelFlushCaches(addr, 2);
#else
    sceKernelSyncVMDomain(vm_blk, addr, 2);
#endif
}

// matinee float property tracks end every tick in actor->ForceUpdateComponents(FALSE, FALSE)
// unless the property has a script callback. for the thugs' DrawScale track that's a full
// reattach of the pawn, ~21ms on the vita, twice per frame: the whole cutscene slowdown.
// the scale almost never changes, so the call is cut out and only made when the value moved
static so_hook fprop_hook;
static int (*fname_eq)(const void *name, const char *str);
static void *(*group_actor)(void *inst);
static uint16_t *fprop_fuc_call;

static void fprop_update(uint8_t *track, uint32_t time, uint8_t *inst, int jump) {
    if (!fname_eq(track + 0x7c, "DrawScale")) {
        SO_CONTINUE(int, fprop_hook, track, time, inst, jump);
        return;
    }
    float *prop = *(float **)(inst + 0x44);
    float old = prop ? *prop : 0.0f;
    poke16(fprop_fuc_call, 0xbf00); // nop over blx r3
    SO_CONTINUE(int, fprop_hook, track, time, inst, jump);
    poke16(fprop_fuc_call, 0x4798); // blx r3
    // same condition as CallPropertyUpdateCallback returning 0
    int has_callback = *(void **)(inst + 0x3c) && *(void **)(inst + 0x40);
    void **actor = group_actor(inst);
    if (actor && prop && *prop != old && !has_callback)
        ((void (*)(void *, int, int))((void **)*actor)[0x124 / 4])(actor, 0, 0);
}

// light environments: every UpdateTransform does a full FDynamicLightEnvironmentState::Update
// (static light traces, light list rebuild), ~20ms each on the vita. in tutorial combat the
// pawns get a transform update every tick, player + thug, two ticks per frame = the 10fps drop.
// the state's own Tick keeps updating on its distance/time schedule, so the forced one is only
// let through once a second per component
#define DLE_SLOTS 32
static struct { void *comp; uint64_t last; } dle_seen[DLE_SLOTS];
static void (*comp_update_transform)(void *);
static void (*dle_state_update)(void *);

static void dle_update_transform(uint8_t *comp) {
    comp_update_transform(comp);
    void *state = *(void **)(comp + 0x70);
    if (!(comp[0x58] & 1) || !state)
        return;
    uint64_t now = sceKernelGetProcessTimeWide();
    int slot = 0;
    for (int i = 0; i < DLE_SLOTS; i++) {
        if (dle_seen[i].comp == comp) {
            if (now - dle_seen[i].last < 1000000)
                return;
            slot = i;
            break;
        }
        if (dle_seen[i].last < dle_seen[slot].last)
            slot = i;
    }
    dle_seen[slot].comp = comp;
    dle_seen[slot].last = now;
    dle_state_update(state);
}

// vtable slot +0x130 of the two classes using it, plain data write
static void patch_light_env(void) {
    static const uint32_t slots[] = { 0x1295880, 0x12959f8 }; // UDynamic/UParticleLightEnvironmentComponent
    uintptr_t orig = ue3_sym("_ZN33UDynamicLightEnvironmentComponent15UpdateTransformEv");
    comp_update_transform = (void *)ue3_sym("_ZN15UActorComponent15UpdateTransformEv");
    dle_state_update = (void *)ue3_sym("_ZN29FDynamicLightEnvironmentState6UpdateEv");
    for (int i = 0; i < 2; i++) {
        uintptr_t *vt = (uintptr_t *)(ue3_mod.load_addr + slots[i]);
        if (orig && comp_update_transform && dle_state_update && *vt == orig)
            *vt = (uintptr_t)&dle_update_transform;
        else
            LOG("light env patch: slot %x has %x", (unsigned)slots[i], (unsigned)*vt);
    }
}

// boot: appCleanFileCache -> USystem::PerformPeriodicCacheCleanup runs HVS_Zip::findFiles over the
// obb index for every search path looking for *.tmp and stale download cache files. 16s of black
// screen on the vita after the intro, and there's nothing to clean (read-only obb, no download
// cache). PreInit is its only caller, through got slot 0x138d480
static void clean_file_cache(void) {
}

static void patch_boot(void) {
    uintptr_t *slot = (uintptr_t *)(ue3_mod.load_addr + 0x138d480);
    uintptr_t orig = ue3_sym("_Z17appCleanFileCachev");
    if (orig && *slot == orig)
        *slot = (uintptr_t)&clean_file_cache;
    else
        LOG("boot patch: slot has %x", (unsigned)*slot);
}

// the counter circles live in the active UCounterTouchMode: TArray at +0x7c/+0x80 of 0x1c byte
// FTouchLocation, screen position at +0x10, bit 0 of +8 = already touched (see ProcessInput)
static so_hook counter_init_hook, counter_end_hook;
static uint8_t *volatile counter_mode;

static void counter_init(uint8_t *mode) {
    SO_CONTINUE(int, counter_init_hook, mode);
    counter_mode = mode;
}

static void counter_end(uint8_t *mode) {
    if (counter_mode == mode)
        counter_mode = NULL;
    SO_CONTINUE(int, counter_end_hook, mode);
}

// next circle still waiting for a tap, read from the input thread so it's only a best guess
int counter_target(int *x, int *y) {
    uint8_t *mode = counter_mode;
    if (!mode)
        return 0;
    uint8_t *locs = *(uint8_t **)(mode + 0x7c);
    int count = *(int *)(mode + 0x80);
    for (int i = 0; locs && i < count && i < 8; i++) {
        uint8_t *loc = locs + i * 0x1c;
        if (loc[8] & 1)
            continue;
        *x = (int)*(float *)(loc + 0x10);
        *y = (int)*(float *)(loc + 0x14);
        return 1;
    }
    return 0;
}

void patch_game(void) {
    fname_eq = (void *)ue3_sym("_ZNK5FNameeqEPKc");
    group_actor = (void *)ue3_sym("_ZN16UInterpTrackInst13GetGroupActorEv");
    uintptr_t fprop = ue3_sym("_ZN21UInterpTrackFloatProp11UpdateTrackEfP16UInterpTrackInstj");
    fprop_fuc_call = (uint16_t *)((fprop & ~1) + 0x4c);
    if (*fprop_fuc_call == 0x4798)
        fprop_hook = hook_addr(fprop, (uintptr_t)&fprop_update);
    else
        LOG("float prop patch: unexpected code %04x", *fprop_fuc_call);

    patch_light_env();
    patch_boot();
#ifdef LEAK_TRACE
    leak_init();
#endif
    store_init();

    counter_init_hook = hook_addr(ue3_sym("_ZN17UCounterTouchMode8InitModeEv"), (uintptr_t)&counter_init);
    counter_end_hook = hook_addr(ue3_sym("_ZN17UCounterTouchMode7EndModeEv"), (uintptr_t)&counter_end);

#ifdef PERF_TRACE
    perf_hooks();
#endif
#ifdef INPUT_TRACE
    world_is_paused = (void *)ue3_sym("_ZN6UWorld8IsPausedEv");
    gworld = (void **)ue3_sym("GWorld");
    hud_touch_hook = hook_addr(ue3_sym("_ZN14AArkhamGameHUD18ProcessTouchEventsEhR9FVector2D"), (uintptr_t)&hud_touch);
    input_enabled = (void *)ue3_sym("_ZNK27AArkhamGamePlayerController14IsInputEnabledEv");
    pc_touch_hook = hook_addr(ue3_sym("_ZN27AArkhamGamePlayerController17ProcessInputTouchEjhRK9FVector2D"), (uintptr_t)&pc_touch);
    pc_tap_hook = hook_addr(ue3_sym("_ZN27AArkhamGamePlayerController10ProcessTapERK9FVector2DP5APawn"), (uintptr_t)&pc_tap);
    pc_swipe_hook = hook_addr(ue3_sym("_ZN27AArkhamGamePlayerController12ProcessSwipeERK9FVector2DS2_hfP5APawn"), (uintptr_t)&pc_swipe);
#endif
}
