// Interpreter core: threaded-code compiler and dispatch loop.
//
// Each function body (and the top-level code) is compiled once, lazily, into
// an array of Op records. Dispatch is direct threaded: op->code holds the
// address of a label inside execute_code() (GCC labels-as-values), so
// executing one operation costs a single indirect goto. The compiler resolves
// everything static ahead of time -- literal spellings, name classification,
// call targets, jump targets -- so the hot loop performs no strcmp chains and
// no symbol-table scans. Only variable existence stays dynamic (variables can
// be created by executing arbitrary stores), and those checks mirror the old
// token-walking interpreter exactly.

#include "vm.h"

#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_top_level(Frame *fr) { return strcmp(fr->fn->name, "<top>") == 0; }

static bool word_is(const char *s, const char *kw) { return strcmp(s, kw) == 0; }

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

// ---------- Literals and printing ----------

static uint8_t decode_escape(char c) {
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case '0': return 0;
    case '\\': return '\\';
    case '"': return '"';
    default: return (uint8_t)c;
    }
}

// Decodes a string-literal token once at compile time into read-only mem, so
// evaluating the same literal in a loop no longer allocates per iteration.
static uint64_t compile_string_literal(VM *vm, const Token *t) {
    size_t raw = strlen(t->text);
    uint64_t mid = mem_new(&vm->mems, raw + 1, true, true);
    uint8_t *out = vm->mems.v[mid].data;
    size_t n = 0;
    for (size_t i = 0; i < raw; ++i) {
        if (t->text[i] == '\\' && i + 1 < raw)
            out[n++] = decode_escape(t->text[++i]);
        else
            out[n++] = (uint8_t)t->text[i];
    }
    out[n++] = 0;
    return mid;
}

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

// ---------- Threaded code ----------

typedef enum {
    OP_PUSH_I64, OP_PUSH_U64, OP_PUSH_F64, OP_PUSH_STR,
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
    OP_CALL_FUNC, OP_CALL_IND,
    OP_JMP, OP_JZ, OP_JNZ,
    OP_JMP_DYN, OP_JZ_DYN, OP_JNZ_DYN,
    OP_RET, OP_HALT,
    OP_COUNT
} OpCode;

// Arithmetic sub-kinds (stored in op->u.i).
enum { AR_ADD, AR_SUB, AR_MUL, AR_DIV, AR_MOD };
// Comparison sub-kinds.
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

typedef struct {
    size_t op_idx; // jump op whose target needs patching
    size_t map_idx; // body-relative destination token index
} JumpFixup;

typedef struct {
    VM *vm;
    FuncSym *fn;
    bool top_level;
    Op *v;
    size_t n, cap;
    size_t *map;    // body-relative token index -> first op emitted there
    size_t map_n;
    JumpFixup *fix; // static jumps awaiting their target op index
    size_t fix_n, fix_cap;
    int pending_label;    // bare label word awaiting fusion, -1 when none
    const Token *pending_tok;
} Compiler;

static void own_string(Compiler *c, char *s) {
    VEC_GROW(c->fn->owned, c->fn->owned_n, c->fn->owned_cap, char *);
    c->fn->owned[c->fn->owned_n++] = s;
}

// Writes the thread immediately: the dispatch table is guaranteed to be
// filled before any compilation runs (execute_code() initializes it first).
static void **tcode_dispatch_table(void);

static size_t emit_op(Compiler *c, OpCode kind, const Token *t) {
    VEC_GROW(c->v, c->n, c->cap, Op);
    Op *op = &c->v[c->n++];
    *op = (Op){0};
    op->code = tcode_dispatch_table()[kind];
    op->text = t ? t->text : "<end>";
    op->line = t ? t->line : 0;
    op->aux = -1;
    op->aux2 = -1;
    return c->n - 1;
}

static void flush_pending_label(Compiler *c) {
    if (c->pending_label < 0) return;
    size_t idx = emit_op(c, OP_PUSH_LABEL, c->pending_tok);
    c->v[idx].u.u = (uint64_t)c->pending_label;
    c->pending_label = -1;
    c->pending_tok = NULL;
}

