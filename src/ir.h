// MAD intermediate representation: a flat, linear IR sits between the token
// stream and the threaded Op array.  The IR enables compile-time constant
// folding, basic stack-depth checking, and provides a clean extension point
// for future passes (type inference, peephole, etc.) without touching the
// runtime dispatch loop.
#ifndef MAD_IR_H
#define MAD_IR_H

#include "tcode.h"

typedef enum {
    IR_CONST_I8, IR_CONST_U8,
    IR_CONST_I16, IR_CONST_U16,
    IR_CONST_I32, IR_CONST_U32,
    IR_CONST_I64, IR_CONST_U64,
    IR_CONST_F32, IR_CONST_F64,
    IR_CONST_STR, IR_CONST_BOOL,
    IR_PUSH_LABEL,  // push a first-class label value
    IR_LOAD, IR_DECLARE, IR_REF, IR_DEREF,
    IR_CAST,
    // Typed arithmetic (hot paths: i64, f64)
    IR_ADD_I64, IR_ADD_F64,
    IR_SUB_I64, IR_SUB_F64,
    IR_MUL_I64, IR_MUL_F64,
    IR_DIV_I64, IR_DIV_F64,
    IR_MOD_I64,
    // Generic arithmetic (fallback: unknown or mixed types)
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    // Typed comparison (hot paths)
    IR_EQ_I64, IR_EQ_F64,
    IR_NE_I64, IR_NE_F64,
    IR_LT_I64, IR_LT_F64,
    IR_GT_I64, IR_GT_F64,
    IR_LE_I64, IR_LE_F64,
    IR_GE_I64, IR_GE_F64,
    // Generic comparison (fallback)
    IR_EQ, IR_NE, IR_LT, IR_GT, IR_LE, IR_GE,
    IR_ASSIGN,
    // Bitwise (integer only, typed i64 path + generic)
    IR_BITNOT, IR_LOGNOT,
    IR_SHL_I64, IR_SHR_I64,
    IR_AND_I64, IR_OR_I64, IR_XOR_I64,
    IR_SHL, IR_SHR, IR_AND, IR_OR, IR_XOR,
    IR_LOGAND, IR_LOGOR,
    IR_ALLOC, IR_HALLOC, IR_FREE, IR_SIZEOF,
    IR_MREAD, IR_WRITE,
    IR_PRINT, IR_PRINTLN, IR_PRINTSTR, IR_READ,
    IR_DUP, IR_DROP, IR_SWAP, IR_ASSERT,
    IR_IMPORT,
    IR_CALL, IR_CALL_IND,
    IR_JMP, IR_JZ, IR_JNZ,
    IR_JMP_DYN, IR_JZ_DYN, IR_JNZ_DYN,
    IR_RET, IR_HALT,
    IR_LABEL_DEF,
    IR_DEAD,       // placeholder for constant-folded / eliminated nodes
    IR_COUNT
} IrKind;

typedef struct {
    IrKind kind;
    const char *text;  // source spelling (pointer into shared token stream)
    size_t line;
    union {
        int64_t i;
        uint64_t u;
        double d;
        char *name;    // owned (tracked via fn->owned)
    } u;
    TypeKind ty;
    bool has_ty;       // type annotation present; also halloc flag for ALLOC
    int64_t aux;       // pre-resolved label id, -1 if absent
    int64_t aux2;      // pre-resolved func id, -1 if absent
    int64_t label_idx; // label index (in fn->labels.v) for LABEL_DEF,
                       // PUSH_LABEL, and static JMP/JZ/JNZ targets; -1 if N/A
} IrNode;

// ---------------------------------------------------------------------------
//  Stack state — type-aware verifier stack
// ---------------------------------------------------------------------------

typedef struct {
    TypeKind *v;
    size_t n;
    size_t cap;
} StackState;

void stack_push(StackState *s, TypeKind t);
bool stack_pop(StackState *s, TypeKind *out);
bool stack_peek(const StackState *s, TypeKind *out);
StackState stack_clone(const StackState *s);
bool stack_equal(const StackState *a, const StackState *b);

// ---------------------------------------------------------------------------
//  ir_apply — apply a single IR instruction to a StackState
//
//  Returns true on success, false on type/stack error (message printed to
//  stderr).  `fn` is needed for CALL/RET signature checking.
// ---------------------------------------------------------------------------

bool ir_apply(const IrNode *ir, StackState *stack, const FuncSym *fn);

// Build IR from a function body token range.
IrNode *ir_build(VM *vm, FuncSym *fn, size_t *out_n, size_t **out_map);

// Constant-fold and eliminate dead pairs in-place.
size_t ir_optimize(IrNode *ir, size_t n);

// Stack-depth check with label-aware propagation.
// Returns true if no errors found.
bool ir_check(const IrNode *ir, size_t n, FuncSym *fn);

// Lower IR to threaded Op array.
void ir_lower(const IrNode *ir, size_t n, FuncSym *fn,
              const size_t *ir_map, size_t ir_map_n);

// Free the IR node array (names already transferred to fn->owned).
void ir_free(IrNode *ir, size_t n);

#endif
