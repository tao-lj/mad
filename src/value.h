// Runtime values: inline scalars, memory objects, pointers, data stack.
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

// A data-stack value: scalars stored inline, handles (mem/ptr/label/func)
// stored as an opaque u64 id.
typedef struct {
    TypeKind type;
    union {
        int64_t  i;
        uint64_t u;
        double   d;
        bool     b;
        uint8_t  c;
    } as;
} Value;

// Scalar constructors — no pool needed, value is inline.
Value make_i64(int64_t x);
Value make_u64(uint64_t x);
Value make_f64(double x);
Value make_bool(bool x);
Value make_char(uint8_t x);

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
