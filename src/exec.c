// Runtime core of the MAD interpreter: variable resolution, frames, the
// direct-threaded dispatch loop, I/O and the module runner.
//
// The compiler (token classification, op emission, jump fusion) lives in
// tcode.c; it produces an array of Op records that execute_code() runs via
// GCC labels-as-values (computed goto).

#include "vm.h"
#include "tcode.h"

#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool fname_is_module(const char *n) {
    return strcmp(n, "<top>") == 0 || strcmp(n, "<import>") == 0;
}

static bool is_top_level(Frame *fr) { return fname_is_module(fr->fn->name); }

// ---------- Value accessors ----------

static double value_to_f64(VM *vm, Value v, size_t line) {
    (void)vm;
    if (v.type == T_F64) return v.as.f64;
    if (v.type == T_F32) return (double)v.as.f32;
    fatal_at(line, "expected f64, got %s", type_name(v.type));
    return 0;
}

static int64_t get_i64(VM *vm, Value v, size_t line) {
    (void)vm;
    return val_as_i64(v);
}

static bool is_true(VM *vm, Value v, size_t line) {
    (void)vm;
    if (v.type == T_BOOL) return v.as.b;
    if (is_numeric(v.type)) return val_as_i64(v) != 0;
    fatal_at(line, "expected boolean/integer condition, got %s", type_name(v.type));
    return false;
}

// ---------- Global visibility ----------

static bool global_visible_in(Frame *fr, const char *name) {
    if (is_top_level(fr)) return true;
    for (size_t i = 0; i < fr->fn->global_count; ++i) {
        if (strcmp(fr->fn->globals[i], name) == 0) return true;
    }
    return false;
}

static bool global_declared_in(Frame *fr, const char *name) {
    if (is_top_level(fr)) return false;
    for (size_t i = 0; i < fr->fn->global_count; ++i) {
        if (strcmp(fr->fn->globals[i], name) == 0) return true;
    }
    return false;
}

static Var *resolve_var(VM *vm, Frame *fr, const char *name, bool *is_global) {
    Var *lv = vm_find_var(&fr->locals, name);
    if (lv) {
        *is_global = false;
        return lv;
    }
    if (global_visible_in(fr, name)) {
        Var *gv = vm_find_var(&vm->globals, name);
        if (!gv) return NULL;
        *is_global = true;
        return gv;
    }
    return NULL;
}

static void require_initialized(Var *v, const char *name, size_t line) {
    if (!v->initialized) fatal_at(line, "uninitialized variable '%s'", name);
}

static Value load_var(VM *vm, Frame *fr, const char *name, size_t line) {
    bool unused = false;
    Var *v = resolve_var(vm, fr, name, &unused);
    if (!v) {
        if (global_declared_in(fr, name))
            fatal_at(line, "declared global variable '%s' does not exist", name);
        fatal_at(line, "unknown variable '%s'", name);
    }
    require_initialized(v, name, line);
    return v->value;
}

static Value make_ptr_value(VM *vm, Frame *fr, const char *name, size_t line) {
    bool g = false;
    Var *v = resolve_var(vm, fr, name, &g);
    if (!v) {
        if (global_declared_in(fr, name))
            fatal_at(line, "declared global variable '%s' does not exist", name);
        fatal_at(line, "unknown variable '%s'", name);
    }
    if (g) {
        size_t gi = (size_t)(v - vm->globals.v);
        return (Value){T_PTR, .as.u64 = ptr_new(&vm->ptrs, (PtrRef){0, 0, true, gi})};
    }
    size_t li = (size_t)(v - fr->locals.v);
    return (Value){T_PTR, .as.u64 = ptr_new(&vm->ptrs, (PtrRef){fr->frame_id, li, false, 0})};
}

static Var *ptr_target(VM *vm, Value pv, size_t line) {
    if (pv.type != T_PTR) fatal_at(line, "expected ptr, got %s", type_name(pv.type));
    PtrRef r = vm->ptrs.v[pv.as.u64];
    if (r.is_global) {
        if (r.global_index >= vm->globals.n)
            fatal_at(line, "dangling global pointer");
        return &vm->globals.v[r.global_index];
    }
    if (r.frame_id >= vm->frames.n) fatal_at(line, "dangling local pointer");
    Frame *owner = &vm->frames.v[r.frame_id];
    if (r.local_index >= owner->locals.n) fatal_at(line, "dangling local pointer");
    return &owner->locals.v[r.local_index];
}

static void assign_var(Var *dst, Value value, size_t line) {
    if (dst->type != value.type) {
        fatal_at(line, "type mismatch in assignment: %s <- %s",
                 type_name(dst->type), type_name(value.type));
    }
    dst->value = value;
    dst->initialized = true;
}

static Value cast_value(VM *vm, Value v, TypeKind ty, size_t line) {
    (void)vm;
    if (v.type == ty) return v;
    switch (ty) {
    case T_I8:
        return make_i8((int8_t)val_as_i64(v));
    case T_U8:
        return make_u8((uint8_t)val_as_i64(v));
    case T_I16:
        return make_i16((int16_t)val_as_i64(v));
    case T_NONE: fatal_at(line, "cannot cast to unknown type"); return v;
    case T_U16:
        return make_u16((uint16_t)val_as_i64(v));
    case T_I32:
        return make_i32((int32_t)val_as_i64(v));
    case T_U32:
        return make_u32((uint32_t)val_as_i64(v));
    case T_I64:
        if (v.type == T_BOOL) return make_i64((int64_t)v.as.b);
        return make_i64(val_as_i64(v));
    case T_U64:
        if (v.type == T_BOOL) return make_u64((uint64_t)v.as.b);
        if (v.type == T_LABEL || v.type == T_FUNC) return make_u64(v.as.u64);
        return make_u64((uint64_t)val_as_i64(v));
    case T_F32:
        return make_f32((float)val_as_f64(v));
    case T_F64:
        return make_f64(val_as_f64(v));
    case T_BOOL:
        if (is_numeric(v.type)) return make_bool(val_as_i64(v) != 0);
        break;
    case T_CHAR:
        if (is_numeric(v.type)) return make_char((uint8_t)val_as_i64(v));
        break;
    case T_LABEL:
    case T_FUNC:
    case T_MEM:
    case T_MEMPTR:
    case T_PTR:
        break;
    }
    fatal_at(line, "unsupported cast from %s to %s", type_name(v.type), type_name(ty));
    return v;
}

