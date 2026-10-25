#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <psp2/types.h>

void perf_thread(SceUID id, const char *name);
void perf_hooks(void);
void perf_swap_begin(void);
void perf_swap_end(uint64_t swap_start);

#endif
