// MAD Ain't Disciplined - MVP interpreter
// Version: 0.2.1
// Prototype semantics: postfix/data-stack language, no AST.
// Build: cc -std=c17 -O2 -Wall -Wextra -pedantic mad.c -o mad

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define INITIAL_CAP 64
#define MAX_NAME 128
#define MAX_ERROR 512

// ---------- Types / Values ----------

typedef enum {
    T_I64,
    T_U64,
    T_F64,
    T_BOOL,
    T_CHAR,
    T_MEM,
    T_MEMPTR,
    T_PTR,
    T_LABEL,
    T_FUNC
} TypeKind;

static const char *type_name(TypeKind t) {
    switch (t) {
    case T_I64: return "i64";
    case T_U64: return "u64";
    case T_F64: return "f64";
    case T_BOOL: return "bool";
    case T_CHAR: return "char";
    case T_MEM: return "mem";
    case T_MEMPTR: return "memptr";
    case T_PTR: return "ptr";
    case T_LABEL: return "label";
    case T_FUNC: return "func";
    default: return "?";
    }
}

typedef struct {
    TypeKind type;
    uint64_t idx; // identity/index in the corresponding runtime pool
} Value;

// Concrete pools. The value stack itself only stores (type, idx).
typedef struct { int64_t *v; size_t n, cap; } I64Pool;
typedef struct { uint64_t *v; size_t n, cap; } U64Pool;
typedef struct { double *v; size_t n, cap; } F64Pool;
typedef struct { uint8_t *v; size_t n, cap; } BytePool;

static void die_oom(void) { fprintf(stderr, "MAD: out of memory\n"); exit(2); }

#define VEC_GROW(ptr, n, cap, T) \
    do { \
        if ((n) == (cap)) { \
            size_t nc = (cap) ? (cap) * 2 : INITIAL_CAP; \
            void *np = realloc((ptr), nc * sizeof(T)); \
            if (!np) die_oom(); \
            (ptr) = np; (cap) = nc; \
        } \
    } while (0)

static uint64_t i64_new(I64Pool *p, int64_t x) {
    VEC_GROW(p->v, p->n, p->cap, int64_t); p->v[p->n] = x; return p->n++;
}
static uint64_t u64_new(U64Pool *p, uint64_t x) {
    VEC_GROW(p->v, p->n, p->cap, uint64_t); p->v[p->n] = x; return p->n++;
}
static uint64_t f64_new(F64Pool *p, double x) {
    VEC_GROW(p->v, p->n, p->cap, double); p->v[p->n] = x; return p->n++;
}
static uint64_t byte_new(BytePool *p, uint8_t x) {
    VEC_GROW(p->v, p->n, p->cap, uint8_t); p->v[p->n] = x; return p->n++;
}

static Value make_i64(I64Pool *p, int64_t x) { return (Value){T_I64, i64_new(p, x)}; }
static Value make_u64(U64Pool *p, uint64_t x) { return (Value){T_U64, u64_new(p, x)}; }
static Value make_f64(F64Pool *p, double x) { return (Value){T_F64, f64_new(p, x)}; }
static Value make_bool(BytePool *p, bool x) { return (Value){T_BOOL, byte_new(p, (uint8_t)(x ? 1 : 0))}; }
static Value make_char(BytePool *p, uint8_t x) { return (Value){T_CHAR, byte_new(p, x)}; }

// ---------- Source tokens ----------

typedef enum {
    TOK_WORD,
    TOK_INT,
    TOK_UINT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_COLON,
    TOK_SEMI,
    TOK_LABEL,
    TOK_GLOBAL_REF,     // :{a b} encoded on function header only
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACK,
    TOK_RBRACK,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_PARAM_END
} TokKind;

typedef struct {
    TokKind kind;
    char *text;
    size_t line;
} Token;

typedef struct { Token *v; size_t n, cap; } TokenVec;

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) die_oom();
    memcpy(p, s, n);
    return p;
}

static void token_push(TokenVec *tv, TokKind k, const char *s, size_t line) {
    VEC_GROW(tv->v, tv->n, tv->cap, Token);
    tv->v[tv->n++] = (Token){k, xstrdup(s), line};
}

static bool is_ident_start(int c) { return isalpha(c) || c == '_'; }
static bool is_ident_char(int c) { return isalnum(c) || c == '_' || c == '@'; }
static bool is_var_token(const char *s) {
    if (!s || !*s || !is_ident_start((unsigned char)s[0])) return false;
    for (const char *p = s + 1; *p; ++p) {
        if (!is_ident_char((unsigned char)*p)) return false;
    }
    return true;
}

static bool is_type_name(const char *s, TypeKind *out) {
    if (strcmp(s, "i64") == 0) { *out = T_I64; return true; }
    if (strcmp(s, "u64") == 0) { *out = T_U64; return true; }
    if (strcmp(s, "f64") == 0 || strcmp(s, "double") == 0) { *out = T_F64; return true; }
    if (strcmp(s, "bool") == 0) { *out = T_BOOL; return true; }
    if (strcmp(s, "char") == 0) { *out = T_CHAR; return true; }
    if (strcmp(s, "mem") == 0) { *out = T_MEM; return true; }
    if (strcmp(s, "memptr") == 0) { *out = T_MEMPTR; return true; }
    if (strcmp(s, "ptr") == 0) { *out = T_PTR; return true; }
    if (strcmp(s, "label") == 0) { *out = T_LABEL; return true; }
    if (strcmp(s, "func") == 0) { *out = T_FUNC; return true; }
    return false;
}

