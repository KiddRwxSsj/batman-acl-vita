#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <pthread.h>
#include <zlib.h>
#include <sys/time.h>
#include <psp2/kernel/clib.h>
#include <so_util/so_util.h>

#include "bionic.h"
#include "gfx.h"
#include "loader.h"
#include "util.h"
#include "zfast.h"

extern void *__aeabi_atexit;
extern void *__cxa_begin_cleanup;
extern void *__cxa_call_unexpected;
extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;
extern void *__cxa_pure_virtual;
extern void *__cxa_type_match;
extern void *_ZdaPv;
extern void *_ZdlPv;
extern void *_Znaj;

static uintptr_t stack_guard = 0x2a5f3c11;

so_default_dynlib ue3_imports[] = {
    { "__aeabi_atexit", (uintptr_t)&__aeabi_atexit },
    { "__cxa_begin_cleanup", (uintptr_t)&__cxa_begin_cleanup },
    { "__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected },
    { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire },
    { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release },
    { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual },
    { "__cxa_type_match", (uintptr_t)&__cxa_type_match },
    { "__gnu_Unwind_Find_exidx", (uintptr_t)&gnu_unwind_find_exidx },
    { "_ZdaPv", (uintptr_t)&_ZdaPv },
    { "_ZdlPv", (uintptr_t)&_ZdlPv },
    { "_Znaj", (uintptr_t)&_Znaj },

    { "__sF", (uintptr_t)&bionic_sF },
    { "_ctype_", (uintptr_t)&bionic_ctype },
    { "_toupper_tab_", (uintptr_t)&bionic_toupper_tab },
    { "__errno", (uintptr_t)&errno_b },
    { "__assert", (uintptr_t)&assert_b },
    { "__stack_chk_fail", (uintptr_t)&stack_chk_fail_b },
    { "__stack_chk_guard", (uintptr_t)&stack_guard },
    { "__android_log_print", (uintptr_t)&android_log_print },
    { "__isfinitef", (uintptr_t)&isfinitef_b },
    { "__isinff", (uintptr_t)&isinff_b },
    { "abort", (uintptr_t)&abort_b },
    { "raise", (uintptr_t)&ret0 },
    { "pause", (uintptr_t)&pause_b },
    { "sysconf", (uintptr_t)&sysconf_b },
    { "getpid", (uintptr_t)&ret1 },

    { "dlopen", (uintptr_t)&dlopen_b },
    { "dlsym", (uintptr_t)&dlsym_b },
    { "dlclose", (uintptr_t)&dlclose_b },

    { "malloc", (uintptr_t)&malloc_b },
    { "realloc", (uintptr_t)&realloc_b },
    { "free", (uintptr_t)&free_b },
    { "mmap", (uintptr_t)&mmap_b },
    { "munmap", (uintptr_t)&munmap_b },

    { "memcmp", (uintptr_t)&memcmp },
    { "memcpy", (uintptr_t)&sceClibMemcpy },
    { "memmove", (uintptr_t)&sceClibMemmove },
    { "memset", (uintptr_t)&sceClibMemset },
    { "strcasecmp", (uintptr_t)&strcasecmp },
    { "strcat", (uintptr_t)&strcat },
    { "strchr", (uintptr_t)&strchr },
    { "strcmp", (uintptr_t)&strcmp },
    { "strcpy", (uintptr_t)&strcpy },
    { "strlen", (uintptr_t)&strlen },
    { "strncasecmp", (uintptr_t)&strncasecmp },
    { "strncmp", (uintptr_t)&strncmp },
    { "strncpy", (uintptr_t)&strncpy },
    { "strrchr", (uintptr_t)&strrchr },
    { "strstr", (uintptr_t)&strstr },
    { "strtod", (uintptr_t)&strtod },
    { "strtoul", (uintptr_t)&strtoul },
    { "strtoull", (uintptr_t)&strtoull },
    { "isalpha", (uintptr_t)&isalpha },
    { "qsort", (uintptr_t)&qsort },
    { "snprintf", (uintptr_t)&snprintf },
    { "sprintf", (uintptr_t)&sprintf },
    { "vsnprintf", (uintptr_t)&vsnprintf },
    { "vprintf", (uintptr_t)&vprintf },
    { "vscanf", (uintptr_t)&vscanf },
    { "puts", (uintptr_t)&puts },
    { "lrand48", (uintptr_t)&lrand48 },
    { "srand48", (uintptr_t)&srand48 },

    { "acosf", (uintptr_t)&acosf },
    { "asinf", (uintptr_t)&asinf },
    { "atan2f", (uintptr_t)&atan2f },
    { "atanf", (uintptr_t)&atanf },
    { "ceilf", (uintptr_t)&ceilf },
    { "cos", (uintptr_t)&cos },
    { "cosf", (uintptr_t)&cosf },
    { "expf", (uintptr_t)&expf },
    { "floor", (uintptr_t)&floor },
    { "floorf", (uintptr_t)&floorf },
    { "fmod", (uintptr_t)&fmod },
    { "fmodf", (uintptr_t)&fmodf },
    { "frexp", (uintptr_t)&frexp },
    { "ldexp", (uintptr_t)&ldexp },
    { "log10f", (uintptr_t)&log10f },
    { "logf", (uintptr_t)&logf },
    { "pow", (uintptr_t)&pow },
    { "powf", (uintptr_t)&powf },
    { "roundf", (uintptr_t)&roundf },
    { "sin", (uintptr_t)&sin },
    { "sinf", (uintptr_t)&sinf },
    { "sqrt", (uintptr_t)&sqrt },
    { "sqrtf", (uintptr_t)&sqrtf },
    { "tanf", (uintptr_t)&tanf },

    { "difftime", (uintptr_t)&difftime },
    { "gettimeofday", (uintptr_t)&gettimeofday },
    { "gmtime", (uintptr_t)&gmtime },
    { "gmtime_r", (uintptr_t)&gmtime_r },
    { "localtime_r", (uintptr_t)&localtime_r },
    { "time", (uintptr_t)&time },
    { "usleep", (uintptr_t)&usleep },
    { "sched_yield", (uintptr_t)&sched_yield },

    { "open", (uintptr_t)&open_b },
    { "close", (uintptr_t)&close },
    { "read", (uintptr_t)&read },
    { "write", (uintptr_t)&write },
    { "lseek", (uintptr_t)&lseek },
    { "stat", (uintptr_t)&stat_b },
    { "fstat", (uintptr_t)&fstat_b },
    { "access", (uintptr_t)&access_b },
    { "mkdir", (uintptr_t)&mkdir_b },
    { "rmdir", (uintptr_t)&rmdir_b },
    { "unlink", (uintptr_t)&unlink_b },
    { "remove", (uintptr_t)&remove_b },
    { "rename", (uintptr_t)&rename_b },
    { "chdir", (uintptr_t)&chdir_b },
    { "chmod", (uintptr_t)&chmod_b },
    { "utimes", (uintptr_t)&utimes_b },
    { "opendir", (uintptr_t)&opendir_b },
    { "readdir", (uintptr_t)&readdir_b },
    { "closedir", (uintptr_t)&closedir_b },
    { "fcntl", (uintptr_t)&ret0 },
    { "ioctl", (uintptr_t)&ret0 },

    { "fopen", (uintptr_t)&fopen_b },
    { "fclose", (uintptr_t)&fclose_b },
    { "fflush", (uintptr_t)&fflush_b },
    { "fgets", (uintptr_t)&fgets_b },
    { "fprintf", (uintptr_t)&fprintf_b },
    { "fread", (uintptr_t)&fread_b },
    { "fwrite", (uintptr_t)&fwrite_b },
    { "fseek", (uintptr_t)&fseek_b },
    { "ftell", (uintptr_t)&ftell_b },
    { "rewind", (uintptr_t)&rewind_b },

    { "compress", (uintptr_t)&compress },
    { "uncompress", (uintptr_t)&zf_uncompress },
    { "inflate", (uintptr_t)&zf_inflate },
    { "inflateEnd", (uintptr_t)&zf_inflateEnd },
    { "inflateInit_", (uintptr_t)&zf_inflateInit_ },

    { "pthread_attr_init", (uintptr_t)&pthread_attr_init_b },
    { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_b },
    { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_b },
    { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_b },
    { "pthread_cond_init", (uintptr_t)&pthread_cond_init_b },
    { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_b },
    { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_b },
    { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_b },
    { "pthread_create", (uintptr_t)&pthread_create_b },
    { "pthread_detach", (uintptr_t)&pthread_detach },
    { "pthread_exit", (uintptr_t)&pthread_exit },
    { "pthread_getspecific", (uintptr_t)&pthread_getspecific },
    { "pthread_join", (uintptr_t)&pthread_join },
    { "pthread_key_create", (uintptr_t)&pthread_key_create },
    { "pthread_key_delete", (uintptr_t)&pthread_key_delete },
    { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy },
    { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init },
    { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype },
    { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_b },
    { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_b },
    { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_b },
    { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_b },
    { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_b },
    { "pthread_self", (uintptr_t)&pthread_self },
    { "pthread_setspecific", (uintptr_t)&pthread_setspecific },

    // no networking, every socket call just fails
    { "socket", (uintptr_t)&retm1 },
    { "accept", (uintptr_t)&retm1 },
    { "bind", (uintptr_t)&retm1 },
    { "connect", (uintptr_t)&retm1 },
    { "listen", (uintptr_t)&retm1 },
    { "select", (uintptr_t)&retm1 },
    { "recv", (uintptr_t)&retm1 },
    { "recvfrom", (uintptr_t)&retm1 },
    { "send", (uintptr_t)&retm1 },
    { "sendto", (uintptr_t)&retm1 },
    { "getsockname", (uintptr_t)&retm1 },
    { "getsockopt", (uintptr_t)&retm1 },
    { "setsockopt", (uintptr_t)&retm1 },
    { "getaddrinfo", (uintptr_t)&retm1 },
    { "freeaddrinfo", (uintptr_t)&ret0 },
    { "gethostname", (uintptr_t)&gethostname_b },
    { "inet_addr", (uintptr_t)&inet_addr_b },

    { "eglGetDisplay", (uintptr_t)&egl_get_display },
    { "eglGetCurrentSurface", (uintptr_t)&egl_get_current_surface },
    { "eglSwapBuffers", (uintptr_t)&egl_swap_buffers },
    { "glGetString", (uintptr_t)&gl_get_string },
    { "glBlendColor", (uintptr_t)&gl_blend_color },
    { "glDetachShader", (uintptr_t)&gl_detach_shader },
    { "glLinkProgram", (uintptr_t)&gl_link_program },
};

int ue3_imports_size = sizeof(ue3_imports);
