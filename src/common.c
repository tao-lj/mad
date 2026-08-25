#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Noreturn void die_oom(void) {
    fprintf(stderr, "MAD: out of memory\n");
    exit(2);
}

char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) die_oom();
    memcpy(p, s, n);
    return p;
}

_Noreturn void fatal_at(size_t line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "MAD:%zu: ", line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "MAD: cannot open %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) die_oom();
    char *s = malloc((size_t)n + 1);
    if (!s) die_oom();
    size_t got = fread(s, 1, (size_t)n, f);
    fclose(f);
    s[got] = '\0';
    return s;
}

char *path_dir_of(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1; // "/foo" -> "/"
    char *dir = malloc(n + 1);
    if (!dir) die_oom();
    memcpy(dir, path, n);
    dir[n] = '\0';
    return dir;
}

static void path_push_seg(const char **segs, size_t *lens, size_t *n,
                          const char *s, size_t len) {
    if (len == 0 || (len == 1 && s[0] == '.')) return;
    if (len == 2 && s[0] == '.' && s[1] == '.') {
        if (*n > 0) --*n; // ".." pops; at the root it is a no-op
        return;
    }
    segs[*n] = s;
    lens[*n] = len;
    ++*n;
}

// Lexically resolves "raw" against directory "base": collapses duplicate
// slashes, drops "." and pops on "..". Purely textual -- symlinks are not
// followed -- which is enough to recognize repeated imports of one file.
char *path_canonical(const char *base, const char *raw) {
    // A relative raw inherits the rootedness of its base directory.
    bool raw_abs = raw[0] == '/';
    bool absolute = raw_abs || base[0] == '/';
    const char *srcs[2] = {base, raw};
    size_t nsrc = raw_abs ? 1 : 2; // absolute paths ignore the base

    const char *segs[MAX_PATH_SEGS];
    size_t lens[MAX_PATH_SEGS], n = 0;
    for (size_t i = 0; i < nsrc && n < MAX_PATH_SEGS; ++i) {
        const char *p = srcs[i];
        while (*p && n < MAX_PATH_SEGS) {
            while (*p == '/') ++p;
            const char *start = p;
            while (*p && *p != '/') ++p;
            path_push_seg(segs, lens, &n, start, (size_t)(p - start));
        }
    }
    if (n >= MAX_PATH_SEGS) return NULL; // too deep to canonicalize

    size_t need = absolute ? 1 : 0;
    for (size_t i = 0; i < n; ++i) need += lens[i] + 1;
    char *out = malloc(need + 2);
    if (!out) die_oom();
    char *w = out;
    if (absolute) *w++ = '/';
    for (size_t i = 0; i < n; ++i) {
        if (i) *w++ = '/';
        memcpy(w, segs[i], lens[i]);
        w += lens[i];
    }
    if (w == out) *w++ = '.';
    *w = '\0';
    return out;
}

bool word_is(const char *s, const char *kw) { return strcmp(s, kw) == 0; }
