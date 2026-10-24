#include "loader.h"
#include "util.h"

#include <psp2/vshbridge.h>
#include <kubridge.h>

#define LOAD_ADDRESS 0x98000000

so_module ue3_mod;

extern so_default_dynlib ue3_imports[];
extern int ue3_imports_size;

#if defined(GL_TRACE) || defined(PERF_TRACE)
#include <stdlib.h>
#include <string.h>
extern so_default_dynlib gl_trace_imports[];
extern int gl_trace_imports_size;
#endif

// kernel plugins don't show up in the user module list, vshbridge can see them
static int module_loaded(const char *name) {
    int search[2];
    return _vshKernelSearchModuleByName(name, search) >= 0;
}

void loader_init(void) {
#ifndef SO_UTIL_VM_DOMAIN
    if (!module_loaded("kubridge"))
        fatal_error("kubridge.skprx is not installed.\nAdd it to *KERNEL in ur0:tai/config.txt and reboot.");
#endif

    if (!file_exists(SO_PATH))
        fatal_error("Missing %s\nCopy lib/armeabi-v7a/libUnrealEngine3.so from the APK there.", SO_PATH);

    if (so_file_load(&ue3_mod, SO_PATH, LOAD_ADDRESS) < 0)
        fatal_error("Failed to load %s", SO_PATH);

    so_relocate(&ue3_mod);
#ifdef PERF_TRACE
    extern so_default_dynlib perf_imports[];
    extern int perf_imports_size;
    so_default_dynlib *table = malloc(perf_imports_size + ue3_imports_size);
    memcpy(table, perf_imports, perf_imports_size);
    memcpy((char *)table + perf_imports_size, ue3_imports, ue3_imports_size);
    so_resolve(&ue3_mod, table, perf_imports_size + ue3_imports_size, 0);
#elif defined(GL_TRACE)
    // first match wins, so the tracing wrappers go in front of the real table
    so_default_dynlib *table = malloc(gl_trace_imports_size + ue3_imports_size);
    memcpy(table, gl_trace_imports, gl_trace_imports_size);
    memcpy((char *)table + gl_trace_imports_size, ue3_imports, ue3_imports_size);
    so_resolve(&ue3_mod, table, gl_trace_imports_size + ue3_imports_size, 0);
#else
    so_resolve(&ue3_mod, ue3_imports, ue3_imports_size, 0);
#endif
    patch_game();
    so_flush_caches(&ue3_mod);
    so_initialize(&ue3_mod);
    LOG("libUnrealEngine3.so loaded at %p", (void *)ue3_mod.text_base);
}

uintptr_t ue3_sym(const char *name) {
    uintptr_t addr = so_symbol(&ue3_mod, name);
    if (!addr)
        LOG("missing symbol %s", name);
    return addr;
}

// the .so's unwinder asks for the exidx table of whatever module owns the pc
void *gnu_unwind_find_exidx(uintptr_t pc, int *count) {
    if (pc >= ue3_mod.text_base && pc < ue3_mod.text_base + ue3_mod.text_size) {
        *count = ue3_mod.exidx_size / 8;
        return (void *)ue3_mod.exidx_base;
    }
    *count = 0;
    return NULL;
}