// ---------- Printing ----------

static void print_value(VM *vm, Value v) {
    switch (v.type) {
    case T_I8:   printf("%" PRId8, v.as.i8); break;
    case T_U8:   printf("%" PRIu8, v.as.u8); break;
    case T_I16:  printf("%" PRId16, v.as.i16); break;
    case T_U16:  printf("%" PRIu16, v.as.u16); break;
    case T_I32:  printf("%" PRId32, v.as.i32); break;
    case T_U32:  printf("%" PRIu32, v.as.u32); break;
    case T_I64:  printf("%" PRId64, v.as.i64); break;
    case T_U64:  printf("%" PRIu64, v.as.u64); break;
    case T_F32:  printf("%g", (double)v.as.f32); break;
    case T_F64:  printf("%g", v.as.f64); break;
    case T_BOOL: printf("%s", v.as.b ? "true" : "false"); break;
    case T_CHAR: printf("%c", v.as.c); break;
    case T_MEM: printf("<mem:%" PRIu64 ">", v.as.u64); break;
    case T_MEMPTR: printf("<memptr:%" PRIu64 ">", vm->memptrs.v[v.as.u64].mem_id); break;
    case T_PTR: printf("<ptr:%" PRIu64 ">", v.as.u64); break;
    case T_LABEL: printf("<label:%" PRIu64 ">", v.as.u64); break;
    case T_FUNC: printf("<func:%" PRIu64 ">", v.as.u64); break;
    case T_NONE: printf("<?>");
    }
}

// ---------- Memory helpers ----------

// Resolves a mem/memptr stack value to its memory object id.
static uint64_t mem_id_of(VM *vm, Value v, size_t line, const char *op) {
    if (v.type == T_MEM) return v.as.u64;
    if (v.type == T_MEMPTR) return vm->memptrs.v[v.as.u64].mem_id;
    fatal_at(line, "%s expects mem/memptr, got %s", op, type_name(v.type));
    return 0;
}

static MemObj *require_mem(VM *vm, uint64_t mid, size_t line) {
    if (mid >= vm->mems.n || vm->mems.v[mid].data == NULL)
        fatal_at(line, "invalid or freed memory object");
    return &vm->mems.v[mid];
}

// ---------- Frame-local memory tracking ----------

static void frame_track_local_mem(Frame *fr, uint64_t id) {
    VEC_GROW(fr->local_mems, fr->local_mem_n, fr->local_mem_cap, uint64_t);
    fr->local_mems[fr->local_mem_n++] = id;
}

static void frame_release_local_mem(VM *vm, Frame *fr) {
    for (size_t i = 0; i < fr->local_mem_n; ++i) {
        uint64_t id = fr->local_mems[i];
        if (id < vm->mems.n && vm->mems.v[id].data && !vm->mems.v[id].heap) {
            free(vm->mems.v[id].data);
            vm->mems.v[id].data = NULL;
            vm->mems.v[id].len = 0;
        }
    }
    free(fr->local_mems);
    fr->local_mems = NULL;
    fr->local_mem_n = fr->local_mem_cap = 0;
}

static void frame_release_locals(Frame *fr) {
    for (size_t i = 0; i < fr->locals.n; ++i) free(fr->locals.v[i].name);
    free(fr->locals.v);
    fr->locals = (VarVec){0};
}

// ---------- Calls ----------

static void execute_code(VM *vm, FuncSym *fn);

static Frame *current_frame(VM *vm) { return &vm->frames.v[vm->frames.n - 1]; }

static void call_by_value(VM *vm, FuncSym *fn, size_t line, const char *where) {
    if (vm->stack.n < fn->param_count)
        fatal_at(line, "not enough arguments for function '%s'", fn->name);

    VEC_GROW(vm->frames.v, vm->frames.n, vm->frames.cap, Frame);
    size_t frame_id = vm->frames.n;
    Frame *nf = &vm->frames.v[frame_id];
    *nf = (Frame){0};
    nf->fn = fn;
    nf->frame_id = frame_id;
    vm->frames.n++;

    // [] reverses the source group, so the first runtime argument is popped first.
    for (size_t i = 0; i < fn->param_count; ++i) {
        Value v = valstack_pop(&vm->stack, where);
        if (fn->param_types[i] != v.type) {
            fatal_at(line, "argument %zu of '%s' has type %s, expected %s",
                     i + 1, fn->name, type_name(v.type), type_name(fn->param_types[i]));
        }
        if (vm_find_var(&vm->globals, fn->params[i])) {
            fatal_at(line, "local parameter '%s' conflicts with global variable", fn->params[i]);
        }
        if (vm_find_var(&nf->locals, fn->params[i])) {
            fatal_at(line, "duplicate parameter '%s'", fn->params[i]);
        }
        vm_add_var(&nf->locals, fn->params[i], fn->param_types[i], true, v);
    }

    execute_code(vm, fn);
    frame_release_local_mem(vm, &vm->frames.v[frame_id]);
    frame_release_locals(&vm->frames.v[frame_id]);
    vm->frames.n = frame_id;
}

static void declare_variable(VM *vm, Frame *fr, const char *base, TypeKind ty,
                             Value init, size_t line) {
    if (is_top_level(fr)) {
        if (vm_find_var(&vm->globals, base))
            fatal_at(line, "global variable '%s' already exists", base);
        vm_add_var(&vm->globals, base, ty, true, init);
    } else {
        if (vm_find_var(&vm->globals, base))
            fatal_at(line, "local variable '%s' conflicts with global variable", base);
        vm_add_var(&fr->locals, base, ty, true, init);
    }
}

// ---------- Imports ----------

