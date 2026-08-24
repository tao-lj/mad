// VM state, symbol tables, frames, and public interpreter entry points.
#ifndef MAD_VM_H
#define MAD_VM_H

#include "lexer.h"
#include "value.h"

typedef struct {
    char *name;
    TypeKind type;
    bool initialized;
    Value value;
} Var;
typedef struct { Var *v; size_t n, cap; } VarVec;

typedef struct {
    char *name;
    size_t token_index; // first token of the label body
} LabelSym;
typedef struct { LabelSym *v; size_t n, cap; } LabelVec;

typedef struct {
    char *name;
    size_t body_start, body_end; // half-open token range [start, end)
    char **params;
    TypeKind *param_types;
    size_t param_count;
    char **globals;
    size_t global_count;
    LabelVec labels;
} FuncSym;
typedef struct { FuncSym *v; size_t n, cap; } FuncVec;

typedef struct {
    FuncSym *fn;
    size_t pc;
    VarVec locals;
    uint64_t *local_mems; // frame-lifetime mem ids to release on return
    size_t local_mem_n, local_mem_cap;
    size_t frame_id;
} Frame;
typedef struct { Frame *v; size_t n, cap; } FrameVec;

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

// ---------- Symbol tables ----------

Var *vm_find_var(VarVec *vv, const char *name);
Var *vm_add_var(VarVec *vv, const char *name, TypeKind type, bool initialized, Value value);
FuncSym *vm_find_func(FuncVec *fv, const char *name);
LabelSym *vm_find_label(FuncSym *fn, const char *name);

// Scan a function's body range for "name:" labels.
void vm_collect_labels(VM *vm, FuncSym *fn);

// ---------- Interpreter phases ----------

// Scan the token stream and register all ":name ... ;" definitions.
void vm_discover_functions(VM *vm);

// Execute top-level code; function bodies are skipped as they are reached.
void vm_run_top_level(VM *vm);

// Release every resource held by the VM.
void vm_free(VM *vm);

#endif