// Add a token, but [] groups are expanded by reversing the tokens in-place.
// Nested [] is rejected. Parentheses are simply discarded.
static void lex_source(const char *src, TokenVec *out) {
    const char *p = src;
    size_t line = 1;
    bool in_bracket = false;
    TokenVec bracket = {0};

    while (*p) {
        if (*p == '\n') { ++line; ++p; continue; }
        if (isspace((unsigned char)*p)) { ++p; continue; }
        if (*p == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n') ++p;
            continue;
        }
        if (*p == '(' || *p == ')') { ++p; continue; }
        if (*p == '[') {
            if (in_bracket) { fprintf(stderr, "MAD:%zu: nested [] is not allowed\n", line); exit(1); }
            in_bracket = true; ++p; continue;
        }
        if (*p == ']') {
            if (!in_bracket) { fprintf(stderr, "MAD:%zu: unexpected ]\n", line); exit(1); }
            in_bracket = false;
            for (size_t i = bracket.n; i-- > 0; ) {
                token_push(out, bracket.v[i].kind, bracket.v[i].text, bracket.v[i].line);
            }
            token_push(out, TOK_PARAM_END, "]", line);
            for (size_t i = 0; i < bracket.n; ++i) free(bracket.v[i].text);
            free(bracket.v); bracket = (TokenVec){0};
            ++p; continue;
        }

        TokKind k = TOK_WORD;
        char buf[256]; size_t n = 0;
        const char *start = p;

        if (*p == ':' || *p == ';') {
            if (*p == ':' && p[1] == '{') {
                // Documented compact global capture syntax: :{a b}foo
                token_push(out, TOK_COLON, ":", line);
                p += 2;
                char gbuf[512]; size_t gn = 0;
                while (*p && *p != '}') {
                    if (gn + 1 >= sizeof(gbuf)) { fprintf(stderr, "MAD:%zu: global list too long\n", line); exit(1); }
                    gbuf[gn++] = *p++;
                }
                if (*p != '}') { fprintf(stderr, "MAD:%zu: unterminated global list\n", line); exit(1); }
                ++p; gbuf[gn] = '\0';
                token_push(out, TOK_GLOBAL_REF, gbuf, line);
                size_t nn = 0;
                while (*p && !isspace((unsigned char)*p) && *p != ';') {
                    if (nn + 1 >= sizeof(buf)) { fprintf(stderr, "MAD:%zu: function name too long\n", line); exit(1); }
                    buf[nn++] = *p++;
                }
                if (nn == 0) { fprintf(stderr, "MAD:%zu: function name expected after global list\n", line); exit(1); }
                buf[nn] = '\0';
                token_push(out, TOK_WORD, buf, line);
                continue;
            }
            k = (*p == ':') ? TOK_COLON : TOK_SEMI;
            buf[0] = *p; buf[1] = '\0'; ++p;
        } else if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    if (n + 2 >= sizeof(buf)) { fprintf(stderr, "MAD:%zu: string too long\n", line); exit(1); }
                    buf[n++] = *p++; buf[n++] = *p++; continue;
                }
                if (n + 1 >= sizeof(buf)) { fprintf(stderr, "MAD:%zu: string too long\n", line); exit(1); }
                if (*p == '\n') ++line;
                buf[n++] = *p++;
            }
            if (*p != '"') { fprintf(stderr, "MAD:%zu: unterminated string\n", line); exit(1); }
            ++p; buf[n] = '\0'; k = TOK_STRING;
        } else if (*p == '&' || (*p == '*' && is_ident_start((unsigned char)p[1]))) {
            // '&name' and '*name' are reference tokens. A standalone '*' is
            // deliberately NOT a reference: it is the multiplication operator.
            char sig = *p++;
            if (!is_ident_start((unsigned char)*p)) {
                fprintf(stderr, "MAD:%zu: bad reference token '%c'\n", line, sig);
                exit(1);
            }
            buf[n++] = sig;
            while (is_ident_char((unsigned char)*p)) {
                if (n + 1 >= sizeof(buf)) die_oom();
                buf[n++] = *p++;
            }
            buf[n] = '\0';
        } else if (*p == '!' && p[1] == '@') {
            p += 2; buf[n++] = '!'; buf[n++] = '@';
            while (is_ident_char((unsigned char)*p)) {
                if (n + 1 >= sizeof(buf)) die_oom();
                buf[n++] = *p++;
            }
            buf[n] = '\0';
        } else {
            while (*p && !isspace((unsigned char)*p) && !strchr("()[]", *p)) {
                if (n + 1 >= sizeof(buf)) { fprintf(stderr, "MAD:%zu: token too long\n", line); exit(1); }
                buf[n++] = *p++;
            }
            buf[n] = '\0';
            if (n == 0 && p != start) continue;
        }

        if (k == TOK_WORD) {
            size_t m = 0; bool has_dot = false; bool all_num = true;
            const char *q = buf;
            if (*q == '-' || *q == '+') ++q;
            for (; *q; ++q) {
                if (*q == '.') { if (has_dot) { all_num = false; break; } has_dot = true; }
                else if (!isdigit((unsigned char)*q)) { all_num = false; break; }
                ++m;
            }
            if (all_num && m > 0) k = has_dot ? TOK_FLOAT : TOK_INT;
            if (k == TOK_WORD && strncmp(buf, "u:", 2) == 0 && buf[2]) k = TOK_UINT;
        }

        Token *target = NULL;
        if (in_bracket) {
            VEC_GROW(bracket.v, bracket.n, bracket.cap, Token);
            bracket.v[bracket.n++] = (Token){k, xstrdup(buf), line};
            target = &bracket.v[bracket.n-1];
        } else {
            token_push(out, k, buf, line);
            target = &out->v[out->n - 1];
        }
        (void)target;
    }
    if (in_bracket) { fprintf(stderr, "MAD:%zu: unterminated []\n", line); exit(1); }
}

static void free_tokens(TokenVec *tv) {
    for (size_t i = 0; i < tv->n; ++i) free(tv->v[i].text);
    free(tv->v); *tv = (TokenVec){0};
}

// ---------- Runtime memory ----------

typedef struct {
    uint8_t *data;
    size_t len;
    bool heap;
    bool readonly;
    uint64_t id;
} MemObj;

typedef struct { MemObj *v; size_t n, cap; } MemVec;

static uint64_t mem_new(MemVec *mv, size_t n, bool heap, bool ro) {
    VEC_GROW(mv->v, mv->n, mv->cap, MemObj);
    MemObj *m = &mv->v[mv->n];
    m->data = calloc(n ? n : 1, 1);
    if (!m->data) die_oom();
    m->len = n; m->heap = heap; m->readonly = ro; m->id = mv->n;
    return mv->n++;
}

// ---------- Symbols ----------

typedef struct { char *name; TypeKind type; bool initialized; Value value; } Var;
typedef struct { Var *v; size_t n, cap; } VarVec;

typedef struct { char *name; size_t token_index; } LabelSym;
typedef struct { LabelSym *v; size_t n, cap; } LabelVec;

typedef struct {
    char *name;
    size_t body_start, body_end;
    char **params;
    TypeKind *param_types;
    size_t param_count;
    char **globals;
    size_t global_count;
    LabelVec labels;
} FuncSym;
typedef struct { FuncSym *v; size_t n, cap; } FuncVec;

static Var *find_var(VarVec *vv, const char *name) {
    for (size_t i = 0; i < vv->n; ++i) if (strcmp(vv->v[i].name, name) == 0) return &vv->v[i];
    return NULL;
}

static Var *add_var(VarVec *vv, const char *name, TypeKind type, bool initialized, Value value) {
    if (find_var(vv, name)) return NULL;
    VEC_GROW(vv->v, vv->n, vv->cap, Var);
    Var *v = &vv->v[vv->n++];
    v->name = xstrdup(name); v->type = type; v->initialized = initialized; v->value = value;
    return v;
}