static void emit_const(Compiler *c, OpCode kind, const Token *t, uint64_t payload) {
    size_t idx = emit_op(c, kind, t);
    c->v[idx].u.u = payload;
}

// Records a static jump whose destination op index is not yet known.
static void emit_static_jump(Compiler *c, OpCode kind, const Token *t, LabelSym *target) {
    size_t idx = emit_op(c, kind, t);
    VEC_GROW(c->fix, c->fix_n, c->fix_cap, JumpFixup);
    c->fix[c->fix_n].op_idx = idx;
    c->fix[c->fix_n].map_idx = target->token_index - c->fn->body_start;
    c->fix_n++;
}

// Parses the "@type" suffix of mread@/write@/read@ words at compile time.
static TypeKind compile_type_suffix(const Token *t, const char *what) {
    const char *at = strchr(t->text, '@');
    if (!at || !at[1]) fatal_at(t->line, "%s requires type, e.g. %si64", what, t->text);
    TypeKind ty;
    if (!is_type_name(at + 1, &ty)) fatal_at(t->line, "unknown %s type '%s'", what, at + 1);
    return ty;
}

static void compile_ref_or_deref(Compiler *c, const Token *t) {
    const char *s = t->text;
    if (s[0] == '&') {
        const char *name = s + 1;
        size_t idx = emit_op(c, OP_REF_NAME, t);
        c->v[idx].u.name = xstrdup(name);
        own_string(c, c->v[idx].u.name);
        // First-class label/func fallbacks are fixed after discovery, so they
        // can be resolved here instead of scanned at run time.
        LabelSym *ls = vm_find_label(c->fn, name);
        if (ls) c->v[idx].aux = (int64_t)(ls - c->fn->labels.v);
        FuncSym *fs = vm_find_func(&c->vm->funcs, name);
        if (fs) c->v[idx].aux2 = (int64_t)(fs - c->vm->funcs.v);
        return;
    }
    // '*name'
    size_t idx = emit_op(c, OP_DEREF_NAME, t);
    c->v[idx].u.name = xstrdup(s + 1);
    own_string(c, c->v[idx].u.name);
}

