#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store.h"
#include "java.h"
#include "loader.h"
#include "util.h"

// no google play here. the suit menu goes PurchaseProduct -> native PurchaseByProductString,
// which wants a network connection and the java store, so that native is swapped (got slot) for
// one that pays with waynetech points (or nothing with unlock_suits 1) and then feeds the game the
// same NativeCallback_ContentPurchased google play would send. CompletedMicroTransaction only
// fires the menu's callback for a pending purchase, so unlock-all goes through the script's own
// AddPurchasedItemName instead. everything that touches script runs on the game thread
#define SUIT_COST 1000

int store_free;

// StoreItemName of every Characters= entry in UDKGame.ini
static const char *suits[] = {
    "com.wb.ArkhamCity.Batman",
    "com.wb.ArkhamCity.AnimatedBatmanSkin1",
    "com.wb.ArkhamCity.BatmanDarkKnight",
    "com.wb.ArkhamCity.BatmanBeyond",
    "com.wb.ArkhamCity.Batman1970",
    "com.wb.ArkhamCity.BatmanEarthOne",
    "com.wb.ArkhamCity.BruceWayne",
    "com.wb.ArkhamCity.BatmanInc",
    "com.wb.ArkhamCity.BatmanYearOne",
};
#define NUM_SUITS (int)(sizeof(suits) / sizeof(suits[0]))

// FString is a TArray<char> on this build
typedef struct { char *data; int num, max; } fstring;

static void *(*find_func)(void *strct, const char *name);
static void *(*find_prop)(void *strct, const char *name);
static void (*fstring_ctor)(fstring *s, const char *text);
static void (*fstring_dtor)(fstring *s);
static void (*content_purchased)(JNIEnv *, jobject, jstring, int, jstring);

#define OBJ_CLASS(o) (*(void **)((uint8_t *)(o) + 0x34))
#define PROP_OFFSET(p) (*(int *)((uint8_t *)(p) + 0x54))

static int parm_off(void *func, const char *name) {
    void *p = find_prop(func, name);
    return p ? PROP_OFFSET(p) : -1;
}

// calls a script function by name through the object's ProcessEvent (vtable +0x100)
static int call(void *obj, const char *name, uint8_t *parms) {
    uint8_t none[64] = { 0 };
    void *func = obj ? find_func(OBJ_CLASS(obj), name) : NULL;
    if (!func) {
        LOG("store: no %s", name);
        return 0;
    }
    ((void (*)(void *, void *, void *, void *))(*(void ***)obj)[0x100 / 4])(obj, func, parms ? parms : none, NULL);
    return 1;
}

static int call_int_ret(void *obj, const char *func_name, int *ret) {
    uint8_t parms[64] = { 0 };
    if (!call(obj, func_name, parms))
        return 0;
    int off = parm_off(find_func(OBJ_CLASS(obj), func_name), "ReturnValue");
    *ret = off >= 0 ? *(int *)(parms + off) : 0;
    return 1;
}

static int call_str(void *obj, const char *func_name, const char *arg, int *ret) {
    uint8_t parms[64] = { 0 };
    void *func = find_func(OBJ_CLASS(obj), func_name);
    int off = func ? parm_off(func, "ItemName") : -1;
    if (off < 0)
        return 0;
    fstring_ctor((fstring *)(parms + off), arg);
    call(obj, func_name, parms);
    fstring_dtor((fstring *)(parms + off));
    if (ret) {
        int roff = parm_off(func, "ReturnValue");
        *ret = roff >= 0 ? *(int *)(parms + roff) != 0 : 0;
    }
    return 1;
}

// GEngine->GamePlayers[0]->Actor
static void *player(void) {
    static uint8_t **gengine;
    if (!gengine)
        gengine = (uint8_t **)ue3_sym("GEngine");
    uint8_t *engine = *gengine;
    uint8_t **players = engine ? *(uint8_t ***)(engine + 0x4b0) : NULL;
    return players && players[0] ? *(void **)(players[0] + 0x40) : NULL;
}

static void *save_data(void *pc) {
    void *p = pc ? find_prop(OBJ_CLASS(pc), "SaveData") : NULL;
    return p ? *(void **)((uint8_t *)pc + PROP_OFFSET(p)) : NULL;
}

static int wt_points(void *pc, int *points) {
    void *save = save_data(pc);
    return save && call_int_ret(save, "GetWayneTechPoints", points);
}

static int is_suit(const char *item) {
    for (int i = 0; i < NUM_SUITS; i++)
        if (!strcmp(item, suits[i]))
            return 1;
    return 0;
}

static char *pending;

static int (*purchase_orig)(void *pc, fstring *product);
static int purchase(void *pc, fstring *product) {
    const char *item = product->data ? product->data : "";
    LOG("purchase request %s", item);
    if (!is_suit(item) || pending)
        return 0; // waynetech point packs and such need the real store
    if (!store_free) {
        int points;
        if (!wt_points(pc, &points) || points < SUIT_COST) {
            LOG("purchase %s: not enough waynetech points", item);
            return 0;
        }
        // same as UpgradeItem pays for upgrades
        uint8_t parms[64] = { 0 };
        void *save = save_data(pc);
        int off = parm_off(find_func(OBJ_CLASS(save), "AddWayneTechPoints"), "NumPoints");
        if (off < 0)
            return 0;
        *(int *)(parms + off) = -SUIT_COST;
        call(save, "AddWayneTechPoints", parms);
        call(pc, "SavePlayerData", NULL);
        LOG("purchase %s: paid %d of %d waynetech points", item, SUIT_COST, points);
    }
    // completed next frame, after PurchaseProduct has hooked up the menu's callbacks
    pending = strdup(item);
    return 1;
}