// "import" consumes a memptr holding a NUL-terminated path. Relative paths
// resolve against the directory of the importing file; absolute paths are
// taken verbatim. Imported tokens are appended to the shared token stream,
// their definitions are discovered, and the module's top level runs in its
// own frame -- so its global variables and functions become visible to the
// importer. Only module frames may import: a live function frame would hold
// pointers into vm.funcs.v, which discovery reallocates.
//
// Each file is imported at most once, #pragma once style: the canonical
// path is checked against vm.imported and repeated imports are silent
// no-ops. The main file is registered at startup, so importing it is an
// ignored no-op too.
static void do_import(VM *vm, size_t line) {
    if (!is_top_level(current_frame(vm)))
        fatal_at(line, "'import' is only allowed at top level");

    Value v = valstack_pop(&vm->stack, "import");
    if (v.type != T_MEMPTR)
        fatal_at(line, "import expects memptr path, got %s", type_name(v.type));
    MemObj *m = require_mem(vm, vm->memptrs.v[v.as.u64].mem_id, line);
    const char *raw = (const char *)m->data;
    if (!memchr(m->data, '\0', m->len))
        fatal_at(line, "import path is not NUL-terminated");

    char *path = path_canonical(vm->file_dir, raw);
    if (!path) fatal_at(line, "import path too deep: '%s'", raw);

    for (size_t i = 0; i < vm->imported_n; ++i) {
        if (strcmp(vm->imported[i], path) == 0) {
            free(path); // already imported once -- silent no-op
            return;
        }
    }
    VEC_GROW(vm->imported, vm->imported_n, vm->imported_cap, char *);
    vm->imported[vm->imported_n++] = path;

    if (++vm->import_depth > MAD_MAX_IMPORT_DEPTH)
        fatal_at(line, "import depth exceeded (cycle?)");

    FILE *probe = fopen(path, "rb");
    if (!probe) fatal_at(line, "cannot open imported file '%s'", path);
    fclose(probe);

    char *saved_dir = vm->file_dir;
    vm->file_dir = path_dir_of(path);

    char *src = read_file(path);
    size_t start = vm->toks.n;
    lex_source(src, &vm->toks);
    free(src);
    size_t end = vm->toks.n;

    vm_discover_functions_range(vm, start, end);
    vm_execute_module(vm, "<import>", start, end);

    free(vm->file_dir);
    vm->file_dir = saved_dir;
    vm->import_depth--;
}

// ---------- Dispatch table ----------

static void **g_disp;
static bool g_disp_ready;

void **tcode_dispatch_table(void) { return g_disp; }

// ---------- Standard input ----------

static void do_stdin_read(VM *vm, TypeKind ty, size_t line, const char *text) {
    switch (ty) {
    case T_I8: {
        int8_t x;
        if (scanf("%hhd", &x) != 1) fatal_at(line, "failed to read i8");
        valstack_push(&vm->stack, make_i8(x));
        break;
    }
    case T_U8: {
        uint8_t x;
        if (scanf("%hhu", &x) != 1) fatal_at(line, "failed to read u8");
        valstack_push(&vm->stack, make_u8(x));
        break;
    }
    case T_I16: {
        int16_t x;
        if (scanf("%hd", &x) != 1) fatal_at(line, "failed to read i16");
        valstack_push(&vm->stack, make_i16(x));
        break;
    }
    case T_U16: {
        uint16_t x;
        if (scanf("%hu", &x) != 1) fatal_at(line, "failed to read u16");
        valstack_push(&vm->stack, make_u16(x));
        break;
    }
    case T_I32: {
        int32_t x;
        if (scanf("%" SCNd32, &x) != 1) fatal_at(line, "failed to read i32");
        valstack_push(&vm->stack, make_i32(x));
        break;
    }
    case T_U32: {
        uint32_t x;
        if (scanf("%" SCNu32, &x) != 1) fatal_at(line, "failed to read u32");
        valstack_push(&vm->stack, make_u32(x));
        break;
    }
    case T_I64: {
        int64_t x;
        if (scanf("%" SCNd64, &x) != 1) fatal_at(line, "failed to read i64");
        valstack_push(&vm->stack, make_i64(x));
        break;
    }
    case T_U64: {
        uint64_t x;
        if (scanf("%" SCNu64, &x) != 1) fatal_at(line, "failed to read u64");
        valstack_push(&vm->stack, make_u64(x));
        break;
    }
    case T_F32: {
        float x;
        if (scanf("%f", &x) != 1) fatal_at(line, "failed to read f32");
        valstack_push(&vm->stack, make_f32(x));
        break;
    }
    case T_F64: {
        double x;
        if (scanf("%lf", &x) != 1) fatal_at(line, "failed to read f64");
        valstack_push(&vm->stack, make_f64(x));
        break;
    }
    case T_CHAR: {
        unsigned char ch;
        if (scanf(" %c", &ch) != 1) fatal_at(line, "failed to read char");
        valstack_push(&vm->stack, make_char(ch));
        break;
    }
    case T_BOOL: {
        char buf[64];
        if (scanf("%63s", buf) != 1) fatal_at(line, "failed to read bool");
        if (word_is(buf, "true") || word_is(buf, "1")) {
            valstack_push(&vm->stack, make_bool(true));
        } else if (word_is(buf, "false") || word_is(buf, "0")) {
            valstack_push(&vm->stack, make_bool(false));
        } else {
            fatal_at(line, "invalid bool input '%s'", buf);
        }
        break;
    }
    default:
        fatal_at(line, "read@%s is not supported by the MVP input module",
                 strchr(text, '@') + 1);
    }
}

// ---------- Dispatch loop ----------

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