static void compile_word(Compiler *c, const Token *t) {
    VM *vm = c->vm;
    const char *s = t->text;

    // Label definitions are pure position markers.
    size_t len = strlen(s);
    if (len && s[len - 1] == ':') return;

    LabelSym *ls = vm_find_label(c->fn, s);
    if (ls) {
        // A bare label word fuses with an immediately following jump;
        // otherwise it still pushes a first-class label value.
        if (c->pending_label >= 0) flush_pending_label(c);
        c->pending_label = (int)(ls - c->fn->labels.v);
        c->pending_tok = t;
        return;
    }

    // User definitions shadow builtin words, so the function table is
    // consulted before any builtin classification -- same as the old
    // interpreter's lookup order.
    FuncSym *fs = vm_find_func(&vm->funcs, s);
    if (fs) {
        flush_pending_label(c);
        size_t idx = emit_op(c, OP_CALL_FUNC, t);
        c->v[idx].u.i = (int64_t)(fs - vm->funcs.v);
        return;
    }

    bool is_jz = word_is(s, "jz"), is_jnz = word_is(s, "jnz");
    bool is_jmp = word_is(s, "jmp") || word_is(s, "jump");
    if ((is_jz || is_jnz || is_jmp) && c->pending_label >= 0) {
        // Fuse: branch straight to the compiled position instead of pushing
        // and popping a label value at run time.
        LabelSym *target = &c->fn->labels.v[c->pending_label];
        OpCode k = is_jmp ? OP_JMP : (is_jz ? OP_JZ : OP_JNZ);
        emit_static_jump(c, k, t, target);
        c->pending_label = -1;
        c->pending_tok = NULL;
        return;
    }
    flush_pending_label(c);

    if (word_is(s, "ret")) { emit_op(c, OP_RET, t); return; }
    if (word_is(s, "halt")) { emit_op(c, OP_HALT, t); return; }
    if (word_is(s, "dup")) { emit_op(c, OP_DUP, t); return; }
    if (word_is(s, "drop")) { emit_op(c, OP_DROP, t); return; }
    if (word_is(s, "swap")) { emit_op(c, OP_SWAP, t); return; }

    static const char *arith_words[] = {"+", "-", "*", "/", "%"};
    for (int k = AR_ADD; k <= AR_MOD; ++k) {
        if (word_is(s, arith_words[k])) {
            size_t idx = emit_op(c, OP_ARITH, t);
            c->v[idx].u.i = k;
            return;
        }
    }
    static const char *cmp_words[] = {"==", "!=", "<", ">", "<=", ">="};
    for (int k = CMP_EQ; k <= CMP_GE; ++k) {
        if (word_is(s, cmp_words[k])) {
            size_t idx = emit_op(c, OP_CMP, t);
            c->v[idx].u.i = k;
            return;
        }
    }

    if (word_is(s, "=")) { emit_op(c, OP_ASSIGN, t); return; }
    if (word_is(s, "call")) { emit_op(c, OP_CALL_IND, t); return; }
    if (is_jz) { emit_op(c, OP_JZ_DYN, t); return; }
    if (is_jnz) { emit_op(c, OP_JNZ_DYN, t); return; }
    if (is_jmp) { emit_op(c, OP_JMP_DYN, t); return; }

    if (strncmp(s, "mread@", 6) == 0) {
        size_t idx = emit_op(c, OP_MREAD, t);
        c->v[idx].ty = compile_type_suffix(t, "mread");
        return;
    }
    if (strncmp(s, "write@", 6) == 0) {
        size_t idx = emit_op(c, OP_WRITE, t);
        c->v[idx].ty = compile_type_suffix(t, "write");
        return;
    }
    if (strncmp(s, "read@", 5) == 0) {
        size_t idx = emit_op(c, OP_READ, t);
        c->v[idx].ty = compile_type_suffix(t, "read");
        return;
    }

    if (word_is(s, "alloc")) { emit_op(c, OP_ALLOC, t); return; }
    if (word_is(s, "halloc")) {
        size_t idx = emit_op(c, OP_HALLOC, t);
        c->v[idx].has_ty = true; // heap-lifetime flag read by the handler
        return;
    }
    if (word_is(s, "free")) { emit_op(c, OP_FREE, t); return; }

    if (word_is(s, "print") || word_is(s, "printn")) { emit_op(c, OP_PRINT, t); return; }
    if (word_is(s, "println")) { emit_op(c, OP_PRINTLN, t); return; }
    if (word_is(s, "printstr")) { emit_op(c, OP_PRINTSTR, t); return; }
    if (word_is(s, "assert")) { emit_op(c, OP_ASSERT, t); return; }

    // Fallback: variable load or implicit declaration from the stack top.
    char base[MAX_NAME];
    bool has_ty = false;
    TypeKind ann = T_I64;
    split_annotated_name(s, base, sizeof(base), &has_ty, &ann);
    if (!is_var_token(base)) fatal_at(t->line, "unknown token '%s'", s);
    size_t idx = emit_op(c, OP_WORD_VAR, t);
    c->v[idx].u.name = xstrdup(base);
    own_string(c, c->v[idx].u.name);
    c->v[idx].ty = ann;
    c->v[idx].has_ty = has_ty;
}