static FuncSym *find_func(FuncVec *fv, const char *name) {
    for (size_t i = 0; i < fv->n; ++i) if (strcmp(fv->v[i].name, name) == 0) return &fv->v[i];
    return NULL;
}

// ---------- Stack / Frames ----------

typedef struct { Value *v; size_t n, cap; } ValStack;
static void push(ValStack *s, Value v) { VEC_GROW(s->v, s->n, s->cap, Value); s->v[s->n++] = v; }
static Value popv(ValStack *s, const char *where) {
    if (!s->n) { fprintf(stderr, "MAD: stack underflow at %s\n", where); exit(1); }
    return s->v[--s->n];
}
static Value peekv(ValStack *s, const char *where) {
    if (!s->n) { fprintf(stderr, "MAD: stack underflow at %s\n", where); exit(1); }
    return s->v[s->n - 1];
}

// A pointer is represented as ptr(multi-level variable reference) via a stable global integer ID.
typedef struct { size_t frame_id; size_t local_index; bool is_global; size_t global_index; } PtrRef;
typedef struct { PtrRef *v; size_t n, cap; } PtrVec;
static uint64_t ptr_new(PtrVec *pv, PtrRef p) {
    VEC_GROW(pv->v, pv->n, pv->cap, PtrRef); pv->v[pv->n] = p; return pv->n++;
}

// memptr points to a memory object ID.
typedef struct { uint64_t mem_id; } MemPtrRef;
typedef struct { MemPtrRef *v; size_t n, cap; } MemPtrVec;
static uint64_t memptr_new(MemPtrVec *mv, uint64_t id) {
    VEC_GROW(mv->v, mv->n, mv->cap, MemPtrRef); mv->v[mv->n] = (MemPtrRef){id}; return mv->n++;
}

typedef struct {
    FuncSym *fn;
    size_t pc;
    VarVec locals;
    uint64_t *local_mems;
    size_t local_mem_n, local_mem_cap;
    size_t frame_id;
} Frame;
typedef struct { Frame *v; size_t n, cap; } FrameVec;

// ---------- VM ----------

typedef struct {
    TokenVec toks;
    I64Pool i64;
    U64Pool u64;
    F64Pool f64;
    BytePool bytes;
    MemVec mems;
    PtrVec ptrs;
    MemPtrVec memptrs;
    VarVec globals;
    FuncVec funcs;
    ValStack stack;
    FrameVec frames;
    bool halted;
} VM;

static void frame_track_local_mem(Frame *fr, uint64_t id) {
    VEC_GROW(fr->local_mems, fr->local_mem_n, fr->local_mem_cap, uint64_t);
    fr->local_mems[fr->local_mem_n++] = id;
}

static void frame_release_local_mem(VM *vm, Frame *fr) {
    for (size_t i = 0; i < fr->local_mem_n; ++i) {
        uint64_t id = fr->local_mems[i];
        if (id < vm->mems.n && vm->mems.v[id].data && !vm->mems.v[id].heap) {
            free(vm->mems.v[id].data);
            vm->mems.v[id].data = NULL;
            vm->mems.v[id].len = 0;
        }
    }
    free(fr->local_mems);
    fr->local_mems = NULL; fr->local_mem_n = fr->local_mem_cap = 0;
}


static void runtime_error(const Token *t, const char *fmt, ...) {
    char msg[MAX_ERROR];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "MAD:%zu: %s\n", t->line, msg);
    exit(1);
}

static bool is_global_visible(Frame *fr, const char *name) {
    if (strcmp(fr->fn->name, "<top>") == 0) return true;
    for (size_t i = 0; i < fr->fn->global_count; ++i) if (strcmp(fr->fn->globals[i], name) == 0) return true;
    return false;
}

static bool is_global_declared(Frame *fr, const char *name) {
    if (strcmp(fr->fn->name, "<top>") == 0) return false;
    for (size_t i = 0; i < fr->fn->global_count; ++i)
        if (strcmp(fr->fn->globals[i], name) == 0) return true;
    return false;
}

static TypeKind parse_annotated_name(const char *tok, char *base, size_t base_sz, bool *has_type, TypeKind *ty) {
    const char *at = strchr(tok, '@');
    if (!at) {
        snprintf(base, base_sz, "%s", tok); *has_type = false; return T_I64;
    }
    size_t n = (size_t)(at - tok);
    if (n == 0 || n + 1 > base_sz) return T_I64;
    memcpy(base, tok, n); base[n] = '\0';
    TypeKind t;
    if (!is_type_name(at + 1, &t)) return T_I64;
    *has_type = true; *ty = t; return t;
}

static double value_to_f64(VM *vm, Value v, const Token *t) {
    if (v.type == T_F64) return vm->f64.v[v.idx];
    runtime_error(t, "expected f64, got %s", type_name(v.type));
    return 0;
}

static Var *resolve_var(VM *vm, Frame *fr, const char *name, bool *is_global) {
    Var *lv = find_var(&fr->locals, name);
    if (lv) { if (is_global) *is_global = false; return lv; }
    if (is_global_visible(fr, name)) {
        Var *gv = find_var(&vm->globals, name);
        if (!gv) return NULL;
        if (is_global) *is_global = true;
        return gv;
    }
    return NULL;
}

static Var *global_var(VM *vm, const char *name) { return find_var(&vm->globals, name); }

static void assign_var(VM *vm, Frame *fr, Var *v, bool is_global, Value value, const Token *t) {
    (void)vm; (void)fr;
    if (v->type != value.type) runtime_error(t, "type mismatch in assignment: %s <- %s", type_name(v->type), type_name(value.type));
    v->value = value; v->initialized = true; (void)is_global;
}

static Value load_var(VM *vm, Frame *fr, const char *name, const Token *t) {
    bool g = false;
    Var *v = resolve_var(vm, fr, name, &g);
    if (!v) {
        if (is_global_declared(fr, name)) runtime_error(t, "declared global variable '%s' does not exist", name);
        runtime_error(t, "unknown variable '%s'", name);
    }
    if (!v->initialized) runtime_error(t, "uninitialized variable '%s'", name);
    (void)g; return v->value;
}

static Value make_ptr_value(VM *vm, Frame *fr, const char *name, const Token *t) {
    bool g = false;
    Var *v = resolve_var(vm, fr, name, &g);
    if (!v) {
        if (is_global_declared(fr, name)) runtime_error(t, "declared global variable '%s' does not exist", name);
        runtime_error(t, "unknown variable '%s'", name);
    }
    if (g) {
        size_t gi = (size_t)(v - vm->globals.v);
        return (Value){T_PTR, ptr_new(&vm->ptrs, (PtrRef){0, 0, true, gi})};
    }
    size_t li = (size_t)(v - fr->locals.v);
    return (Value){T_PTR, ptr_new(&vm->ptrs, (PtrRef){fr->frame_id, li, false, 0})};
}

