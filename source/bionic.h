#ifndef BIONIC_H
#define BIONIC_H

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <sys/types.h>

// arm32 bionic layouts, offsets verified against the engine's own field accesses
struct bionic_stat {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    uint64_t st_ino;
};

struct bionic_dirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

extern char bionic_sF[3][84];
extern const char *bionic_ctype;
extern const short *bionic_toupper_tab;

void bionic_init(void);

int stat_b(const char *path, struct bionic_stat *buf);
int fstat_b(int fd, struct bionic_stat *buf);
int open_b(const char *path, int flags, ...);
DIR *opendir_b(const char *path);
struct bionic_dirent *readdir_b(DIR *d);
int closedir_b(DIR *d);
int mkdir_b(const char *path, mode_t mode);
int access_b(const char *path, int mode);
int rmdir_b(const char *path);
int unlink_b(const char *path);
int remove_b(const char *path);
int rename_b(const char *from, const char *to);
int chdir_b(const char *path);
int chmod_b(const char *path, mode_t mode);

FILE *fopen_b(const char *path, const char *mode);
int fclose_b(FILE *f);
int fflush_b(FILE *f);
char *fgets_b(char *s, int n, FILE *f);
int fprintf_b(FILE *f, const char *fmt, ...);
size_t fread_b(void *p, size_t sz, size_t n, FILE *f);
size_t fwrite_b(const void *p, size_t sz, size_t n, FILE *f);
int fseek_b(FILE *f, long off, int whence);
long ftell_b(FILE *f);
void rewind_b(FILE *f);

void *malloc_b(size_t n);
void free_b(void *p);
extern volatile unsigned big_count, big_bytes;
void *realloc_b(void *old, size_t n);
void *big_alloc(size_t len, int heap);
void big_free(void *p);
void mem_log(const char *what);
extern int vmaps_open;
int vmap_has(const void *p);
int vmap_read(const void *p, void *dst, size_t n);
void *mmap_from(uintptr_t lr, void *addr, size_t len, int prot, int flags, int fd, off_t off);
void *mmap_b(void *addr, size_t len, int prot, int flags, int fd, off_t off);
int munmap_b(void *addr, size_t len);
long sysconf_b(int name);

int *errno_b(void);
void assert_b(const char *file, int line, const char *expr);
void abort_b(void);
void stack_chk_fail_b(void);
int pause_b(void);
int utimes_b(const char *path, const void *times);
int gethostname_b(char *name, size_t len);
uint32_t inet_addr_b(const char *cp);
int android_log_print(int prio, const char *tag, const char *fmt, ...);

void *dlopen_b(const char *name, int flags);
void *dlsym_b(void *handle, const char *sym);
int dlclose_b(void *handle);

int pthread_attr_init_b(void *attr);
int pthread_attr_setstacksize_b(void *attr, size_t size);
int pthread_create_b(void *thread, const void *attr, void *(*fn)(void *), void *arg);
int pthread_mutex_init_b(int *m, const void *attr);
int pthread_mutex_destroy_b(int *m);
int pthread_mutex_lock_b(int *m);
int pthread_mutex_trylock_b(int *m);
int pthread_mutex_unlock_b(int *m);
int pthread_cond_init_b(int *c, const void *attr);
int pthread_cond_destroy_b(int *c);
int pthread_cond_wait_b(int *c, int *m);
int pthread_cond_timedwait_b(int *c, int *m, const struct timespec *t);
int pthread_cond_signal_b(int *c);
int pthread_cond_broadcast_b(int *c);

int isfinitef_b(float f);
int isinff_b(float f);

#endif