static void execute_code(VM *vm, FuncSym *fn) {
    if (!g_disp_ready) {
        static void *disp[OP_COUNT];
        disp[OP_PUSH_I8]  = &&L_PUSH_I8;
        disp[OP_PUSH_U8]  = &&L_PUSH_U8;
        disp[OP_PUSH_I16] = &&L_PUSH_I16;
        disp[OP_PUSH_U16] = &&L_PUSH_U16;
        disp[OP_PUSH_I32] = &&L_PUSH_I32;
        disp[OP_PUSH_U32] = &&L_PUSH_U32;
        disp[OP_PUSH_I64] = &&L_PUSH_I64;
        disp[OP_PUSH_U64] = &&L_PUSH_U64;
        disp[OP_PUSH_F32] = &&L_PUSH_F32;
        disp[OP_PUSH_F64] = &&L_PUSH_F64;
        disp[OP_PUSH_STR] = &&L_PUSH_STR;
        disp[OP_PUSH_BOOL] = &&L_PUSH_BOOL;
        disp[OP_PUSH_LABEL] = &&L_PUSH_LABEL;
        disp[OP_PUSH_FUNC] = &&L_PUSH_FUNC;
        disp[OP_WORD_VAR] = &&L_WORD_VAR;
        disp[OP_REF_NAME] = &&L_REF_NAME;
        disp[OP_DEREF_NAME] = &&L_DEREF_NAME;
        disp[OP_CAST] = &&L_CAST;
        // Typed arithmetic
        disp[OP_ADD_I64] = &&L_ADD_I64;
        disp[OP_ADD_F64] = &&L_ADD_F64;
        disp[OP_SUB_I64] = &&L_SUB_I64;
        disp[OP_SUB_F64] = &&L_SUB_F64;
        disp[OP_MUL_I64] = &&L_MUL_I64;
        disp[OP_MUL_F64] = &&L_MUL_F64;
        disp[OP_DIV_I64] = &&L_DIV_I64;
        disp[OP_DIV_F64] = &&L_DIV_F64;
        disp[OP_MOD_I64] = &&L_MOD_I64;
        // Generic arithmetic
        disp[OP_ADD] = &&L_ADD;
        disp[OP_SUB] = &&L_SUB;
        disp[OP_MUL] = &&L_MUL;
        disp[OP_DIV] = &&L_DIV;
        disp[OP_MOD] = &&L_MOD;
        // Typed comparison
        disp[OP_EQ_I64] = &&L_EQ_I64;
        disp[OP_EQ_F64] = &&L_EQ_F64;
        disp[OP_NE_I64] = &&L_NE_I64;
        disp[OP_NE_F64] = &&L_NE_F64;
        disp[OP_LT_I64] = &&L_LT_I64;
        disp[OP_LT_F64] = &&L_LT_F64;
        disp[OP_GT_I64] = &&L_GT_I64;
        disp[OP_GT_F64] = &&L_GT_F64;
        disp[OP_LE_I64] = &&L_LE_I64;
        disp[OP_LE_F64] = &&L_LE_F64;
        disp[OP_GE_I64] = &&L_GE_I64;
        disp[OP_GE_F64] = &&L_GE_F64;
        // Generic comparison
        disp[OP_EQ] = &&L_EQ;
        disp[OP_NE] = &&L_NE;
        disp[OP_LT] = &&L_LT;
        disp[OP_GT] = &&L_GT;
        disp[OP_LE] = &&L_LE;
        disp[OP_GE] = &&L_GE;
        disp[OP_ASSIGN] = &&L_ASSIGN;
        // Bitwise (typed i64 + generic)
        disp[OP_BITNOT] = &&L_BITNOT;
        disp[OP_LOGNOT] = &&L_LOGNOT;
        disp[OP_SHL_I64] = &&L_SHL_I64;
        disp[OP_SHR_I64] = &&L_SHR_I64;
        disp[OP_AND_I64] = &&L_AND_I64;
        disp[OP_OR_I64]  = &&L_OR_I64;
        disp[OP_XOR_I64] = &&L_XOR_I64;
        disp[OP_SHL] = &&L_SHL;
        disp[OP_SHR] = &&L_SHR;
        disp[OP_AND] = &&L_AND;
        disp[OP_OR]  = &&L_OR;
        disp[OP_XOR] = &&L_XOR;
        disp[OP_LOGAND] = &&L_LOGAND;
        disp[OP_LOGOR]  = &&L_LOGOR;
        disp[OP_ALLOC] = &&L_ALLOC;
        disp[OP_HALLOC] = &&L_HALLOC;
        disp[OP_FREE] = &&L_FREE;
        disp[OP_SIZEOF] = &&L_SIZEOF;
        disp[OP_MREAD] = &&L_MREAD;
        disp[OP_WRITE] = &&L_WRITE;
        disp[OP_PRINT] = &&L_PRINT;
        disp[OP_PRINTLN] = &&L_PRINTLN;
        disp[OP_PRINTSTR] = &&L_PRINTSTR;
        disp[OP_READ] = &&L_READ;
        disp[OP_DUP] = &&L_DUP;
        disp[OP_DROP] = &&L_DROP;
        disp[OP_SWAP] = &&L_SWAP;
        disp[OP_ASSERT] = &&L_ASSERT;
        disp[OP_IMPORT] = &&L_IMPORT;
        disp[OP_CALL_FUNC] = &&L_CALL_FUNC;
        disp[OP_CALL_IND] = &&L_CALL_IND;
        disp[OP_JMP] = &&L_JMP;
        disp[OP_JZ] = &&L_JZ;
        disp[OP_JNZ] = &&L_JNZ;
        disp[OP_JMP_DYN] = &&L_JMP_DYN;
        disp[OP_JZ_DYN] = &&L_JZ_DYN;
        disp[OP_JNZ_DYN] = &&L_JNZ_DYN;
        disp[OP_RET] = &&L_RET;
        disp[OP_HALT] = &&L_HALT;
        g_disp = disp;
        g_disp_ready = true;
    }
    if (!fn->compiled) tcode_compile_func(vm, fn);

    ValStack *st = &vm->stack;
    const Op *code = fn->code;
    const Op *ip = code - 1;

#define NEXT() goto *(++ip)->code
#define JUMP_TO(idx)               \
    do {                           \
        ip = code + (size_t)(idx); \
        goto *ip->code;            \
    } while (0)

    NEXT();

L_PUSH_I8:
    valstack_push(st, make_i8((int8_t)ip->u.i));
    NEXT();
L_PUSH_U8:
    valstack_push(st, make_u8((uint8_t)ip->u.u));
    NEXT();
L_PUSH_I16:
    valstack_push(st, make_i16((int16_t)ip->u.i));
    NEXT();
L_PUSH_U16:
    valstack_push(st, make_u16((uint16_t)ip->u.u));
    NEXT();
L_PUSH_I32:
    valstack_push(st, make_i32((int32_t)ip->u.i));
    NEXT();
L_PUSH_U32:
    valstack_push(st, make_u32((uint32_t)ip->u.u));
    NEXT();
L_PUSH_I64:
    valstack_push(st, make_i64(ip->u.i));
    NEXT();
L_PUSH_U64:
    valstack_push(st, make_u64(ip->u.u));
    NEXT();
L_PUSH_F32:
    valstack_push(st, make_f32((float)ip->u.d));
    NEXT();
L_PUSH_F64:
    valstack_push(st, make_f64(ip->u.d));
    NEXT();
L_PUSH_BOOL:
    valstack_push(st, make_bool((bool)ip->u.i));
    NEXT();
L_PUSH_STR:
    valstack_push(st, (Value){T_MEMPTR, .as.u64 = memptr_new(&vm->memptrs, ip->u.u)});
    NEXT();
L_PUSH_LABEL:
    valstack_push(st, (Value){T_LABEL, .as.u64 = ip->u.u});
    NEXT();
L_PUSH_FUNC:
    valstack_push(st, (Value){T_FUNC, .as.u64 = ip->u.u});
    NEXT();

L_WORD_VAR: {
    // Plain word: load an existing variable, or implicitly declare one by
    // consuming the stack top. Existence stays a run-time property.
    const char *base = ip->u.name;
    Frame *fr = current_frame(vm);
    bool unused = false;
    Var *v = resolve_var(vm, fr, base, &unused);
    if (v) {
        if (ip->has_ty && v->type != ip->ty)
            fatal_at(ip->line, "variable '%s' already has type %s, not %s",
                     base, type_name(v->type), type_name(ip->ty));
        require_initialized(v, base, ip->line);
        valstack_push(st, v->value);
        NEXT();
    }
    if (global_declared_in(fr, base))
        fatal_at(ip->line, "declared global variable '%s' does not exist", base);
    // Late binding: the name may denote a function registered by a later
    // "import", after this unit was already compiled. Functions shadow
    // variables (compile-time priority), so prefer the call here too.
    FuncSym *fs = vm_find_func(&vm->funcs, base);
    if (fs) {
        call_by_value(vm, fs, ip->line, ip->text);
        if (vm->halted) return;
        NEXT();
    }
    Value init = valstack_pop(st, ip->text);
    TypeKind ty = ip->has_ty ? ip->ty : init.type;
    if (ty != init.type) {
        fatal_at(ip->line, "initializer type %s does not match declared type %s",
                 type_name(init.type), type_name(ty));
    }
    declare_variable(vm, fr, base, ty, init, ip->line);
    NEXT();
}

L_REF_NAME: {
    const char *name = ip->u.name;
    bool g = false;
    Var *vv = resolve_var(vm, current_frame(vm), name, &g);
    if (vv) {
        if (vv->type == T_MEM) {
            // MVP: &mem yields a memptr to the memory object stored there.
            require_initialized(vv, name, ip->line);
            if (vv->value.type != T_MEM)
                fatal_at(ip->line, "internal mem variable type error");
            valstack_push(st, (Value){T_MEMPTR, .as.u64 = memptr_new(&vm->memptrs, vv->value.as.u64)});
        } else {
            valstack_push(st, make_ptr_value(vm, current_frame(vm), name, ip->line));
        }
        NEXT();
    }
    if (ip->aux >= 0) {
        valstack_push(st, (Value){T_LABEL, .as.u64 = (uint64_t)ip->aux});
        NEXT();
    }
    if (ip->aux2 >= 0) {
        valstack_push(st, (Value){T_FUNC, .as.u64 = (uint64_t)ip->aux2});
        NEXT();
    }
    // Late-bound '&name': the function may have arrived via a later "import".
    FuncSym *fs = vm_find_func(&vm->funcs, name);
    if (fs) {
        valstack_push(st, (Value){T_FUNC, .as.u64 = (uint64_t)(fs - vm->funcs.v)});
        NEXT();
    }
    fatal_at(ip->line, "unknown reference '&%s'", name);
}

L_DEREF_NAME: {
    Value pv = load_var(vm, current_frame(vm), ip->u.name, ip->line);
    Var *target = ptr_target(vm, pv, ip->line);
    if (!target->initialized)
        fatal_at(ip->line, "dereferenced uninitialized pointer '%s'", ip->u.name);
    valstack_push(st, target->value);
    NEXT();
}

L_CAST: {
    Value v = valstack_pop(st, ip->text);
    valstack_push(st, cast_value(vm, v, ip->ty, ip->line));
    NEXT();
}

// --- Typed arithmetic: fast path, no type dispatch ---

L_ADD_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) + val_as_i64(b)));
    NEXT();
}
L_ADD_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_f64(a.as.f64 + b.as.f64));
    NEXT();
}
L_SUB_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) - val_as_i64(b)));
    NEXT();
}
L_SUB_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_f64(a.as.f64 - b.as.f64));
    NEXT();
}
L_MUL_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) * val_as_i64(b)));
    NEXT();
}
L_MUL_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_f64(a.as.f64 * b.as.f64));
    NEXT();
}
L_DIV_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    int64_t y = val_as_i64(b);
    if (!y) fatal_at(ip->line, "division by zero");
    valstack_push(st, make_i64(val_as_i64(a) / y));
    NEXT();
}
L_DIV_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_f64(a.as.f64 / b.as.f64));
    NEXT();
}
L_MOD_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    int64_t y = val_as_i64(b);
    if (!y) fatal_at(ip->line, "modulo by zero");
    valstack_push(st, make_i64(val_as_i64(a) % y));
    NEXT();
}

