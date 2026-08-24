#include "value.h"

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *type_name(TypeKind t) {
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
    }
    return "?";
}

bool is_type_name(const char *s, TypeKind *out) {
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

TypeKind split_annotated_name(const char *tok, char *base, size_t base_sz,
                              bool *has_type, TypeKind *ty) {
    const char *at = strchr(tok, '@');
    if (!at) {
        snprintf(base, base_sz, "%s", tok);
        *has_type = false;
        return T_I64;
    }
    size_t n = (size_t)(at - tok);
    if (n == 0 || n + 1 > base_sz) return T_I64;
    memcpy(base, tok, n);
    base[n] = '\0';
    TypeKind t;
    if (!is_type_name(at + 1, &t)) return T_I64;
    *has_type = true;
    *ty = t;
    return t;
}

// ---------- Typed pools ----------

uint64_t i64_new(I64Pool *p, int64_t x) {
    VEC_GROW(p->v, p->n, p->cap, int64_t);
    p->v[p->n] = x;
    return p->n++;
}

uint64_t u64_new(U64Pool *p, uint64_t x) {
    VEC_GROW(p->v, p->n, p->cap, uint64_t);
    p->v[p->n] = x;
    return p->n++;
}

uint64_t f64_new(F64Pool *p, double x) {
    VEC_GROW(p->v, p->n, p->cap, double);
    p->v[p->n] = x;
    return p->n++;
}

uint64_t byte_new(BytePool *p, uint8_t x) {
    VEC_GROW(p->v, p->n, p->cap, uint8_t);
    p->v[p->n] = x;
    return p->n++;
}

Value make_i64(I64Pool *p, int64_t x) { return (Value){T_I64, i64_new(p, x)}; }
Value make_u64(U64Pool *p, uint64_t x) { return (Value){T_U64, u64_new(p, x)}; }
Value make_f64(F64Pool *p, double x) { return (Value){T_F64, f64_new(p, x)}; }
Value make_bool(BytePool *p, bool x) { return (Value){T_BOOL, byte_new(p, (uint8_t)(x ? 1 : 0))}; }
Value make_char(BytePool *p, uint8_t x) { return (Value){T_CHAR, byte_new(p, x)}; }

// ---------- Runtime memory ----------

uint64_t mem_new(MemVec *mv, size_t n, bool heap, bool ro) {
    VEC_GROW(mv->v, mv->n, mv->cap, MemObj);
    MemObj *m = &mv->v[mv->n];
    m->data = calloc(n ? n : 1, 1);
    if (!m->data) die_oom();
    m->len = n;
    m->heap = heap;
    m->readonly = ro;
    m->id = mv->n;
    return mv->n++;
}

// ---------- Pointers ----------

uint64_t ptr_new(PtrVec *pv, PtrRef p) {
    VEC_GROW(pv->v, pv->n, pv->cap, PtrRef);
    pv->v[pv->n] = p;
    return pv->n++;
}

uint64_t memptr_new(MemPtrVec *mv, uint64_t id) {
    VEC_GROW(mv->v, mv->n, mv->cap, MemPtrRef);
    mv->v[mv->n] = (MemPtrRef){id};
    return mv->n++;
}

// ---------- Data stack ----------

void valstack_push(ValStack *s, Value v) {
    VEC_GROW(s->v, s->n, s->cap, Value);
    s->v[s->n++] = v;
}

Value valstack_pop(ValStack *s, const char *where) {
    if (!s->n) {
        fprintf(stderr, "MAD: stack underflow at %s\n", where);
        exit(1);
    }
    return s->v[--s->n];
}

Value valstack_peek(ValStack *s, const char *where) {
    if (!s->n) {
        fprintf(stderr, "MAD: stack underflow at %s\n", where);
        exit(1);
    }
    return s->v[s->n - 1];
}