static Var *ptr_target(VM *vm, Frame *current, Value pv, bool *is_global, const Token *t) {
    if (pv.type != T_PTR) runtime_error(t, "expected ptr, got %s", type_name(pv.type));
    PtrRef r = vm->ptrs.v[pv.idx];
    if (r.is_global) {
        if (is_global) *is_global = true;
        if (r.global_index >= vm->globals.n) runtime_error(t, "dangling global pointer", NULL);
        return &vm->globals.v[r.global_index];
    }
    if (r.frame_id >= vm->frames.n) runtime_error(t, "dangling local pointer", NULL);
    Frame *owner = &vm->frames.v[r.frame_id];
    if (r.local_index >= owner->locals.n) runtime_error(t, "dangling local pointer", NULL);
    if (is_global) *is_global = false;
    (void)current;
    return &owner->locals.v[r.local_index];
}

static Value cast_value(VM *vm, Value v, TypeKind t, const Token *tok) {
    if (v.type == t) return v;
    switch (t) {
    case T_I64:
        if (v.type == T_U64) return make_i64(&vm->i64, (int64_t)vm->u64.v[v.idx]);
        if (v.type == T_F64) return make_i64(&vm->i64, (int64_t)vm->f64.v[v.idx]);
        if (v.type == T_BOOL || v.type == T_CHAR) return make_i64(&vm->i64, (int64_t)vm->bytes.v[v.idx]);
        break;
    case T_U64:
        if (v.type == T_I64) return make_u64(&vm->u64, (uint64_t)vm->i64.v[v.idx]);
        if (v.type == T_F64) return make_u64(&vm->u64, (uint64_t)vm->f64.v[v.idx]);
        if (v.type == T_BOOL || v.type == T_CHAR) return make_u64(&vm->u64, (uint64_t)vm->bytes.v[v.idx]);
        if (v.type == T_LABEL || v.type == T_FUNC) return make_u64(&vm->u64, v.idx);
        break;
    case T_F64:
        if (v.type == T_I64) return make_f64(&vm->f64, (double)vm->i64.v[v.idx]);
        if (v.type == T_U64) return make_f64(&vm->f64, (double)vm->u64.v[v.idx]);
        if (v.type == T_BOOL || v.type == T_CHAR) return make_f64(&vm->f64, (double)vm->bytes.v[v.idx]);
        break;
    case T_BOOL:
        if (v.type == T_I64) return make_bool(&vm->bytes, vm->i64.v[v.idx] != 0);
        if (v.type == T_U64) return make_bool(&vm->bytes, vm->u64.v[v.idx] != 0);
        break;
    case T_CHAR:
        if (v.type == T_U64) return make_char(&vm->bytes, (uint8_t)vm->u64.v[v.idx]);
        if (v.type == T_I64) return make_char(&vm->bytes, (uint8_t)vm->i64.v[v.idx]);
        break;
    case T_LABEL:
        // Explicit integer -> label is deliberately not supported.
        break;
    case T_FUNC:
        break;
    case T_MEM: case T_MEMPTR: case T_PTR:
        break;
    }
    runtime_error(tok, "unsupported cast from %s to %s", type_name(v.type));
    return v;
}

// ---------- Function discovery ----------

static void split_names_from_global_token(const char *s, char ***names, size_t *n) {
    char *tmp = xstrdup(s);
    char *p = tmp;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',') ++p;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',') ++p;
        char save = *p; *p = '\0';
        *names = realloc(*names, (*n + 1) * sizeof(char*)); if (!*names) die_oom();
        (*names)[(*n)++] = xstrdup(start);
        *p = save;
    }
    free(tmp);
}

static void collect_function_labels(VM *vm, FuncSym *fn) {
    for (size_t i = fn->body_start; i < fn->body_end; ++i) {
        if (vm->toks.v[i].kind == TOK_WORD) {
            const char *s = vm->toks.v[i].text;
            size_t L = strlen(s);
            if (L > 1 && s[L-1] == ':') {
                char *name = xstrdup(s); name[L-1] = '\0';
                if (!is_var_token(name)) { free(name); continue; }
                for (size_t li = 0; li < fn->labels.n; ++li) {
                    if (strcmp(fn->labels.v[li].name, name) == 0) {
                        fprintf(stderr, "MAD:%zu: duplicate label '%s' in function '%s'\n", vm->toks.v[i].line, name, fn->name);
                        free(name); exit(1);
                    }
                }
                VEC_GROW(fn->labels.v, fn->labels.n, fn->labels.cap, LabelSym);
                fn->labels.v[fn->labels.n++] = (LabelSym){name, i + 1};
            }
        }
    }
}

static LabelSym *find_label(FuncSym *fn, const char *name) {
    for (size_t i = 0; i < fn->labels.n; ++i) if (strcmp(fn->labels.v[i].name, name) == 0) return &fn->labels.v[i];
    return NULL;
}

static void discover_functions(VM *vm) {
    size_t i = 0;
    while (i < vm->toks.n) {
        Token *t = &vm->toks.v[i];
        if (t->kind != TOK_COLON) { ++i; continue; }
        if (i + 1 >= vm->toks.n || vm->toks.v[i+1].kind == TOK_SEMI) {
            fprintf(stderr, "MAD:%zu: function name expected after ':'\n", t->line); exit(1);
        }
        i++;
        if (vm->toks.v[i].kind == TOK_WORD && vm->toks.v[i].text[0] == '{') {
            fprintf(stderr, "MAD:%zu: global list must be attached directly to ':' (use :{a b}name)\n", vm->toks.v[i].line);
            exit(1);
        }
        char **globals = NULL; size_t global_n = 0;
        if (vm->toks.v[i].kind == TOK_GLOBAL_REF) {
            split_names_from_global_token(vm->toks.v[i].text, &globals, &global_n); ++i;
            if (i >= vm->toks.n || vm->toks.v[i].kind != TOK_WORD) {
                fprintf(stderr, "MAD:%zu: function name expected after global list\n", t->line); exit(1);
            }
        }
        const char *fname = vm->toks.v[i].text; ++i;
        if (find_func(&vm->funcs, fname)) { fprintf(stderr, "MAD:%zu: duplicate function '%s'\n", t->line, fname); exit(1); }

        char **params = NULL; TypeKind *ptypes = NULL; size_t pn = 0;
        bool has_param_group = false;
        if (i < vm->toks.n && vm->toks.v[i].kind == TOK_WORD) {
            // [] expansion leaves TOK_PARAM_END immediately after the reversed parameter names.
            // We only treat names as parameters when the group marker appears here.
            size_t j = i;
            while (j < vm->toks.n && vm->toks.v[j].kind == TOK_WORD) ++j;
            if (j < vm->toks.n && vm->toks.v[j].kind == TOK_PARAM_END) {
                has_param_group = true;
                while (i < j) {
                    const char *s = vm->toks.v[i].text;
                    char base[MAX_NAME]; bool has_ty = false; TypeKind ty = T_I64;
                    parse_annotated_name(s, base, sizeof(base), &has_ty, &ty);
                    if (!is_var_token(base)) { fprintf(stderr, "MAD:%zu: bad parameter '%s'\n", vm->toks.v[i].line, s); exit(1); }
                    params = realloc(params, (pn+1)*sizeof(char*));
                    ptypes = realloc(ptypes, (pn+1)*sizeof(TypeKind));
                    if (!params || !ptypes) die_oom();
                    params[pn] = xstrdup(base);
                    ptypes[pn] = has_ty ? ty : T_I64;
                    ++pn; ++i;
                }
                ++i; // consume TOK_PARAM_END
            }
        }
        (void)has_param_group;
        size_t body_start = i;
        size_t body_end = body_start;
        int depth = 1;
        while (body_end < vm->toks.n) {
            if (vm->toks.v[body_end].kind == TOK_COLON) { /* labels are words ending in :; COLON is only function start */ }
            if (vm->toks.v[body_end].kind == TOK_SEMI) { --depth; if (depth == 0) break; }
            ++body_end;
        }
        if (body_end >= vm->toks.n) { fprintf(stderr, "MAD:%zu: unterminated function '%s'\n", t->line, fname); exit(1); }
        VEC_GROW(vm->funcs.v, vm->funcs.n, vm->funcs.cap, FuncSym);
        FuncSym *fn = &vm->funcs.v[vm->funcs.n++];
        *fn = (FuncSym){0};
        fn->name=xstrdup(fname); fn->body_start=body_start; fn->body_end=body_end;
        fn->params=params; fn->param_types=ptypes; fn->param_count=pn;
        fn->globals=globals; fn->global_count=global_n;
        collect_function_labels(vm, fn);
        i = body_end + 1;
    }
}