// --- Generic arithmetic: runtime type dispatch ---

#define L_ARITH_BODY(op, a, b) do {                                         \
    if ((a).type == T_F64 || (b).type == T_F64) {                           \
        valstack_push(st, make_f64(value_to_f64(vm, (a), ip->line)          \
                        op value_to_f64(vm, (b), ip->line)));               \
    } else if ((a).type == T_F32 || (b).type == T_F32) {                    \
        valstack_push(st, make_f32((float)(value_to_f64(vm,(a),ip->line)    \
                        op value_to_f64(vm,(b),ip->line))));                \
    } else {                                                                \
        valstack_push(st, make_i64(get_i64(vm,(a),ip->line)                \
                        op get_i64(vm,(b),ip->line)));                      \
    }                                                                       \
} while(0)

L_ADD: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    L_ARITH_BODY(+, a, b);
    NEXT();
}
L_SUB: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    L_ARITH_BODY(-, a, b);
    NEXT();
}
L_MUL: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    L_ARITH_BODY(*, a, b);
    NEXT();
}
L_DIV: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    if (b.type == T_F64) {
        valstack_push(st, make_f64(value_to_f64(vm, a, ip->line) / b.as.f64));
    } else if (b.type == T_F32) {
        valstack_push(st, make_f32((float)(value_to_f64(vm, a, ip->line) / b.as.f32)));
    } else {
        int64_t y = get_i64(vm, b, ip->line);
        if (!y) fatal_at(ip->line, "division by zero");
        valstack_push(st, make_i64(get_i64(vm, a, ip->line) / y));
    }
    NEXT();
}
L_MOD: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    int64_t x = get_i64(vm, a, ip->line);
    int64_t y = get_i64(vm, b, ip->line);
    if (!y) fatal_at(ip->line, "modulo by zero");
    valstack_push(st, make_i64(x % y));
    NEXT();
}

