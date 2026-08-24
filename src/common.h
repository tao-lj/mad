// Shared utilities: grow-only vectors, diagnostics, allocation, file IO.
#ifndef MAD_COMMON_H
#define MAD_COMMON_H

#include <stddef.h>
#include <stdint.h>

#define INITIAL_CAP 64
#define MAX_NAME 128

// Print "MAD: out of memory" and exit with status 2.
_Noreturn void die_oom(void);

// strdup that aborts on allocation failure.
char *xstrdup(const char *s);

// Print "MAD:<line>: <message>" to stderr and exit with status 1.
_Noreturn void fatal_at(size_t line, const char *fmt, ...);

// Read an entire file into a NUL-terminated heap buffer; exits on failure.
char *read_file(const char *path);

// Double the capacity of a grow-only vector when it is full.
#define VEC_GROW(ptr, n, cap, T) \
    do { \
        if ((n) == (cap)) { \
            size_t mad_nc_ = (cap) ? (cap) * 2 : INITIAL_CAP; \
            void *mad_np_ = realloc((ptr), mad_nc_ * sizeof(T)); \
            if (!mad_np_) die_oom(); \
            (ptr) = mad_np_; \
            (cap) = mad_nc_; \
        } \
    } while (0)

#endif