// ---------- Execution ----------

static uint8_t decode_escape(char c) {
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case '0': return 0;
    case '\\': return '\\';
    case '"': return '"';
    default: return (uint8_t)c;
    }
}

static Value parse_literal(VM *vm, const Token *t) {
    if (t->kind == TOK_INT) return make_i64(&vm->i64, strtoll(t->text, NULL, 10));
    if (t->kind == TOK_UINT) return make_u64(&vm->u64, strtoull(t->text + 2, NULL, 10));
    if (t->kind == TOK_FLOAT) return make_f64(&vm->f64, strtod(t->text, NULL));
    if (t->kind == TOK_STRING) {
        size_t raw = strlen(t->text);
        uint8_t *tmp = malloc(raw + 1);
        if (!tmp) die_oom();
        size_t n = 0;
        for (size_t i = 0; i < raw; ++i) {
            if (t->text[i] == '\\' && i + 1 < raw) tmp[n++] = decode_escape(t->text[++i]);
            else tmp[n++] = (uint8_t)t->text[i];
        }
        tmp[n++] = 0;
        uint64_t mid = mem_new(&vm->mems, n, true, true);
        memcpy(vm->mems.v[mid].data, tmp, n);
        free(tmp);
        return (Value){T_MEMPTR, memptr_new(&vm->memptrs, mid)};
    }
    return (Value){T_I64, 0};
}

static void print_value(VM *vm, Value v) {
    switch (v.type) {
    case T_I64: printf("%" PRId64, vm->i64.v[v.idx]); break;
    case T_U64: printf("%" PRIu64, vm->u64.v[v.idx]); break;
    case T_F64: printf("%g", vm->f64.v[v.idx]); break;
    case T_BOOL: printf("%s", vm->bytes.v[v.idx] ? "true" : "false"); break;
    case T_CHAR: printf("%c", vm->bytes.v[v.idx]); break;
    case T_MEM: printf("<mem:%" PRIu64 ">", v.idx); break;
    case T_MEMPTR: printf("<memptr:%" PRIu64 ">", vm->memptrs.v[v.idx].mem_id); break;
    case T_PTR: printf("<ptr:%" PRIu64 ">", v.idx); break;
    case T_LABEL: printf("<label:%" PRIu64 ">", v.idx); break;
    case T_FUNC: printf("<func:%" PRIu64 ">", v.idx); break;
    }
}

static int64_t get_i64(VM *vm, Value v, const Token *t) {
    if (v.type != T_I64) runtime_error(t, "expected i64, got %s", type_name(v.type));
    return vm->i64.v[v.idx];
}

static bool is_true(VM *vm, Value v, const Token *t) {
    if (v.type == T_BOOL) return vm->bytes.v[v.idx] != 0;
    if (v.type == T_I64) return vm->i64.v[v.idx] != 0;
    if (v.type == T_U64) return vm->u64.v[v.idx] != 0;
    runtime_error(t, "expected boolean/integer condition, got %s", type_name(v.type));
    return false;
}

static void execute_function(VM *vm, FuncSym *fn);

static void call_by_value(VM *vm, FuncSym *fn, const Token *t) {
    if (vm->stack.n < fn->param_count) runtime_error(t, "not enough arguments for function '%s'", fn->name);
    VEC_GROW(vm->frames.v, vm->frames.n, vm->frames.cap, Frame);
    size_t frame_id = vm->frames.n;
    vm->frames.v[frame_id] = (Frame){0};
    vm->frames.v[frame_id].fn = fn;
    vm->frames.v[frame_id].pc = fn->body_start;
    vm->frames.v[frame_id].frame_id = frame_id;
    vm->frames.n++;
    // [] reverses the source group, so the first runtime argument is popped first.
    for (size_t i = 0; i < fn->param_count; ++i) {
        Value v = popv(&vm->stack, t->text);
        if (fn->param_types[i] != v.type)
            runtime_error(t, "argument %zu of '%s' has type %s, expected %s",
                          i + 1, fn->name, type_name(v.type), type_name(fn->param_types[i]));
        if (global_var(vm, fn->params[i]))
            runtime_error(t, "local parameter '%s' conflicts with global variable", fn->params[i]);
        if (find_var(&vm->frames.v[frame_id].locals, fn->params[i]))
            runtime_error(t, "duplicate parameter '%s'", fn->params[i]);
        add_var(&vm->frames.v[frame_id].locals, fn->params[i], fn->param_types[i], true, v);
    }
    execute_function(vm, fn);
    frame_release_local_mem(vm, &vm->frames.v[frame_id]);
    vm->frames.n = frame_id;
}