// --- Typed comparison: fast path ---

L_EQ_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(val_as_i64(a) == val_as_i64(b)));
    NEXT();
}
L_EQ_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(a.as.f64 == b.as.f64));
    NEXT();
}
L_NE_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(val_as_i64(a) != val_as_i64(b)));
    NEXT();
}
L_NE_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(a.as.f64 != b.as.f64));
    NEXT();
}
L_LT_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(val_as_i64(a) < val_as_i64(b)));
    NEXT();
}
L_LT_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(a.as.f64 < b.as.f64));
    NEXT();
}
L_GT_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(val_as_i64(a) > val_as_i64(b)));
    NEXT();
}
L_GT_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(a.as.f64 > b.as.f64));
    NEXT();
}
L_LE_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(val_as_i64(a) <= val_as_i64(b)));
    NEXT();
}
L_LE_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(a.as.f64 <= b.as.f64));
    NEXT();
}
L_GE_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(val_as_i64(a) >= val_as_i64(b)));
    NEXT();
}
L_GE_F64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(a.as.f64 >= b.as.f64));
    NEXT();
}

// --- Generic comparison: runtime type dispatch ---

#define CMP_RESULT(x, y)                                                       \
    (k == CMP_EQ ? (x) == (y)                                                  \
     : k == CMP_NE ? (x) != (y)                                                \
     : k == CMP_LT ? (x) < (y)                                                 \
     : k == CMP_GT ? (x) > (y)                                                 \
     : k == CMP_LE ? (x) <= (y)                                                \
                   : (x) >= (y))

#define DISPATCH_CMP(cmp_k) do {                                              \
    int k = (cmp_k);                                                          \
    Value b = valstack_pop(st, ip->text);                                     \
    Value a = valstack_pop(st, ip->text);                                     \
    bool r;                                                                   \
    if (a.type == T_F64 && b.type == T_F64) {                                 \
        r = CMP_RESULT(a.as.f64, b.as.f64);                                  \
    } else if (a.type == T_F32 && b.type == T_F32) {                          \
        r = CMP_RESULT((double)a.as.f32, (double)b.as.f32);                  \
    } else if (is_numeric(a.type) && is_numeric(b.type)) {                    \
        r = CMP_RESULT(val_as_i64(a), val_as_i64(b));                        \
    } else {                                                                  \
        fatal_at(ip->line, "comparison requires equal scalar types, got %s", \
                 type_name(a.type));                                          \
    }                                                                         \
    valstack_push(st, make_bool(r));                                          \
    NEXT();                                                                   \
} while(0)

L_EQ: { DISPATCH_CMP(CMP_EQ); }
L_NE: { DISPATCH_CMP(CMP_NE); }
L_LT: { DISPATCH_CMP(CMP_LT); }
L_GT: { DISPATCH_CMP(CMP_GT); }
L_LE: { DISPATCH_CMP(CMP_LE); }
L_GE: { DISPATCH_CMP(CMP_GE); }

#undef CMP_RESULT

// --- Bitwise ---

L_BITNOT: {
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(~val_as_i64(a)));
    NEXT();
}
L_LOGNOT: {
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(!is_true(vm, a, ip->line)));
    NEXT();
}
// Typed i64 shifts
L_SHL_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) << val_as_i64(b)));
    NEXT();
}
L_SHR_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) >> val_as_i64(b)));
    NEXT();
}
// Typed i64 bitwise binary
L_AND_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) & val_as_i64(b)));
    NEXT();
}
L_OR_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) | val_as_i64(b)));
    NEXT();
}
L_XOR_I64: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) ^ val_as_i64(b)));
    NEXT();
}
// Generic shifts/bitwise (runtime type dispatch via val_as_i64)
L_SHL: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) << val_as_i64(b)));
    NEXT();
}
L_SHR: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) >> val_as_i64(b)));
    NEXT();
}
L_AND: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) & val_as_i64(b)));
    NEXT();
}
L_OR: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) | val_as_i64(b)));
    NEXT();
}
L_XOR: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_i64(val_as_i64(a) ^ val_as_i64(b)));
    NEXT();
}
L_LOGAND: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(is_true(vm, a, ip->line) && is_true(vm, b, ip->line)));
    NEXT();
}
L_LOGOR: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    valstack_push(st, make_bool(is_true(vm, a, ip->line) || is_true(vm, b, ip->line)));
    NEXT();
}

L_ASSIGN: {
    Value pv = valstack_pop(st, ip->text);
    Value val = valstack_pop(st, ip->text);
    assign_var(ptr_target(vm, pv, ip->line), val, ip->line);
    NEXT();
}

L_ALLOC:
L_HALLOC: {
    bool heap = ip->has_ty; // set at compile time for halloc
    Value n = valstack_pop(st, ip->text);
    int64_t bytes = get_i64(vm, n, ip->line);
    if (bytes < 0) fatal_at(ip->line, "negative allocation");
    uint64_t id = mem_new(&vm->mems, (size_t)bytes, heap, false);
    if (!heap) frame_track_local_mem(current_frame(vm), id);
    valstack_push(st, (Value){T_MEM, .as.u64 = id});
    NEXT();
}

