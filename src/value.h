// Runtime values: inline scalars, memory objects, pointers, data stack.
#ifndef MAD_VALUE_H
#define MAD_VALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    T_NONE,  // unknown / not yet determined (compile-time sentinel)
    T_I8, T_U8,
    T_I16, T_U16,
    T_I32, T_U32,
    T_I64, T_U64,
    T_F32, T_F64,
    T_BOOL, T_CHAR,
    T_MEM, T_MEMPTR, T_PTR, T_LABEL, T_FUNC
} TypeKind;

const char *type_name(TypeKind t);

// Maps "i8"/"u8"/"i64"/"f64"/... to a TypeKind. Returns false for unknown names.
bool is_type_name(const char *s, TypeKind *out);

// Splits "name@type" into the base name plus an optional type annotation.
// Returns the annotation type (T_I64 when absent or unrecognized).
TypeKind split_annotated_name(const char *tok, char *base, size_t base_sz,
                              bool *has_type, TypeKind *ty);

// Returns true for all scalar types (integer, float, bool, char).
bool is_numeric(TypeKind t);

// Type family queries — classify types by operation family.
bool type_is_integer(TypeKind t);  // i8..u64, bool, char
bool type_is_float(TypeKind t);    // f32, f64
bool type_is_i64(TypeKind t);      // exactly i64
bool type_is_f64(TypeKind t);      // exactly f64

// Byte size of a scalar type (for mread/write bounds checks).
size_t type_size(TypeKind t);

// A data-stack value: scalars stored inline, handles (mem/ptr/label/func)
// stored as an opaque u64 id.
typedef struct {
    TypeKind type;
    union {
        int8_t   i8;
        uint8_t  u8;
        int16_t  i16;
        uint16_t u16;
        int32_t  i32;
        uint32_t u32;
        int64_t  i64;
        uint64_t u64;
        float    f32;
        double   f64;
        bool     b;
        uint8_t  c;  // char (semantically distinct from u8)
    } as;
} Value;

// Scalar constructors — no pool needed, value is inline.
Value make_i8(int8_t x);
Value make_u8(uint8_t x);
Value make_i16(int16_t x);
Value make_u16(uint16_t x);
Value make_i32(int32_t x);
Value make_u32(uint32_t x);
Value make_i64(int64_t x);
Value make_u64(uint64_t x);
Value make_f32(float x);
Value make_f64(double x);
Value make_bool(bool x);
Value make_char(uint8_t x);

// Extract an int64 from any integer/bool/char value (sign-extended).
int64_t val_as_i64(Value v);

// Extract a double from any numeric value.
double val_as_f64(Value v);

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