static void do_stdin_read(VM *vm, const Token *t) {
    const char *at = strchr(t->text, '@');
    if (!at || !at[1]) runtime_error(t, "read requires type, e.g. read@i64", NULL);

    TypeKind ty;
    if (!is_type_name(at + 1, &ty)) runtime_error(t, "unknown input type '%s'", at + 1);

    if (ty == T_I64) {
        int64_t x;
        if (scanf("%" SCNd64, &x) != 1) runtime_error(t, "failed to read i64", NULL);
        push(&vm->stack, make_i64(&vm->i64, x));
    } else if (ty == T_U64) {
        uint64_t x;
        if (scanf("%" SCNu64, &x) != 1) runtime_error(t, "failed to read u64", NULL);
        push(&vm->stack, make_u64(&vm->u64, x));
    } else if (ty == T_F64) {
        double x;
        if (scanf("%lf", &x) != 1) runtime_error(t, "failed to read f64", NULL);
        push(&vm->stack, make_f64(&vm->f64, x));
    } else if (ty == T_CHAR) {
        unsigned char c;
        if (scanf(" %c", &c) != 1) runtime_error(t, "failed to read char", NULL);
        push(&vm->stack, make_char(&vm->bytes, c));
    } else if (ty == T_BOOL) {
        char buf[64];
        if (scanf("%63s", buf) != 1) runtime_error(t, "failed to read bool", NULL);
        if (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0) {
            push(&vm->stack, make_bool(&vm->bytes, true));
        } else if (strcmp(buf, "false") == 0 || strcmp(buf, "0") == 0) {
            push(&vm->stack, make_bool(&vm->bytes, false));
        } else {
            runtime_error(t, "invalid bool input '%s'", buf);
        }
    } else {
        runtime_error(t, "read@%s is not supported by the MVP input module", at + 1);
    }
}

static void do_mem_read(VM *vm, const Token *t) {
    Value off = popv(&vm->stack, "read offset");
    Value memv = popv(&vm->stack, "read mem");
    uint64_t mid;
    if (memv.type == T_MEM) mid = memv.idx;
    else if (memv.type == T_MEMPTR) mid = vm->memptrs.v[memv.idx].mem_id;
    else runtime_error(t, "read expects mem/memptr, got %s", type_name(memv.type));
    size_t o = (size_t)get_i64(vm, off, t);
    // Type is encoded as read@type token, but lexer keeps @ in word.
    const char *at = strchr(t->text, '@');
    if (!at || !at[1]) runtime_error(t, "mread requires type, e.g. mread@i64", NULL);
    TypeKind ty; if (!is_type_name(at+1, &ty)) runtime_error(t, "unknown read type '%s'", at+1);
    if (mid >= vm->mems.n || vm->mems.v[mid].data == NULL) runtime_error(t, "invalid or freed memory object", NULL);
    MemObj *m = &vm->mems.v[mid];
    size_t sz = 0;
    switch (ty) { case T_I64: case T_U64: sz=8; break; case T_F64: sz=8; break; case T_BOOL: case T_CHAR: sz=1; break; default: runtime_error(t,"mread supports scalar types only, got %s",type_name(ty)); }
    if (o + sz > m->len) runtime_error(t, "read out of bounds", NULL);
    if (ty == T_I64) { int64_t x; memcpy(&x,m->data+o,8); push(&vm->stack,make_i64(&vm->i64,x)); }
    else if (ty == T_U64) { uint64_t x; memcpy(&x,m->data+o,8); push(&vm->stack,make_u64(&vm->u64,x)); }
    else if (ty == T_F64) { double x; memcpy(&x,m->data+o,8); push(&vm->stack,make_f64(&vm->f64,x)); }
    else if (ty == T_BOOL) push(&vm->stack,make_bool(&vm->bytes,m->data[o]!=0));
    else push(&vm->stack,make_char(&vm->bytes,m->data[o]));
}

static void do_write(VM *vm, const Token *t) {
    Value off = popv(&vm->stack, "write offset");
    Value memv = popv(&vm->stack, "write mem");
    Value val = popv(&vm->stack, "write value");
    uint64_t mid;
    if (memv.type == T_MEM) mid = memv.idx;
    else if (memv.type == T_MEMPTR) mid = vm->memptrs.v[memv.idx].mem_id;
    else runtime_error(t, "write expects mem/memptr, got %s", type_name(memv.type));
    size_t o=(size_t)get_i64(vm,off,t); const char *at=strchr(t->text,'@');
    if(!at) runtime_error(t,"write requires type, e.g. write@i64",NULL);
    TypeKind ty; if(!is_type_name(at+1,&ty)) runtime_error(t,"unknown write type '%s'",at+1);
    if(val.type!=ty) runtime_error(t,"write type mismatch: value is %s but write@%s requested",type_name(val.type));
    if (mid >= vm->mems.n || vm->mems.v[mid].data == NULL) runtime_error(t,"invalid or freed memory object",NULL);
    MemObj*m=&vm->mems.v[mid]; if(m->readonly) runtime_error(t,"cannot write read-only memory",NULL);
    size_t sz=0; switch(ty){case T_I64:case T_U64:case T_F64:sz=8;break;case T_BOOL:case T_CHAR:sz=1;break;default:runtime_error(t,"write supports scalar types only, got %s",type_name(ty));}
    if(o+sz>m->len) runtime_error(t,"write out of bounds",NULL);
    if(ty==T_I64) memcpy(m->data+o,&vm->i64.v[val.idx],8);
    else if(ty==T_U64) memcpy(m->data+o,&vm->u64.v[val.idx],8);
    else if(ty==T_F64) memcpy(m->data+o,&vm->f64.v[val.idx],8);
    else m->data[o]=vm->bytes.v[val.idx];
}