L_FREE: {
    Value m = valstack_pop(st, ip->text);
    uint64_t id;
    if (m.type == T_MEM) {
        id = m.as.u64;
    } else if (m.type == T_MEMPTR) {
        if (m.as.u64 >= vm->memptrs.n) fatal_at(ip->line, "invalid memptr");
        id = vm->memptrs.v[m.as.u64].mem_id;
    } else {
        fatal_at(ip->line, "free expects mem/memptr, got %s", type_name(m.type));
    }
    if (id >= vm->mems.n || vm->mems.v[id].data == NULL)
        fatal_at(ip->line, "invalid or already freed mem id");
    free(vm->mems.v[id].data);
    vm->mems.v[id].data = NULL;
    vm->mems.v[id].len = 0;
    NEXT();
}

L_SIZEOF: {
    Value v = valstack_pop(st, ip->text);
    if (v.type == T_MEM) {
        uint64_t id = v.as.u64;
        if (id >= vm->mems.n || vm->mems.v[id].data == NULL)
            fatal_at(ip->line, "invalid mem");
        valstack_push(st, make_i64((int64_t)vm->mems.v[id].len));
    } else if (v.type == T_MEMPTR) {
        if (v.as.u64 >= vm->memptrs.n) fatal_at(ip->line, "invalid memptr");
        uint64_t mid = vm->memptrs.v[v.as.u64].mem_id;
        if (mid >= vm->mems.n || vm->mems.v[mid].data == NULL)
            fatal_at(ip->line, "invalid mem");
        valstack_push(st, make_i64((int64_t)vm->mems.v[mid].len));
    } else {
        valstack_push(st, make_i64((int64_t)type_size(v.type)));
    }
    NEXT();
}

L_MREAD: {
    Value off = valstack_pop(st, "read offset");
    Value memv = valstack_pop(st, "read mem");
    uint64_t mid = mem_id_of(vm, memv, ip->line, "read");
    MemObj *m = require_mem(vm, mid, ip->line);
    TypeKind ty = ip->ty;
    size_t o = (size_t)get_i64(vm, off, ip->line);
    size_t sz = type_size(ty);
    if (o + sz > m->len) fatal_at(ip->line, "read out of bounds");
    switch (ty) {
    case T_I8:  { int8_t  x; memcpy(&x, m->data + o, 1); valstack_push(st, make_i8(x)); } break;
    case T_U8:  { uint8_t x; memcpy(&x, m->data + o, 1); valstack_push(st, make_u8(x)); } break;
    case T_I16: { int16_t x; memcpy(&x, m->data + o, 2); valstack_push(st, make_i16(x)); } break;
    case T_U16: { uint16_t x; memcpy(&x, m->data + o, 2); valstack_push(st, make_u16(x)); } break;
    case T_I32: { int32_t x; memcpy(&x, m->data + o, 4); valstack_push(st, make_i32(x)); } break;
    case T_U32: { uint32_t x; memcpy(&x, m->data + o, 4); valstack_push(st, make_u32(x)); } break;
    case T_I64: { int64_t x; memcpy(&x, m->data + o, 8); valstack_push(st, make_i64(x)); } break;
    case T_U64: { uint64_t x; memcpy(&x, m->data + o, 8); valstack_push(st, make_u64(x)); } break;
    case T_F32: { float x; memcpy(&x, m->data + o, 4); valstack_push(st, make_f32(x)); } break;
    case T_F64: { double x; memcpy(&x, m->data + o, 8); valstack_push(st, make_f64(x)); } break;
    case T_BOOL: valstack_push(st, make_bool(m->data[o] != 0)); break;
    case T_CHAR: valstack_push(st, make_char(m->data[o])); break;
    default: fatal_at(ip->line, "mread supports scalar types only, got %s", type_name(ty));
    }
    NEXT();
}

L_WRITE: {
    Value off = valstack_pop(st, "write offset");
    Value memv = valstack_pop(st, "write mem");
    Value val = valstack_pop(st, "write value");
    uint64_t mid = mem_id_of(vm, memv, ip->line, "write");
    TypeKind ty = ip->ty;
    if (val.type != ty) {
        fatal_at(ip->line, "write type mismatch: value is %s but write@%s requested",
                 type_name(val.type), type_name(ty));
    }
    MemObj *m = require_mem(vm, mid, ip->line);
    if (m->readonly) fatal_at(ip->line, "cannot write read-only memory");
    size_t sz = type_size(ty);
    size_t o = (size_t)get_i64(vm, off, ip->line);
    if (o + sz > m->len) fatal_at(ip->line, "write out of bounds");
    switch (ty) {
    case T_I8:  memcpy(m->data + o, &val.as.i8, 1); break;
    case T_U8:  memcpy(m->data + o, &val.as.u8, 1); break;
    case T_I16: memcpy(m->data + o, &val.as.i16, 2); break;
    case T_U16: memcpy(m->data + o, &val.as.u16, 2); break;
    case T_I32: memcpy(m->data + o, &val.as.i32, 4); break;
    case T_U32: memcpy(m->data + o, &val.as.u32, 4); break;
    case T_I64: memcpy(m->data + o, &val.as.i64, 8); break;
    case T_U64: memcpy(m->data + o, &val.as.u64, 8); break;
    case T_F32: memcpy(m->data + o, &val.as.f32, 4); break;
    case T_F64: memcpy(m->data + o, &val.as.f64, 8); break;
    case T_BOOL: m->data[o] = val.as.b ? 1 : 0; break;
    case T_CHAR: m->data[o] = val.as.c; break;
    case T_NONE: fatal_at(ip->line, "write@ with unknown type"); break;
    }
    NEXT();
}

L_PRINT:
    print_value(vm, valstack_pop(st, ip->text));
    NEXT();
L_PRINTLN:
    putchar('\n');
    NEXT();
L_PRINTSTR: {
    Value v = valstack_pop(st, ip->text);
    uint64_t mid = mem_id_of(vm, v, ip->line, "printstr");
    MemObj *m = require_mem(vm, mid, ip->line);
    for (size_t k = 0; k < m->len && m->data[k]; ++k) putchar((char)m->data[k]);
    NEXT();
}

