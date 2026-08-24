// Runtime values: typed pools, memory objects, pointers, data stack.
#ifndef MAD_VALUE_H
#define MAD_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

const char *type_name(TypeKind t);

// Maps "i64"/"u64"/... to a TypeKind. Returns false for unknown names.
bool is_type_name(const char *s, TypeKind *out);

// Splits "name@type" into the base name plus an optional type annotation.
// Returns the annotation type (T_I64 when absent or unrecognized).
TypeKind split_annotated_name(const char *tok, char *base, size_t base_sz,
                              bool *has_type, TypeKind *ty);

// A data-stack value is just (type, idx); idx indexes the matching pool below.
typedef struct {
    TypeKind type;
    uint64_t idx;
} Value;

typedef struct { int64_t *v; size_t n, cap; } I64Pool;
typedef struct { uint64_t *v; size_t n, cap; } U64Pool;
typedef struct { double *v; size_t n, cap; } F64Pool;
typedef struct { uint8_t *v; size_t n, cap; } BytePool;

uint64_t i64_new(I64Pool *p, int64_t x);
uint64_t u64_new(U64Pool *p, uint64_t x);
uint64_t f64_new(F64Pool *p, double x);
uint64_t byte_new(BytePool *p, uint8_t x);

Value make_i64(I64Pool *p, int64_t x);
Value make_u64(U64Pool *p, uint64_t x);
Value make_f64(F64Pool *p, double x);
Value make_bool(BytePool *p, bool x);
Value make_char(BytePool *p, uint8_t x);

// ---------- Runtime memory ----------

typedef struct {
    uint8_t *data;
    size_t len;
    bool heap;      // heap-lifetime (halloc) vs frame-lifetime (alloc)
    bool readonly;  // string literals
    uint64_t id;
} MemObj;
typedef struct { MemObj *v; size_t n, cap; } MemVec;

uint64_t mem_new(MemVec *mv, size_t n, bool heap, bool ro);

// A ptr refers to a variable slot via a stable integer id.
typedef struct {
    size_t frame_id;
    size_t local_index;
    bool is_global;
    size_t global_index;
} PtrRef;
typedef struct { PtrRef *v; size_t n, cap; } PtrVec;

uint64_t ptr_new(PtrVec *pv, PtrRef p);

// A memptr refers to a memory object id.
typedef struct { uint64_t mem_id; } MemPtrRef;
typedef struct { MemPtrRef *v; size_t n, cap; } MemPtrVec;

uint64_t memptr_new(MemPtrVec *mv, uint64_t id);

// ---------- Data stack ----------

typedef struct { Value *v; size_t n, cap; } ValStack;

void valstack_push(ValStack *s, Value v);
// Both exit with "stack underflow at <where>" when empty.
Value valstack_pop(ValStack *s, const char *where);
Value valstack_peek(ValStack *s, const char *where);

#endif