// 1 once every suit shows up as owned
static int unlock_all(void *pc) {
    int added = 0, all = 1;
    for (int i = 0; i < NUM_SUITS; i++) {
        int owned = 0;
        if (!call_str(pc, "GetIsItemPurchased", suits[i], &owned))
            return 0;
        if (!owned) {
            call_str(pc, "AddPurchasedItemName", suits[i], NULL);
            call_str(pc, "GetIsItemPurchased", suits[i], &owned);
            LOG("unlock %s: %s", suits[i], owned ? "ok" : "not yet");
            added += owned;
            all &= owned;
        }
    }
    if (added)
        call(pc, "SaveGlobalGameData", NULL);
    // bruce wayne isn't sold, he's locked until two-face is beaten. the flag only gates him and
    // the main menu's one-off "character unlocked" notice, SetDefeatedTwoFace sets it and saves
    int beaten = 0;
    if (!call_int_ret(pc, "HasPlayerEverDefeatedTwoFace", &beaten))
        return 0;
    if (!beaten) {
        call(pc, "SetDefeatedTwoFace", NULL);
        call_int_ret(pc, "HasPlayerEverDefeatedTwoFace", &beaten);
        LOG("unlock bruce wayne (two-face flag): %s", beaten ? "ok" : "not yet");
    }
    return all && beaten;
}

// runs once per frame on the game thread, right after FEngineLoop::Tick
static void (*process_orig)(void);
static void process_purchases(void) {
    static int unlocked, frames;
    void *pc = player();
    // the save info only exists once a game info is up, so keep trying about once a second
    if (pc && store_free && !unlocked && frames++ % 60 == 0)
        unlocked = unlock_all(pc);
    if (pc && pending) {
        content_purchased(&jni, java_thiz, jni->NewStringUTF(&jni, "ArkhamCity"), 1, jni->NewStringUTF(&jni, pending));
        free(pending);
        pending = NULL;
    }
    process_orig();
}

// the suit menu's purchase button: show the price and what the player has
static void (*localize_orig)(fstring *ret, const char *sec, const char *key, const char *pkg, const char *lang, int optional);
static void localize(fstring *ret, const char *sec, const char *key, const char *pkg, const char *lang, int optional) {
    localize_orig(ret, sec, key, pkg, lang, optional);
    if (!key || strcmp(key, "PurchaseButton") || !sec || !strstr(sec, "CharacterSelect"))
        return;
    char text[64];
    int points;
    if (store_free)
        snprintf(text, sizeof(text), "Unlock");
    else if (!wt_points(player(), &points))
        snprintf(text, sizeof(text), "Buy: %d WT", SUIT_COST);
    else if (points >= SUIT_COST)
        snprintf(text, sizeof(text), "Buy: %d WT (have %d)", SUIT_COST, points);
    else
        snprintf(text, sizeof(text), "Need %d WT (have %d)", SUIT_COST, points);
    fstring_dtor(ret);
    fstring_ctor(ret, text);
}

// got slots (offsets from the load base), checked against the symbol before swapping
static void *got_swap(uint32_t slot, const char *sym, void *fn) {
    uintptr_t *p = (uintptr_t *)(ue3_mod.load_addr + slot);
    uintptr_t orig = ue3_sym(sym);
    if (!orig || *p != orig) {
        LOG("store: got %x has %x, not %s", (unsigned)slot, (unsigned)*p, sym);
        return NULL;
    }
    *p = (uintptr_t)fn;
    return (void *)orig;
}

void store_init(void) {
    find_func = (void *)ue3_sym("_Z9FindFieldI9UFunctionEPT_P7UStructPKc");
    find_prop = (void *)ue3_sym("_Z9FindFieldI9UPropertyEPT_P7UStructPKc");
    fstring_ctor = (void *)ue3_sym("_ZN7FStringC1EPKc");
    fstring_dtor = (void *)ue3_sym("_ZN7FStringD1Ev");
    content_purchased = (void *)ue3_sym("_Z31NativeCallback_ContentPurchasedP7_JNIEnvP8_jobjectP8_jstringiS4_");
    if (!find_func || !find_prop || !fstring_ctor || !fstring_dtor || !content_purchased)
        return;
    // process first, the other two lean on it
    process_orig = got_swap(0x138205c, "_Z30vProcessQueuedAndroidPurchasesv", process_purchases);
    if (!process_orig)
        return;
    purchase_orig = got_swap(0x138cb44, "_ZN27AArkhamGamePlayerController23PurchaseByProductStringERK7FString", purchase);
    localize_orig = got_swap(0x137c420, "_Z8LocalizePKcS0_S0_S0_j", localize);
}