static void execute_function(VM *vm, FuncSym *fn) {
    const size_t frame_id = vm->frames.n - 1;
    while (vm->frames.v[frame_id].pc < fn->body_end && !vm->halted) {
        Frame *fr = &vm->frames.v[frame_id];
        size_t ip = fr->pc++;
        Token *t = &vm->toks.v[ip];
        if (t->kind == TOK_SEMI) return;
        if (t->kind == TOK_PARAM_END) continue;
        if (t->kind == TOK_COLON && strcmp(fn->name, "<top>") == 0) {
            size_t j = ip + 1;
            while (j < fn->body_end && vm->toks.v[j].kind != TOK_SEMI) ++j;
            if (j >= fn->body_end) runtime_error(t, "unterminated function definition", NULL);
            fr->pc = j + 1;
            continue;
        }
        if (t->kind != TOK_WORD && t->kind != TOK_INT && t->kind != TOK_UINT && t->kind != TOK_FLOAT && t->kind != TOK_STRING) continue;
        if (t->kind != TOK_WORD) { push(&vm->stack, parse_literal(vm,t)); continue; }
        const char *s=t->text;
        size_t L=strlen(s);
        if(L>0 && s[L-1]==':') { continue; } // label definition
        if (s[0]=='&') {
            const char *name = s + 1;
            bool g = false;
            Var *vv = resolve_var(vm, fr, name, &g);
            if (vv) {
                if (vv->type == T_MEM) {
                    // MVP: &mem yields a memptr to the memory object stored in that variable.
                    if (!vv->initialized) runtime_error(t, "uninitialized variable '%s'", name);
                    if (vv->value.type != T_MEM) runtime_error(t, "internal mem variable type error", NULL);
                    push(&vm->stack, (Value){T_MEMPTR, memptr_new(&vm->memptrs, vv->value.idx)});
                } else {
                    push(&vm->stack, make_ptr_value(vm, fr, name, t));
                }
                continue;
            }
            LabelSym *ls = find_label(fn, name);
            if (ls) { push(&vm->stack, (Value){T_LABEL, (uint64_t)(ls - fn->labels.v)}); continue; }
            FuncSym *ff = find_func(&vm->funcs, name);
            if (ff) { push(&vm->stack, (Value){T_FUNC, (uint64_t)(ff - vm->funcs.v)}); continue; }
            runtime_error(t, "unknown reference '&%s'", name);
        }
        if (s[0]=='*' && s[1]) {
            Value pv=load_var(vm,fr,s+1,t);
            bool g=false; Var *target=ptr_target(vm,fr,pv,&g,t); (void)g;
            if(!target->initialized) runtime_error(t,"dereferenced uninitialized pointer '%s'",s+1);
            push(&vm->stack,target->value); continue;
        }
        if (strncmp(s,"!@",2)==0) { TypeKind ty; if(!is_type_name(s+2,&ty)) runtime_error(t,"unknown cast type '%s'",s+2); Value v=popv(&vm->stack,s); push(&vm->stack,cast_value(vm,v,ty,t)); continue; }

        LabelSym *early_ls = find_label(fn, s);
        if (early_ls) {
            size_t id = (size_t)(early_ls - fn->labels.v);
            push(&vm->stack, (Value){T_LABEL, id});
            continue;
        }
        FuncSym *early_callee = find_func(&vm->funcs, s);
        if (early_callee) {
            call_by_value(vm, early_callee, t);
            continue;
        }

        if (strcmp(s,"ret")==0) return;
        if (strcmp(s,"halt")==0) { vm->halted=true; return; }
        if (strcmp(s,"dup")==0) { push(&vm->stack,peekv(&vm->stack,s)); continue; }
        if (strcmp(s,"drop")==0) { (void)popv(&vm->stack,s); continue; }
        if (strcmp(s,"swap")==0) { Value a=popv(&vm->stack,s), b=popv(&vm->stack,s); push(&vm->stack,a); push(&vm->stack,b); continue; }
        if (strcmp(s,"+")==0 || strcmp(s,"-")==0 || strcmp(s,"*")==0 || strcmp(s,"/")==0 || strcmp(s,"%")==0) {
            Value b=popv(&vm->stack,s), a=popv(&vm->stack,s);
            if(a.type==T_F64 || b.type==T_F64){ double x=value_to_f64(vm,a,t), y=value_to_f64(vm,b,t), r=0; if(strcmp(s,"+")==0)r=x+y;else if(strcmp(s,"-")==0)r=x-y;else if(strcmp(s,"*")==0)r=x*y;else if(strcmp(s,"/")==0)r=x/y;else runtime_error(t,"% is not defined for f64",NULL); push(&vm->stack,make_f64(&vm->f64,r)); }
            else { int64_t x=get_i64(vm,a,t), y=get_i64(vm,b,t), r=0; if(strcmp(s,"+")==0)r=x+y;else if(strcmp(s,"-")==0)r=x-y;else if(strcmp(s,"*")==0)r=x*y;else if(strcmp(s,"/")==0){if(!y)runtime_error(t,"division by zero",NULL);r=x/y;}else{if(!y)runtime_error(t,"division by zero",NULL);r=x%y;} push(&vm->stack,make_i64(&vm->i64,r)); }
            continue;
        }
        if(strcmp(s,"==")==0||strcmp(s,"!=")==0||strcmp(s,"<")==0||strcmp(s,">")==0||strcmp(s,"<=")==0||strcmp(s,">=")==0){
            Value b=popv(&vm->stack,s),a=popv(&vm->stack,s); bool r=false;
            if(a.type==T_I64&&b.type==T_I64){int64_t x=vm->i64.v[a.idx],y=vm->i64.v[b.idx];if(strcmp(s,"==")==0)r=x==y;else if(strcmp(s,"!=")==0)r=x!=y;else if(strcmp(s,"<")==0)r=x<y;else if(strcmp(s,">")==0)r=x>y;else if(strcmp(s,"<=")==0)r=x<=y;else r=x>=y;}
            else if(a.type==T_F64&&b.type==T_F64){double x=vm->f64.v[a.idx],y=vm->f64.v[b.idx];if(strcmp(s,"==")==0)r=x==y;else if(strcmp(s,"!=")==0)r=x!=y;else if(strcmp(s,"<")==0)r=x<y;else if(strcmp(s,">")==0)r=x>y;else if(strcmp(s,"<=")==0)r=x<=y;else r=x>=y;}
            else runtime_error(t,"comparison requires equal scalar types, got %s",type_name(a.type));
            push(&vm->stack,make_bool(&vm->bytes,r)); continue;
        }
        if(strcmp(s,"=")==0){
            Value pv=popv(&vm->stack,s), val=popv(&vm->stack,s); bool g=false; Var*dst=ptr_target(vm,fr,pv,&g,t); assign_var(vm,fr,dst,g,val,t); continue;
        }
        if(strcmp(s,"call")==0){
            Value fv=popv(&vm->stack,s);
            if(fv.type!=T_FUNC) runtime_error(t,"call expects func, got %s",type_name(fv.type));
            if(fv.idx>=vm->funcs.n) runtime_error(t,"invalid func id",NULL);
            call_by_value(vm,&vm->funcs.v[fv.idx],t);
            continue;
        }
        if(strcmp(s,"jz")==0 || strcmp(s,"jmp")==0 || strcmp(s,"jump")==0){
            Value target=popv(&vm->stack,s);
            if(target.type!=T_LABEL) runtime_error(t,"%s expects label, got %s",s,type_name(target.type));
            if(target.idx>=fn->labels.n) runtime_error(t,"invalid label id",NULL);
            if(strcmp(s,"jz")==0){
                Value cond=popv(&vm->stack,s);
                if(!is_true(vm,cond,t)) continue;
            }
            fr->pc=fn->labels.v[target.idx].token_index; continue;
        }
        if(strcmp(s,"alloc")==0 || strcmp(s,"halloc")==0){ Value n=popv(&vm->stack,s); int64_t bytes=get_i64(vm,n,t); if(bytes<0)runtime_error(t,"negative allocation",NULL); bool heap=strcmp(s,"halloc")==0; uint64_t id=mem_new(&vm->mems,(size_t)bytes,heap,false); if(!heap) frame_track_local_mem(fr,id); push(&vm->stack,(Value){T_MEM,id}); continue; }
        if(strcmp(s,"free")==0){ Value m=popv(&vm->stack,s); uint64_t id; if(m.type==T_MEM)id=m.idx;else if(m.type==T_MEMPTR){if(m.idx>=vm->memptrs.n)runtime_error(t,"invalid memptr",NULL);id=vm->memptrs.v[m.idx].mem_id;}else runtime_error(t,"free expects mem/memptr, got %s",type_name(m.type)); if(id>=vm->mems.n || vm->mems.v[id].data==NULL)runtime_error(t,"invalid or already freed mem id",NULL); free(vm->mems.v[id].data);vm->mems.v[id].data=NULL;vm->mems.v[id].len=0;continue; }
        if(strncmp(s,"mread@",6)==0){ do_mem_read(vm,t); continue; }
        if(strncmp(s,"read@",5)==0){ do_stdin_read(vm,t); continue; }
        if(strncmp(s,"write@",6)==0){ do_write(vm,t); continue; }
        if(strcmp(s,"print")==0){ Value v=popv(&vm->stack,s); print_value(vm,v); continue; }
        if(strcmp(s,"printn")==0){ Value v=popv(&vm->stack,s); print_value(vm,v); continue; }
        if(strcmp(s,"println")==0){ putchar('\n'); continue; }
        if(strcmp(s,"printstr")==0){
            Value v=popv(&vm->stack,s);
            uint64_t mid;
            if(v.type==T_MEMPTR) mid=vm->memptrs.v[v.idx].mem_id;
            else if(v.type==T_MEM) mid=v.idx;
            else runtime_error(t,"printstr expects mem/memptr, got %s",type_name(v.type));
            MemObj *m=&vm->mems.v[mid];
            for(size_t k=0;k<m->len && m->data[k];++k) putchar((char)m->data[k]);
            continue;
        }
        if(strcmp(s,"assert")==0){ Value v=popv(&vm->stack,s); if(!is_true(vm,v,t))runtime_error(t,"assertion failed",NULL); continue; }

        // Label references are first-class values when prefixed with &label.
        LabelSym *ls=find_label(fn,s);
        if(ls){ size_t id=(size_t)(ls-fn->labels.v); push(&vm->stack,(Value){T_LABEL,id}); continue; }
        FuncSym *callee=find_func(&vm->funcs,s);
        if(callee){ call_by_value(vm,callee,t); continue; }

        char base[MAX_NAME]; bool has_ty=false; TypeKind ann=T_I64; TypeKind dummy=parse_annotated_name(s,base,sizeof(base),&has_ty,&ann); (void)dummy;
        if(!is_var_token(base)) runtime_error(t,"unknown token '%s'",s);
        bool g=false; Var *v=resolve_var(vm,fr,base,&g);
        if(v){
            if(has_ty && v->type!=ann) runtime_error(t,"variable '%s' already has type %s, not %s",base,type_name(v->type),type_name(ann));
            if(!v->initialized) runtime_error(t,"uninitialized variable '%s'",base);
            push(&vm->stack,v->value); continue;
        }
        if (is_global_declared(fr, base))
            runtime_error(t,"declared global variable '%s' does not exist",base);
        // Unknown variable token: implicit declaration by consuming stack top.
        Value init=popv(&vm->stack,s); TypeKind ty=has_ty?ann:init.type;
        if(ty!=init.type) runtime_error(t,"initializer type %s does not match declared type %s",type_name(init.type),type_name(ty));
        if(strcmp(fr->fn->name, "<top>") == 0) {
            if (find_var(&vm->globals, base)) runtime_error(t, "global variable '%s' already exists", base);
            add_var(&vm->globals, base, ty, true, init);
        } else {
            if(global_var(vm,base)) runtime_error(t,"local variable '%s' conflicts with global variable",base);
            add_var(&fr->locals,base,ty,true,init);
        }
    }
}

