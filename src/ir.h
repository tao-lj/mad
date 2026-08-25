// MAD intermediate representation: a flat, linear IR sits between the token
// stream and the threaded Op array.  The IR enables compile-time constant
// folding, basic stack-depth checking, and provides a clean extension point
// for future passes (type inference, peephole, etc.) without touching the
// runtime dispatch loop.
#ifndef MAD_IR_H
#define MAD_IR_H

#include "tcode.h"

typedef enum {
    IR_CONST_I64, IR_CONST_U64, IR_CONST_F64, IR_CONST_STR,
    IR_PUSH_LABEL,  // push a first-class label value
    IR_VAR, IR_REF, IR_DEREF,
    IR_CAST,
    IR_ARITH, IR_CMP, IR_ASSIGN,
    IR_ALLOC, IR_HALLOC, IR_FREE,
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
} IrNode;

// Build IR from a function body token range.
// *out_map is a malloc'd array of size (body_end - body_start) mapping
// body-relative token index → IR index (or SIZE_MAX if none).  Caller must
// free *out_map after ir_lower.
IrNode *ir_build(VM *vm, FuncSym *fn, size_t *out_n, size_t **out_map);

// Constant-fold and eliminate dead pairs in-place.
size_t ir_optimize(IrNode *ir, size_t n);

// Basic stack-depth check (linear walk). Returns true if no errors.
bool ir_check(const IrNode *ir, size_t n, const FuncSym *fn);

// Lower IR to threaded Op array.  Resolves jump targets, fills dispatch
// labels, builds code_map.  The map (body-relative token index → op index)
// is rebuilt from ir_build's map and stored on fn->code_map.
void ir_lower(const IrNode *ir, size_t n, FuncSym *fn,
              const size_t *ir_map, size_t ir_map_n);

// Free the IR node array (names already transferred to fn->owned).
void ir_free(IrNode *ir, size_t n);

#endif
