#ifndef LOADER_H
#define LOADER_H

#include <so_util/so_util.h>

#define SO_PATH  DATA_PATH "libUnrealEngine3.so"
#define OBB_PATH DATA_PATH "main.2.com.wb.goog.ArkhamCity.obb"

extern so_module ue3_mod;

void loader_init(void);
void patch_game(void);
int counter_target(int *x, int *y);
uintptr_t ue3_sym(const char *name);
void *gnu_unwind_find_exidx(uintptr_t pc, int *count);

#endif