static void execute_top_level(VM *vm) {
    FuncSym top = {0};
    top.name = xstrdup("<top>");
    top.body_start = 0;
    top.body_end = vm->toks.n;
    collect_function_labels(vm, &top);
    VEC_GROW(vm->frames.v, vm->frames.n, vm->frames.cap, Frame);
    Frame *fr = &vm->frames.v[vm->frames.n];
    *fr = (Frame){0};
    fr->fn = &top;
    fr->pc = 0;
    fr->frame_id = vm->frames.n;
    vm->frames.n++;
    execute_function(vm, &top);
    vm->frames.n--;
    free(top.name);
}

// Global declarations are naturally created during top-level execution. For MVP, functions that declare globals
// must appear after those globals have already been defined.

static void free_vm(VM *vm) {
    free_tokens(&vm->toks);
    free(vm->i64.v); free(vm->u64.v); free(vm->f64.v); free(vm->bytes.v);
    for(size_t i=0;i<vm->mems.n;++i) free(vm->mems.v[i].data);
    free(vm->mems.v);
    free(vm->ptrs.v); free(vm->memptrs.v);
    for(size_t i=0;i<vm->globals.n;++i) free(vm->globals.v[i].name);
    free(vm->globals.v);
    for(size_t i=0;i<vm->funcs.n;++i){FuncSym*f=&vm->funcs.v[i];free(f->name);for(size_t j=0;j<f->param_count;++j)free(f->params[j]);free(f->params);free(f->param_types);for(size_t j=0;j<f->global_count;++j)free(f->globals[j]);free(f->globals);for(size_t j=0;j<f->labels.n;++j)free(f->labels.v[j].name);free(f->labels.v);}
    free(vm->funcs.v);free(vm->stack.v);
    for(size_t i=0;i<vm->frames.n;++i){
        for(size_t j=0;j<vm->frames.v[i].locals.n;++j) free(vm->frames.v[i].locals.v[j].name);
        free(vm->frames.v[i].locals.v);
        free(vm->frames.v[i].local_mems);
    }
    free(vm->frames.v);
}

static char *read_file(const char *path) {
    FILE *f=fopen(path,"rb"); if(!f){fprintf(stderr,"MAD: cannot open %s: %s\n",path,strerror(errno));exit(1);} fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); if(n<0)die_oom(); char *s=malloc((size_t)n+1);if(!s)die_oom(); size_t got=fread(s,1,(size_t)n,f);fclose(f);s[got]='\0';return s;
}

int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: %s file.mad\n",argv[0]);return 2;}
    char *src=read_file(argv[1]); VM vm={0};
    lex_source(src,&vm.toks); free(src);
    discover_functions(&vm);
    execute_top_level(&vm);
    free_vm(&vm); return 0;
}
