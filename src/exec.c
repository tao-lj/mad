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
    if (v.type != T_F64) fatal_at(line, "expected f64, got %s", type_name(v.type));
    return vm->f64.v[v.idx];
}

static int64_t get_i64(VM *vm, Value v, size_t line) {
    if (v.type != T_I64) fatal_at(line, "expected i64, got %s", type_name(v.type));
    return vm->i64.v[v.idx];
}

static bool is_true(VM *vm, Value v, size_t line) {
    if (v.type == T_BOOL) return vm->bytes.v[v.idx] != 0;
    if (v.type == T_I64) return vm->i64.v[v.idx] != 0;
    if (v.type == T_U64) return vm->u64.v[v.idx] != 0;
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
        return (Value){T_PTR, ptr_new(&vm->ptrs, (PtrRef){0, 0, true, gi})};
    }
    size_t li = (size_t)(v - fr->locals.v);
    return (Value){T_PTR, ptr_new(&vm->ptrs, (PtrRef){fr->frame_id, li, false, 0})};
}

static Var *ptr_target(VM *vm, Value pv, size_t line) {
    if (pv.type != T_PTR) fatal_at(line, "expected ptr, got %s", type_name(pv.type));
    PtrRef r = vm->ptrs.v[pv.idx];
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
    if (v.type == ty) return v;
    switch (ty) {
    case T_I64:
        if (v.type == T_U64) return make_i64(&vm->i64, (int64_t)vm->u64.v[v.idx]);
        if (v.type == T_F64) return make_i64(&vm->i64, (int64_t)vm->f64.v[v.idx]);
        if (v.type == T_BOOL || v.type == T_CHAR) return make_i64(&vm->i64, (int64_t)vm->bytes.v[v.idx]);
        break;
    case T_U64:
        if (v.type == T_I64) return make_u64(&vm->u64, (uint64_t)vm->i64.v[v.idx]);
        if (v.type == T_F64) return make_u64(&vm->u64, (uint64_t)vm->f64.v[v.idx]);
        if (v.type == T_BOOL || v.type == T_CHAR) return make_u64(&vm->u64, (uint64_t)vm->bytes.v[v.idx]);
        if (v.type == T_LABEL || v.type == T_FUNC) return make_u64(&vm->u64, v.idx);
        break;
    case T_F64:
        if (v.type == T_I64) return make_f64(&vm->f64, (double)vm->i64.v[v.idx]);
        if (v.type == T_U64) return make_f64(&vm->f64, (double)vm->u64.v[v.idx]);
        if (v.type == T_BOOL || v.type == T_CHAR) return make_f64(&vm->f64, (double)vm->bytes.v[v.idx]);
        break;
    case T_BOOL:
        if (v.type == T_I64) return make_bool(&vm->bytes, vm->i64.v[v.idx] != 0);
        if (v.type == T_U64) return make_bool(&vm->bytes, vm->u64.v[v.idx] != 0);
        break;
    case T_CHAR:
        if (v.type == T_U64) return make_char(&vm->bytes, (uint8_t)vm->u64.v[v.idx]);
        if (v.type == T_I64) return make_char(&vm->bytes, (uint8_t)vm->i64.v[v.idx]);
        break;
    case T_LABEL:
        // Explicit integer -> label is deliberately not supported.
        break;
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
    case T_I64: printf("%" PRId64, vm->i64.v[v.idx]); break;
    case T_U64: printf("%" PRIu64, vm->u64.v[v.idx]); break;
    case T_F64: printf("%g", vm->f64.v[v.idx]); break;
    case T_BOOL: printf("%s", vm->bytes.v[v.idx] ? "true" : "false"); break;
    case T_CHAR: printf("%c", vm->bytes.v[v.idx]); break;
    case T_MEM: printf("<mem:%" PRIu64 ">", v.idx); break;
    case T_MEMPTR: printf("<memptr:%" PRIu64 ">", vm->memptrs.v[v.idx].mem_id); break;
    case T_PTR: printf("<ptr:%" PRIu64 ">", v.idx); break;
    case T_LABEL: printf("<label:%" PRIu64 ">", v.idx); break;
    case T_FUNC: printf("<func:%" PRIu64 ">", v.idx); break;
    }
}

// ---------- Memory helpers ----------