// Compiles one function body (or the whole top level) into threaded code.
static void compile_func(VM *vm, FuncSym *fn) {
    Compiler c = {0};
    c.vm = vm;
    c.fn = fn;
    c.top_level = strcmp(fn->name, "<top>") == 0;
    c.pending_label = -1;
    c.map_n = fn->body_end - fn->body_start;
    c.map = malloc((c.map_n ? c.map_n : 1) * sizeof *c.map);
    if (!c.map) die_oom();
    for (size_t i = 0; i < c.map_n; ++i) c.map[i] = SIZE_MAX;

    size_t i = fn->body_start;
    while (i < fn->body_end) {
        Token *t = &vm->toks.v[i];
        c.map[i - fn->body_start] = c.n;

        switch (t->kind) {
        case TOK_INT: {
            flush_pending_label(&c);
            size_t idx = emit_op(&c, OP_PUSH_I64, t);
            c.v[idx].u.i = strtoll(t->text, NULL, 10);
            ++i;
            break;
        }
        case TOK_UINT: {
            flush_pending_label(&c);
            size_t idx = emit_op(&c, OP_PUSH_U64, t);
            c.v[idx].u.u = strtoull(t->text + 2, NULL, 10);
            ++i;
            break;
        }
        case TOK_FLOAT: {
            flush_pending_label(&c);
            size_t idx = emit_op(&c, OP_PUSH_F64, t);
            c.v[idx].u.d = strtod(t->text, NULL);
            ++i;
            break;
        }
        case TOK_STRING:
            flush_pending_label(&c);
            emit_const(&c, OP_PUSH_STR, t, compile_string_literal(vm, t));
            ++i;
            break;
        case TOK_COLON:
            // Registered definitions are skipped; their bodies compile
            // separately through their own FuncSym.
            flush_pending_label(&c);
            if (c.top_level) {
                while (i < fn->body_end && vm->toks.v[i].kind != TOK_SEMI) ++i;
            }
            ++i;
            break;
        case TOK_SEMI:
            // End of a function definition body; a stray ';' at top level
            // stopped execution in the old interpreter, so it stops here too.
            ++i;
            goto done;
        case TOK_WORD:
            if (t->text[0] == '&' || (t->text[0] == '*' && t->text[1])) {
                flush_pending_label(&c);
                compile_ref_or_deref(&c, t);
            } else if (strncmp(t->text, "!@", 2) == 0) {
                flush_pending_label(&c);
                size_t idx = emit_op(&c, OP_CAST, t);
                if (!is_type_name(t->text + 2, &c.v[idx].ty))
                    fatal_at(t->line, "unknown cast type '%s'", t->text + 2);
            } else {
                compile_word(&c, t);
            }
            ++i;
            break;
        default: // TOK_PARAM_END, stray TOK_GLOBAL_REF
            ++i;
            break;
        }
    }
done:
    flush_pending_label(&c);

    // Sentinel: jumps aimed past the last real instruction land here,
    // mirroring the old "reached end of body" implicit return.
    size_t sentinel = emit_op(&c, OP_RET, NULL);
    for (size_t j = 0; j < c.fix_n; ++j) {
        size_t d = c.map[c.fix[j].map_idx];
        c.v[c.fix[j].op_idx].u.u = (d == SIZE_MAX) ? sentinel : d;
    }
    for (size_t j = 0; j < c.map_n; ++j) {
        if (c.map[j] == SIZE_MAX) c.map[j] = sentinel;
    }

    fn->code = c.v;
    fn->code_n = c.n;
    fn->code_map = c.map;
    fn->map_n = c.map_n;
    fn->compiled = true;
}

// The dispatch table maps OpCode -> &&label. It is filled on the first entry
// into execute_code() because label addresses are only visible there; the
// compiler (which only ever runs after that point) reads it back.
static void **g_disp;
static bool g_disp_ready;

static void **tcode_dispatch_table(void) { return g_disp; }

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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

static void execute_code(VM *vm, FuncSym *fn) {
    if (!g_disp_ready) {
        static void *disp[OP_COUNT];
        disp[OP_PUSH_I64] = &&L_PUSH_I64;
        disp[OP_PUSH_U64] = &&L_PUSH_U64;
        disp[OP_PUSH_F64] = &&L_PUSH_F64;
        disp[OP_PUSH_STR] = &&L_PUSH_STR;
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
    if (!fn->compiled) compile_func(vm, fn);

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

// Releases compiled code owned by a FuncSym (used for the synthetic <top>).
static void tcode_free_sym(FuncSym *fn) {
    free(fn->code);
    free(fn->code_map);
    for (size_t i = 0; i < fn->owned_n; ++i) free(fn->owned[i]);
    free(fn->owned);
    fn->code = NULL;
    fn->code_map = NULL;
    fn->owned = NULL;
    fn->owned_n = fn->owned_cap = 0;
    fn->compiled = false;
}

void vm_run_top_level(VM *vm) {
    FuncSym top = {0};
    top.name = xstrdup("<top>");
    top.body_start = 0;
    top.body_end = vm->toks.n;
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
    for (size_t i = 0; i < vm->frames.n; ++i) {
        frame_release_locals(&vm->frames.v[i]);
        free(vm->frames.v[i].local_mems);
    }
    free(vm->frames.v);
}
