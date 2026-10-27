#ifndef LEAK_H
#define LEAK_H

#include <stddef.h>
#include <stdint.h>

void leak_init(void);
void leak_report(const char *why);
void leak_direct(void *p, size_t n, uintptr_t lr);

#endif