L_READ:
    do_stdin_read(vm, ip->ty, ip->line, ip->text);
    NEXT();

L_DUP:
    valstack_push(st, valstack_peek(st, ip->text));
    NEXT();
L_DROP:
    (void)valstack_pop(st, ip->text);
    NEXT();
L_SWAP: {
    Value a = valstack_pop(st, ip->text);
    Value b = valstack_pop(st, ip->text);
    valstack_push(st, a);
    valstack_push(st, b);
    NEXT();
}

L_ASSERT: {
    Value v = valstack_pop(st, ip->text);
    if (!is_true(vm, v, ip->line)) fatal_at(ip->line, "assertion failed");
    NEXT();
}

L_IMPORT:
    do_import(vm, ip->line);
    if (vm->halted) return;
    NEXT();

L_CALL_FUNC:
    call_by_value(vm, &vm->funcs.v[ip->u.i], ip->line, ip->text);
    if (vm->halted) return;
    NEXT();

L_CALL_IND: {
    Value fv = valstack_pop(st, ip->text);
    if (fv.type != T_FUNC)
        fatal_at(ip->line, "call expects func, got %s", type_name(fv.type));
    if (fv.as.u64 >= vm->funcs.n) fatal_at(ip->line, "invalid func id");
    call_by_value(vm, &vm->funcs.v[fv.as.u64], ip->line, ip->text);
    if (vm->halted) return;
    NEXT();
}

L_JMP:
    JUMP_TO(ip->u.u);

L_JZ:
    // Assembly convention: jz branches on zero/false.
    if (is_true(vm, valstack_pop(st, ip->text), ip->line)) NEXT();
    JUMP_TO(ip->u.u);

L_JNZ:
    // ...and jnz branches on non-zero/true.
    if (!is_true(vm, valstack_pop(st, ip->text), ip->line)) NEXT();
    JUMP_TO(ip->u.u);

// Dynamic variants take the target as a first-class label stack value.

L_JMP_DYN: {
    Value target = valstack_pop(st, ip->text);
    if (target.type != T_LABEL)
        fatal_at(ip->line, "%s expects label, got %s", ip->text, type_name(target.type));
    if (target.as.u64 >= fn->labels.n) fatal_at(ip->line, "invalid label id");
    JUMP_TO(fn->code_map[fn->labels.v[target.as.u64].token_index - fn->body_start]);
}

L_JZ_DYN: {
    Value target = valstack_pop(st, ip->text);
    if (target.type != T_LABEL)
        fatal_at(ip->line, "%s expects label, got %s", ip->text, type_name(target.type));
    if (target.as.u64 >= fn->labels.n) fatal_at(ip->line, "invalid label id");
    if (is_true(vm, valstack_pop(st, ip->text), ip->line)) NEXT();
    JUMP_TO(fn->code_map[fn->labels.v[target.as.u64].token_index - fn->body_start]);
}

L_JNZ_DYN: {
    Value target = valstack_pop(st, ip->text);
    if (target.type != T_LABEL)
        fatal_at(ip->line, "%s expects label, got %s", ip->text, type_name(target.type));
    if (target.as.u64 >= fn->labels.n) fatal_at(ip->line, "invalid label id");
    if (!is_true(vm, valstack_pop(st, ip->text), ip->line)) NEXT();
    JUMP_TO(fn->code_map[fn->labels.v[target.as.u64].token_index - fn->body_start]);
}

L_RET:
    return;

L_HALT:
    vm->halted = true;
    return;

#undef NEXT
#undef JUMP_TO
}

#pragma GCC diagnostic pop

// ---------- Module runner ----------

void vm_execute_module(VM *vm, const char *label, size_t start, size_t end) {
    FuncSym top = {0};
    top.name = xstrdup(label);
    top.body_start = start;
    top.body_end = end;
    vm_collect_labels(vm, &top);

    VEC_GROW(vm->frames.v, vm->frames.n, vm->frames.cap, Frame);
    Frame *fr = &vm->frames.v[vm->frames.n];
    *fr = (Frame){0};
    fr->fn = &top;
    fr->frame_id = vm->frames.n;
    const size_t frame_id = vm->frames.n;
    vm->frames.n++;

    execute_code(vm, &top);

    vm->frames.n = frame_id;
    frame_release_local_mem(vm, &vm->frames.v[frame_id]);
    frame_release_locals(&vm->frames.v[frame_id]);

    tcode_free_sym(&top);
    for (size_t i = 0; i < top.labels.n; ++i) free(top.labels.v[i].name);
    free(top.labels.v);
    free(top.name);
}

void vm_run_top_level(VM *vm) {
    vm_execute_module(vm, "<top>", 0, vm->toks.n);
}

void vm_free(VM *vm) {
    free_tokens(&vm->toks);
    for (size_t i = 0; i < vm->mems.n; ++i) free(vm->mems.v[i].data);
    free(vm->mems.v);
    free(vm->ptrs.v);
    free(vm->memptrs.v);
    for (size_t i = 0; i < vm->globals.n; ++i) free(vm->globals.v[i].name);
    free(vm->globals.v);
    for (size_t i = 0; i < vm->funcs.n; ++i) {
        FuncSym *f = &vm->funcs.v[i];
        free(f->name);
        for (size_t j = 0; j < f->param_count; ++j) free(f->params[j]);
        free(f->params);
        free(f->param_types);
        for (size_t j = 0; j < f->global_count; ++j) free(f->globals[j]);
        free(f->globals);
        for (size_t j = 0; j < f->labels.n; ++j) free(f->labels.v[j].name);
        free(f->labels.v);
        tcode_free_sym(f);
    }
    free(vm->funcs.v);
    free(vm->stack.v);
    free(vm->file_dir);
    for (size_t i = 0; i < vm->imported_n; ++i) free(vm->imported[i]);
    free(vm->imported);
    for (size_t i = 0; i < vm->frames.n; ++i) {
        frame_release_locals(&vm->frames.v[i]);
        free(vm->frames.v[i].local_mems);
    }
    free(vm->frames.v);
}
