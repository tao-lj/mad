// Threaded-code instruction format and compiler/runtime boundary.
#ifndef MAD_TCODE_H
#define MAD_TCODE_H

#include "vm.h"

typedef enum {
    OP_PUSH_I8, OP_PUSH_U8,
    OP_PUSH_I16, OP_PUSH_U16,
    OP_PUSH_I32, OP_PUSH_U32,
    OP_PUSH_I64, OP_PUSH_U64,
    OP_PUSH_F32, OP_PUSH_F64,
    OP_PUSH_STR, OP_PUSH_BOOL,
    OP_PUSH_LABEL, OP_PUSH_FUNC,
    OP_WORD_VAR,   // plain word: load existing variable or declare from stack top
    OP_REF_NAME,   // &name: ptr/memptr to variable, else first-class label/func
    OP_DEREF_NAME, // *name
    OP_CAST,       // !@type
    OP_ARITH, OP_CMP, OP_ASSIGN,
    OP_ALLOC, OP_HALLOC, OP_FREE,
    OP_MREAD, OP_WRITE,
    OP_PRINT, OP_PRINTLN, OP_PRINTSTR, OP_READ,
    OP_DUP, OP_DROP, OP_SWAP, OP_ASSERT,
    OP_IMPORT,
    OP_CALL_FUNC, OP_CALL_IND,
    OP_JMP, OP_JZ, OP_JNZ,
    OP_JMP_DYN, OP_JZ_DYN, OP_JNZ_DYN,
    OP_RET, OP_HALT,
    OP_COUNT
} OpCode;

// Sub-kind payloads stored in op->u.i.
enum { AR_ADD, AR_SUB, AR_MUL, AR_DIV, AR_MOD };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_GT, CMP_LE, CMP_GE };

typedef struct Op {
    void *code;       // &&label inside execute_code(): the thread
    const char *text; // original spelling, reused in diagnostics/pop context
    size_t line;
    union {
        int64_t i;
        uint64_t u;
        double d;
        char *name;
    } u;
    TypeKind ty;
    bool has_ty;  // declared-type marker; doubles as the halloc flag
    int64_t aux;  // pre-resolved label id ('&name' fallback), -1 when absent
    int64_t aux2; // pre-resolved func id ('&name' fallback), -1 when absent
} Op;

// Defined in exec.c; filled once, lazily, on the first execute_code() entry.
void **tcode_dispatch_table(void);

// Defined in tcode.c.
void tcode_compile_func(VM *vm, FuncSym *fn);
void tcode_free_sym(FuncSym *fn);

#endif
