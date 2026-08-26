#include "value.h"

#include "common.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *type_name(TypeKind t) {
    switch (t) {
    case T_I8:     return "i8";
    case T_U8:     return "u8";
    case T_I16:    return "i16";
    case T_U16:    return "u16";
    case T_I32:    return "i32";
    case T_U32:    return "u32";
    case T_I64:    return "i64";
    case T_U64:    return "u64";
    case T_F32:    return "f32";
    case T_F64:    return "f64";
    case T_BOOL:   return "bool";
    case T_CHAR:   return "char";
    case T_MEM:    return "mem";
    case T_MEMPTR: return "memptr";
    case T_PTR:    return "ptr";
    case T_LABEL:  return "label";
    case T_FUNC:   return "func";
    case T_FILE:   return "file";
    case T_NONE:   return "?";
    }
    return "?";
}

bool is_type_name(const char *s, TypeKind *out) {
    // Must be sorted longest-first within same length to avoid prefix matches.
    // All names here are 2-4 chars, no prefix ambiguity.
    if (strcmp(s, "i8") == 0)   { *out = T_I8;   return true; }
    if (strcmp(s, "u8") == 0)   { *out = T_U8;   return true; }
    if (strcmp(s, "i16") == 0)  { *out = T_I16;  return true; }
    if (strcmp(s, "u16") == 0)  { *out = T_U16;  return true; }
    if (strcmp(s, "i32") == 0)  { *out = T_I32;  return true; }
    if (strcmp(s, "u32") == 0)  { *out = T_U32;  return true; }
    if (strcmp(s, "i64") == 0)  { *out = T_I64;  return true; }
    if (strcmp(s, "u64") == 0)  { *out = T_U64;  return true; }
    if (strcmp(s, "f32") == 0)  { *out = T_F32;  return true; }
    if (strcmp(s, "f64") == 0 || strcmp(s, "double") == 0) { *out = T_F64; return true; }
    if (strcmp(s, "bool") == 0) { *out = T_BOOL; return true; }
    if (strcmp(s, "char") == 0) { *out = T_CHAR; return true; }
    if (strcmp(s, "mem") == 0)    { *out = T_MEM;    return true; }
    if (strcmp(s, "memptr") == 0) { *out = T_MEMPTR; return true; }
    if (strcmp(s, "ptr") == 0)    { *out = T_PTR;    return true; }
    if (strcmp(s, "label") == 0)  { *out = T_LABEL;  return true; }
    if (strcmp(s, "func") == 0)   { *out = T_FUNC;   return true; }
    if (strcmp(s, "file") == 0)   { *out = T_FILE;   return true; }
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

bool is_numeric(TypeKind t) {
    return t == T_I8  || t == T_U8  || t == T_I16 || t == T_U16 ||
           t == T_I32 || t == T_U32 || t == T_I64 || t == T_U64 ||
           t == T_F32 || t == T_F64 || t == T_BOOL || t == T_CHAR;
}

bool type_is_integer(TypeKind t) {
    return t == T_I8  || t == T_U8  || t == T_I16 || t == T_U16 ||
           t == T_I32 || t == T_U32 || t == T_I64 || t == T_U64 ||
           t == T_BOOL || t == T_CHAR;
}

bool type_is_float(TypeKind t) { return t == T_F32 || t == T_F64; }
bool type_is_i64(TypeKind t)   { return t == T_I64; }
bool type_is_f64(TypeKind t)   { return t == T_F64; }

size_t type_size(TypeKind t) {
    switch (t) {
    case T_I8:  case T_U8:  case T_BOOL: case T_CHAR: return 1;
    case T_I16: case T_U16: return 2;
    case T_I32: case T_U32: case T_F32: return 4;
    case T_I64: case T_U64: case T_F64: return 8;
    default: return 0;
    }
}

// ---------- Scalar constructors ----------

Value make_i8(int8_t x)         { return (Value){T_I8,  .as.i8  = x}; }
Value make_u8(uint8_t x)        { return (Value){T_U8,  .as.u8  = x}; }
Value make_i16(int16_t x)       { return (Value){T_I16, .as.i16 = x}; }
Value make_u16(uint16_t x)      { return (Value){T_U16, .as.u16 = x}; }
Value make_i32(int32_t x)       { return (Value){T_I32, .as.i32 = x}; }
Value make_u32(uint32_t x)      { return (Value){T_U32, .as.u32 = x}; }
Value make_i64(int64_t x)       { return (Value){T_I64, .as.i64 = x}; }
Value make_u64(uint64_t x)      { return (Value){T_U64, .as.u64 = x}; }
Value make_f32(float x)         { return (Value){T_F32, .as.f32 = x}; }
Value make_f64(double x)        { return (Value){T_F64, .as.f64 = x}; }
Value make_bool(bool x)         { return (Value){T_BOOL,.as.b   = x}; }
Value make_char(uint8_t x)      { return (Value){T_CHAR,.as.c   = x}; }

int64_t val_as_i64(Value v) {
    switch (v.type) {
    case T_I8:   return (int64_t)v.as.i8;
    case T_U8:   return (int64_t)v.as.u8;
    case T_I16:  return (int64_t)v.as.i16;
    case T_U16:  return (int64_t)v.as.u16;
    case T_I32:  return (int64_t)v.as.i32;
    case T_U32:  return (int64_t)v.as.u32;
    case T_I64:  return v.as.i64;
    case T_U64:  return (int64_t)v.as.u64;
    case T_F32:  return (int64_t)v.as.f32;
    case T_F64:  return (int64_t)v.as.f64;
    case T_BOOL: return (int64_t)v.as.b;
    case T_CHAR: return (int64_t)v.as.c;
    default:     return 0;
    }
}

double val_as_f64(Value v) {
    switch (v.type) {
    case T_F32:  return (double)v.as.f32;
    case T_F64:  return v.as.f64;
    case T_I8:   return (double)v.as.i8;
    case T_U8:   return (double)v.as.u8;
    case T_I16:  return (double)v.as.i16;
    case T_U16:  return (double)v.as.u16;
    case T_I32:  return (double)v.as.i32;
    case T_U32:  return (double)v.as.u32;
    case T_I64:  return (double)v.as.i64;
    case T_U64:  return (double)v.as.u64;
    case T_BOOL: return (double)v.as.b;
    case T_CHAR: return (double)v.as.c;
    default:     return 0.0;
    }
}

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

uint64_t mem_adopt(MemVec *mv, uint8_t *data, size_t len, bool heap, bool ro) {
    VEC_GROW(mv->v, mv->n, mv->cap, MemObj);
    MemObj *m = &mv->v[mv->n];
    m->data = data;
    m->len = len;
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