// Resolves a mem/memptr stack value to its memory object id.
static uint64_t mem_id_of(VM *vm, Value v, size_t line, const char *op) {
    if (v.type == T_MEM) return v.idx;
    if (v.type == T_MEMPTR) return vm->memptrs.v[v.idx].mem_id;
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
    MemObj *m = require_mem(vm, vm->memptrs.v[v.idx].mem_id, line);
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
    case T_I64: {
        int64_t x;
        if (scanf("%" SCNd64, &x) != 1) fatal_at(line, "failed to read i64");
        valstack_push(&vm->stack, make_i64(&vm->i64, x));
        break;
    }
    case T_U64: {
        uint64_t x;
        if (scanf("%" SCNu64, &x) != 1) fatal_at(line, "failed to read u64");
        valstack_push(&vm->stack, make_u64(&vm->u64, x));
        break;
    }
    case T_F64: {
        double x;
        if (scanf("%lf", &x) != 1) fatal_at(line, "failed to read f64");
        valstack_push(&vm->stack, make_f64(&vm->f64, x));
        break;
    }
    case T_CHAR: {
        unsigned char ch;
        if (scanf(" %c", &ch) != 1) fatal_at(line, "failed to read char");
        valstack_push(&vm->stack, make_char(&vm->bytes, ch));
        break;
    }
    case T_BOOL: {
        char buf[64];
        if (scanf("%63s", buf) != 1) fatal_at(line, "failed to read bool");
        if (word_is(buf, "true") || word_is(buf, "1")) {
            valstack_push(&vm->stack, make_bool(&vm->bytes, true));
        } else if (word_is(buf, "false") || word_is(buf, "0")) {
            valstack_push(&vm->stack, make_bool(&vm->bytes, false));
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
        disp[OP_PUSH_I64] = &&L_PUSH_I64;
        disp[OP_PUSH_U64] = &&L_PUSH_U64;
        disp[OP_PUSH_F64] = &&L_PUSH_F64;
        disp[OP_PUSH_STR] = &&L_PUSH_STR;
        disp[OP_PUSH_BOOL] = &&L_PUSH_BOOL;
        disp[OP_PUSH_LABEL] = &&L_PUSH_LABEL;
        disp[OP_PUSH_FUNC] = &&L_PUSH_FUNC;
        disp[OP_WORD_VAR] = &&L_WORD_VAR;
        disp[OP_REF_NAME] = &&L_REF_NAME;
        disp[OP_DEREF_NAME] = &&L_DEREF_NAME;
        disp[OP_CAST] = &&L_CAST;
        disp[OP_ARITH] = &&L_ARITH;
        disp[OP_CMP] = &&L_CMP;
        disp[OP_ASSIGN] = &&L_ASSIGN;
        disp[OP_ALLOC] = &&L_ALLOC;
        disp[OP_HALLOC] = &&L_HALLOC;
        disp[OP_FREE] = &&L_FREE;
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

L_PUSH_I64:
    valstack_push(st, make_i64(&vm->i64, ip->u.i));
    NEXT();
L_PUSH_U64:
    valstack_push(st, make_u64(&vm->u64, ip->u.u));
    NEXT();
L_PUSH_F64:
    valstack_push(st, make_f64(&vm->f64, ip->u.d));
    NEXT();
L_PUSH_BOOL:
    valstack_push(st, make_bool(&vm->bytes, (bool)ip->u.i));
    NEXT();
L_PUSH_STR:
    valstack_push(st, (Value){T_MEMPTR, memptr_new(&vm->memptrs, ip->u.u)});
    NEXT();
L_PUSH_LABEL:
    valstack_push(st, (Value){T_LABEL, ip->u.u});
    NEXT();
L_PUSH_FUNC:
    valstack_push(st, (Value){T_FUNC, ip->u.u});
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
            valstack_push(st, (Value){T_MEMPTR, memptr_new(&vm->memptrs, vv->value.idx)});
        } else {
            valstack_push(st, make_ptr_value(vm, current_frame(vm), name, ip->line));
        }
        NEXT();
    }
    if (ip->aux >= 0) {
        valstack_push(st, (Value){T_LABEL, (uint64_t)ip->aux});
        NEXT();
    }
    if (ip->aux2 >= 0) {
        valstack_push(st, (Value){T_FUNC, (uint64_t)ip->aux2});
        NEXT();
    }
    // Late-bound '&name': the function may have arrived via a later "import".
    FuncSym *fs = vm_find_func(&vm->funcs, name);
    if (fs) {
        valstack_push(st, (Value){T_FUNC, (uint64_t)(fs - vm->funcs.v)});
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

L_ARITH: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    int k = (int)ip->u.i;
    if (a.type == T_F64 || b.type == T_F64) {
        double x = value_to_f64(vm, a, ip->line);
        double y = value_to_f64(vm, b, ip->line);
        double r = 0;
        switch (k) {
        case AR_ADD: r = x + y; break;
        case AR_SUB: r = x - y; break;
        case AR_MUL: r = x * y; break;
        case AR_DIV: r = x / y; break;
        default: fatal_at(ip->line, "%% is not defined for f64");
        }
        valstack_push(st, make_f64(&vm->f64, r));
        NEXT();
    }
    int64_t x = get_i64(vm, a, ip->line);
    int64_t y = get_i64(vm, b, ip->line);
    int64_t r = 0;
    switch (k) {
    case AR_ADD: r = x + y; break;
    case AR_SUB: r = x - y; break;
    case AR_MUL: r = x * y; break;
    case AR_DIV:
        if (!y) fatal_at(ip->line, "division by zero");
        r = x / y;
        break;
    default:
        if (!y) fatal_at(ip->line, "division by zero");
        r = x % y;
        break;
    }
    valstack_push(st, make_i64(&vm->i64, r));
    NEXT();
}

#define CMP_RESULT(x, y)                                                       \
    (k == CMP_EQ ? (x) == (y)                                                  \
     : k == CMP_NE ? (x) != (y)                                                \
     : k == CMP_LT ? (x) < (y)                                                 \
     : k == CMP_GT ? (x) > (y)                                                 \
     : k == CMP_LE ? (x) <= (y)                                                \
                   : (x) >= (y))

L_CMP: {
    Value b = valstack_pop(st, ip->text);
    Value a = valstack_pop(st, ip->text);
    int k = (int)ip->u.i;
    bool r;
    if (a.type == T_I64 && b.type == T_I64) {
        r = CMP_RESULT(vm->i64.v[a.idx], vm->i64.v[b.idx]);
    } else if (a.type == T_F64 && b.type == T_F64) {
        r = CMP_RESULT(vm->f64.v[a.idx], vm->f64.v[b.idx]);
    } else {
        fatal_at(ip->line, "comparison requires equal scalar types, got %s",
                 type_name(a.type));
    }
    valstack_push(st, make_bool(&vm->bytes, r));
    NEXT();
}

#undef CMP_RESULT

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
    valstack_push(st, (Value){T_MEM, id});
    NEXT();
}

