// VM state, symbol tables, frames, and public interpreter entry points.
#ifndef MAD_VM_H
#define MAD_VM_H

#include "lexer.h"
#include "value.h"

typedef struct Op Op; // threaded-code instruction, defined in tcode.h

// True for the synthetic top-level / imported module frames.
bool fname_is_module(const char *n);

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

    // Threaded-code compilation (lazy, on first execution).
    Op *code;        // emitted ops
    size_t code_n;
    size_t *code_map; // body-relative token index -> first op index
    size_t map_n;
    char **owned;     // strings referenced by compiled ops
    size_t owned_n, owned_cap;
    bool compiled;
} FuncSym;
typedef struct { FuncSym *v; size_t n, cap; } FuncVec;

typedef struct {
    FuncSym *fn;
    VarVec locals;
    uint64_t *local_mems; // frame-lifetime mem ids to release on return
    size_t local_mem_n, local_mem_cap;
    size_t frame_id;
} Frame;
typedef struct { Frame *v; size_t n, cap; } FrameVec;

typedef struct {
    TokenVec toks;
    MemVec mems;
    PtrVec ptrs;
    MemPtrVec memptrs;
    VarVec globals;
    FuncVec funcs;
    FileVec files;   // open-file handles (NULL = closed slot)
    ValStack stack;
    FrameVec frames;
    bool halted;
    char *file_dir;      // directory of the file currently being executed
    unsigned import_depth;
    char **imported;     // canonical paths already imported (#pragma once)
    size_t imported_n, imported_cap;
} VM;

// Hard cap on nested "import" chains (cycle guard).
#define MAD_MAX_IMPORT_DEPTH 64

// ---------- Symbol tables ----------

Var *vm_find_var(VarVec *vv, const char *name);
Var *vm_add_var(VarVec *vv, const char *name, TypeKind type, bool initialized, Value value);
FuncSym *vm_find_func(FuncVec *fv, const char *name);
LabelSym *vm_find_label(FuncSym *fn, const char *name);

// Scan a function's body range for "name:" labels.
void vm_collect_labels(VM *vm, FuncSym *fn);

// ---------- Interpreter phases ----------

// Scan the whole token stream and register all ":name ... ;" definitions.
void vm_discover_functions(VM *vm);

// Scan a token range [start, end) and register all ":name ... ;" definitions
// found inside it; used by "import" for tokens from imported files.
void vm_discover_functions_range(VM *vm, size_t start, size_t end);

// Execute top-level code; function bodies are skipped as they are reached.
void vm_run_top_level(VM *vm);

// Execute a top-level token range [start, end) in its own frame named
// "label"; function bodies inside the range are skipped. Used both for
// the main file and by "import".
void vm_execute_module(VM *vm, const char *label, size_t start, size_t end);

// Release every resource held by the VM.
void vm_free(VM *vm);

#endif
