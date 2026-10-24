#ifndef UTIL_H
#define UTIL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <psp2/kernel/clib.h>

// everything also lands in DATA_PATH log.txt, there's no console on a retail vita
#define LOG(fmt, ...) log_printf("[acl %u] " fmt "\n", (unsigned)uptime_ms(), ##__VA_ARGS__)

extern volatile unsigned progress_ticks, swap_ticks;

void log_open(void);
void log_write(const char *s, int len);
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef VERBOSE_LOG
#define VLOG(fmt, ...) LOG(fmt, ##__VA_ARGS__)
#else
#define VLOG(fmt, ...) do {} while (0)
#endif

void fatal_error(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

bool file_exists(const char *path);
bool file_load(const char *path, void **buf, size_t *size);
bool file_save(const char *path, const void *buf, size_t size);
void mkpath(const char *path);

uint64_t hash64(const void *data, size_t len);
uint64_t uptime_ms(void);

int ret0(void);
int ret1(void);
int retm1(void);

#endif