L_FREE: {
    Value m = valstack_pop(st, ip->text);
    uint64_t id;
    if (m.type == T_MEM) {
        id = m.idx;
    } else if (m.type == T_MEMPTR) {
        if (m.idx >= vm->memptrs.n) fatal_at(ip->line, "invalid memptr");
        id = vm->memptrs.v[m.idx].mem_id;
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

L_MREAD: {
    Value off = valstack_pop(st, "read offset");
    Value memv = valstack_pop(st, "read mem");
    uint64_t mid = mem_id_of(vm, memv, ip->line, "read");
    MemObj *m = require_mem(vm, mid, ip->line);
    TypeKind ty = ip->ty;
    size_t o = (size_t)get_i64(vm, off, ip->line);
    size_t sz;
    switch (ty) {
    case T_I64: case T_U64: case T_F64: sz = 8; break;
    case T_BOOL: case T_CHAR: sz = 1; break;
    default: fatal_at(ip->line, "mread supports scalar types only, got %s", type_name(ty));
    }
    if (o + sz > m->len) fatal_at(ip->line, "read out of bounds");
    switch (ty) {
    case T_I64: { int64_t x; memcpy(&x, m->data + o, 8); valstack_push(st, make_i64(&vm->i64, x)); } break;
    case T_U64: { uint64_t x; memcpy(&x, m->data + o, 8); valstack_push(st, make_u64(&vm->u64, x)); } break;
    case T_F64: { double x; memcpy(&x, m->data + o, 8); valstack_push(st, make_f64(&vm->f64, x)); } break;
    case T_BOOL: valstack_push(st, make_bool(&vm->bytes, m->data[o] != 0)); break;
    default: valstack_push(st, make_char(&vm->bytes, m->data[o])); break;
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
    size_t sz;
    switch (ty) {
    case T_I64: case T_U64: case T_F64: sz = 8; break;
    case T_BOOL: case T_CHAR: sz = 1; break;
    default: fatal_at(ip->line, "write supports scalar types only, got %s", type_name(ty));
    }
    size_t o = (size_t)get_i64(vm, off, ip->line);
    if (o + sz > m->len) fatal_at(ip->line, "write out of bounds");
    switch (ty) {
    case T_I64: memcpy(m->data + o, &vm->i64.v[val.idx], 8); break;
    case T_U64: memcpy(m->data + o, &vm->u64.v[val.idx], 8); break;
    case T_F64: memcpy(m->data + o, &vm->f64.v[val.idx], 8); break;
    default: m->data[o] = vm->bytes.v[val.idx]; break;
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
    if (fv.idx >= vm->funcs.n) fatal_at(ip->line, "invalid func id");
    call_by_value(vm, &vm->funcs.v[fv.idx], ip->line, ip->text);
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
    if (target.idx >= fn->labels.n) fatal_at(ip->line, "invalid label id");
    JUMP_TO(fn->code_map[fn->labels.v[target.idx].token_index - fn->body_start]);
}

L_JZ_DYN: {
    Value target = valstack_pop(st, ip->text);
    if (target.type != T_LABEL)
        fatal_at(ip->line, "%s expects label, got %s", ip->text, type_name(target.type));
    if (target.idx >= fn->labels.n) fatal_at(ip->line, "invalid label id");
    if (is_true(vm, valstack_pop(st, ip->text), ip->line)) NEXT();
    JUMP_TO(fn->code_map[fn->labels.v[target.idx].token_index - fn->body_start]);
}

L_JNZ_DYN: {
    Value target = valstack_pop(st, ip->text);
    if (target.type != T_LABEL)
        fatal_at(ip->line, "%s expects label, got %s", ip->text, type_name(target.type));
    if (target.idx >= fn->labels.n) fatal_at(ip->line, "invalid label id");
    if (!is_true(vm, valstack_pop(st, ip->text), ip->line)) NEXT();
    JUMP_TO(fn->code_map[fn->labels.v[target.idx].token_index - fn->body_start]);
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
    free(vm->i64.v);
    free(vm->u64.v);
    free(vm->f64.v);
    free(vm->bytes.v);
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
