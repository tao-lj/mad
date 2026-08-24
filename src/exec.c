// Interpreter core: frames, opcode dispatch, variable resolution.
#include "vm.h"

#include "common.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool word_is(const char *s, const char *kw) { return strcmp(s, kw) == 0; }

static bool is_top_level(Frame *fr) { return strcmp(fr->fn->name, "<top>") == 0; }

// ---------- Value accessors ----------

static double value_to_f64(VM *vm, Value v, const Token *t) {
    if (v.type != T_F64) fatal_at(t->line, "expected f64, got %s", type_name(v.type));
    return vm->f64.v[v.idx];
}

static int64_t get_i64(VM *vm, Value v, const Token *t) {
    if (v.type != T_I64) fatal_at(t->line, "expected i64, got %s", type_name(v.type));
    return vm->i64.v[v.idx];
}

static bool is_true(VM *vm, Value v, const Token *t) {
    if (v.type == T_BOOL) return vm->bytes.v[v.idx] != 0;
    if (v.type == T_I64) return vm->i64.v[v.idx] != 0;
    if (v.type == T_U64) return vm->u64.v[v.idx] != 0;
    fatal_at(t->line, "expected boolean/integer condition, got %s", type_name(v.type));
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

static void require_initialized(Var *v, const char *name, const Token *t) {
    if (!v->initialized) fatal_at(t->line, "uninitialized variable '%s'", name);
}

static Value load_var(VM *vm, Frame *fr, const char *name, const Token *t) {
    bool unused = false;
    Var *v = resolve_var(vm, fr, name, &unused);
    if (!v) {
        if (global_declared_in(fr, name))
            fatal_at(t->line, "declared global variable '%s' does not exist", name);
        fatal_at(t->line, "unknown variable '%s'", name);
    }
    require_initialized(v, name, t);
    return v->value;
}

static Value make_ptr_value(VM *vm, Frame *fr, const char *name, const Token *t) {
    bool g = false;
    Var *v = resolve_var(vm, fr, name, &g);
    if (!v) {
        if (global_declared_in(fr, name))
            fatal_at(t->line, "declared global variable '%s' does not exist", name);
        fatal_at(t->line, "unknown variable '%s'", name);
    }
    if (g) {
        size_t gi = (size_t)(v - vm->globals.v);
        return (Value){T_PTR, ptr_new(&vm->ptrs, (PtrRef){0, 0, true, gi})};
    }
    size_t li = (size_t)(v - fr->locals.v);
    return (Value){T_PTR, ptr_new(&vm->ptrs, (PtrRef){fr->frame_id, li, false, 0})};
}

static Var *ptr_target(VM *vm, Value pv, const Token *t) {
    if (pv.type != T_PTR) fatal_at(t->line, "expected ptr, got %s", type_name(pv.type));
    PtrRef r = vm->ptrs.v[pv.idx];
    if (r.is_global) {
        if (r.global_index >= vm->globals.n)
            fatal_at(t->line, "dangling global pointer");
        return &vm->globals.v[r.global_index];
    }
    if (r.frame_id >= vm->frames.n) fatal_at(t->line, "dangling local pointer");
    Frame *owner = &vm->frames.v[r.frame_id];
    if (r.local_index >= owner->locals.n) fatal_at(t->line, "dangling local pointer");
    return &owner->locals.v[r.local_index];
}

static void assign_var(Var *dst, Value value, const Token *t) {
    if (dst->type != value.type) {
        fatal_at(t->line, "type mismatch in assignment: %s <- %s",
                 type_name(dst->type), type_name(value.type));
    }
    dst->value = value;
    dst->initialized = true;
}

static Value cast_value(VM *vm, Value v, TypeKind ty, const Token *tok) {
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
    fatal_at(tok->line, "unsupported cast from %s to %s", type_name(v.type), type_name(ty));
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

static Value parse_literal(VM *vm, const Token *t) {
    switch (t->kind) {
    case TOK_INT:
        return make_i64(&vm->i64, strtoll(t->text, NULL, 10));
    case TOK_UINT:
        return make_u64(&vm->u64, strtoull(t->text + 2, NULL, 10));
    case TOK_FLOAT:
        return make_f64(&vm->f64, strtod(t->text, NULL));
    case TOK_STRING: {
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
        return (Value){T_MEMPTR, memptr_new(&vm->memptrs, mid)};
    }
    default:
        return (Value){T_I64, 0};
    }
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
static uint64_t mem_id_of(VM *vm, Value v, const Token *t, const char *op) {
    if (v.type == T_MEM) return v.idx;
    if (v.type == T_MEMPTR) return vm->memptrs.v[v.idx].mem_id;
    fatal_at(t->line, "%s expects mem/memptr, got %s", op, type_name(v.type));
    return 0;
}

static MemObj *require_mem(VM *vm, uint64_t mid, const Token *t) {
    if (mid >= vm->mems.n || vm->mems.v[mid].data == NULL)
        fatal_at(t->line, "invalid or freed memory object");
    return &vm->mems.v[mid];
}

static TypeKind token_read_type(const Token *t, const char *what) {
    const char *at = strchr(t->text, '@');
    if (!at || !at[1]) fatal_at(t->line, "%s requires type, e.g. %si64", what, t->text);
    TypeKind ty;
    if (!is_type_name(at + 1, &ty)) fatal_at(t->line, "unknown %s type '%s'", what, at + 1);
    return ty;
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

static void execute_function(VM *vm, FuncSym *fn);

static void call_by_value(VM *vm, FuncSym *fn, const Token *t) {
    if (vm->stack.n < fn->param_count)
        fatal_at(t->line, "not enough arguments for function '%s'", fn->name);

    VEC_GROW(vm->frames.v, vm->frames.n, vm->frames.cap, Frame);
    size_t frame_id = vm->frames.n;
    Frame *nf = &vm->frames.v[frame_id];
    *nf = (Frame){0};
    nf->fn = fn;
    nf->pc = fn->body_start;
    nf->frame_id = frame_id;
    vm->frames.n++;

    // [] reverses the source group, so the first runtime argument is popped first.
    for (size_t i = 0; i < fn->param_count; ++i) {
        Value v = valstack_pop(&vm->stack, t->text);
        if (fn->param_types[i] != v.type) {
            fatal_at(t->line, "argument %zu of '%s' has type %s, expected %s",
                     i + 1, fn->name, type_name(v.type), type_name(fn->param_types[i]));
        }
        if (vm_find_var(&vm->globals, fn->params[i])) {
            fatal_at(t->line, "local parameter '%s' conflicts with global variable", fn->params[i]);
        }
        if (vm_find_var(&nf->locals, fn->params[i])) {
            fatal_at(t->line, "duplicate parameter '%s'", fn->params[i]);
        }
        vm_add_var(&nf->locals, fn->params[i], fn->param_types[i], true, v);
    }

    execute_function(vm, fn);
    frame_release_local_mem(vm, &vm->frames.v[frame_id]);
    frame_release_locals(&vm->frames.v[frame_id]);
    vm->frames.n = frame_id;
}

// ---------- Opcode groups ----------
// Each handler returns true when it consumed the word.

static bool op_arithmetic(VM *vm, const Token *t) {
    const char *s = t->text;
    bool plus = word_is(s, "+"), minus = word_is(s, "-"), times = word_is(s, "*");
    bool div = word_is(s, "/"), rem = word_is(s, "%");
    if (!plus && !minus && !times && !div && !rem) return false;

    Value b = valstack_pop(&vm->stack, s);
    Value a = valstack_pop(&vm->stack, s);
    if (a.type == T_F64 || b.type == T_F64) {
        double x = value_to_f64(vm, a, t);
        double y = value_to_f64(vm, b, t);
        double r = 0;
        if (plus) r = x + y;
        else if (minus) r = x - y;
        else if (times) r = x * y;
        else if (div) r = x / y;
        else fatal_at(t->line, "%% is not defined for f64");
        valstack_push(&vm->stack, make_f64(&vm->f64, r));
        return true;
    }

    int64_t x = get_i64(vm, a, t);
    int64_t y = get_i64(vm, b, t);
    int64_t r = 0;
    if (plus) r = x + y;
    else if (minus) r = x - y;
    else if (times) r = x * y;
    else if (div) {
        if (!y) fatal_at(t->line, "division by zero");
        r = x / y;
    } else {
        if (!y) fatal_at(t->line, "division by zero");
        r = x % y;
    }
    valstack_push(&vm->stack, make_i64(&vm->i64, r));
    return true;
}

static bool op_comparison(VM *vm, const Token *t) {
    const char *s = t->text;
    if (!word_is(s, "==") && !word_is(s, "!=") && !word_is(s, "<") &&
        !word_is(s, ">") && !word_is(s, "<=") && !word_is(s, ">=")) {
        return false;
    }

    Value b = valstack_pop(&vm->stack, s);
    Value a = valstack_pop(&vm->stack, s);
    bool r = false;
    if (a.type == T_I64 && b.type == T_I64) {
        int64_t x = vm->i64.v[a.idx], y = vm->i64.v[b.idx];
        if (word_is(s, "==")) r = x == y;
        else if (word_is(s, "!=")) r = x != y;
        else if (word_is(s, "<")) r = x < y;
        else if (word_is(s, ">")) r = x > y;
        else if (word_is(s, "<=")) r = x <= y;
        else r = x >= y;
    } else if (a.type == T_F64 && b.type == T_F64) {
        double x = vm->f64.v[a.idx], y = vm->f64.v[b.idx];
        if (word_is(s, "==")) r = x == y;
        else if (word_is(s, "!=")) r = x != y;
        else if (word_is(s, "<")) r = x < y;
        else if (word_is(s, ">")) r = x > y;
        else if (word_is(s, "<=")) r = x <= y;
        else r = x >= y;
    } else {
        fatal_at(t->line, "comparison requires equal scalar types, got %s", type_name(a.type));
    }
    valstack_push(&vm->stack, make_bool(&vm->bytes, r));
    return true;
}

static bool op_memory(VM *vm, Frame *fr, const Token *t) {
    const char *s = t->text;

    if (word_is(s, "alloc") || word_is(s, "halloc")) {
        Value n = valstack_pop(&vm->stack, s);
        int64_t bytes = get_i64(vm, n, t);
        if (bytes < 0) fatal_at(t->line, "negative allocation");
        bool heap = word_is(s, "halloc");
        uint64_t id = mem_new(&vm->mems, (size_t)bytes, heap, false);
        if (!heap) frame_track_local_mem(fr, id);
        valstack_push(&vm->stack, (Value){T_MEM, id});
        return true;
    }

    if (word_is(s, "free")) {
        Value m = valstack_pop(&vm->stack, s);
        uint64_t id;
        if (m.type == T_MEM) {
            id = m.idx;
        } else if (m.type == T_MEMPTR) {
            if (m.idx >= vm->memptrs.n) fatal_at(t->line, "invalid memptr");
            id = vm->memptrs.v[m.idx].mem_id;
        } else {
            fatal_at(t->line, "free expects mem/memptr, got %s", type_name(m.type));
        }
        if (id >= vm->mems.n || vm->mems.v[id].data == NULL)
            fatal_at(t->line, "invalid or already freed mem id");
        free(vm->mems.v[id].data);
        vm->mems.v[id].data = NULL;
        vm->mems.v[id].len = 0;
        return true;
    }

    if (strncmp(s, "mread@", 6) == 0) {
        Value off = valstack_pop(&vm->stack, "read offset");
        Value memv = valstack_pop(&vm->stack, "read mem");
        uint64_t mid = mem_id_of(vm, memv, t, "read");
        TypeKind ty = token_read_type(t, "mread");
        MemObj *m = require_mem(vm, mid, t);
        size_t o = (size_t)get_i64(vm, off, t);
        size_t sz;
        switch (ty) {
        case T_I64: case T_U64: case T_F64: sz = 8; break;
        case T_BOOL: case T_CHAR: sz = 1; break;
        default: fatal_at(t->line, "mread supports scalar types only, got %s", type_name(ty)); return true;
        }
        if (o + sz > m->len) fatal_at(t->line, "read out of bounds");
        switch (ty) {
        case T_I64: { int64_t x; memcpy(&x, m->data + o, 8); valstack_push(&vm->stack, make_i64(&vm->i64, x)); } break;
        case T_U64: { uint64_t x; memcpy(&x, m->data + o, 8); valstack_push(&vm->stack, make_u64(&vm->u64, x)); } break;
        case T_F64: { double x; memcpy(&x, m->data + o, 8); valstack_push(&vm->stack, make_f64(&vm->f64, x)); } break;
        case T_BOOL: valstack_push(&vm->stack, make_bool(&vm->bytes, m->data[o] != 0)); break;
        default: valstack_push(&vm->stack, make_char(&vm->bytes, m->data[o])); break;
        }
        return true;
    }

    if (strncmp(s, "write@", 6) == 0) {
        Value off = valstack_pop(&vm->stack, "write offset");
        Value memv = valstack_pop(&vm->stack, "write mem");
        Value val = valstack_pop(&vm->stack, "write value");
        uint64_t mid = mem_id_of(vm, memv, t, "write");
        TypeKind ty = token_read_type(t, "write");
        if (val.type != ty) {
            fatal_at(t->line, "write type mismatch: value is %s but write@%s requested",
                     type_name(val.type), type_name(ty));
        }
        MemObj *m = require_mem(vm, mid, t);
        if (m->readonly) fatal_at(t->line, "cannot write read-only memory");
        size_t sz;
        switch (ty) {
        case T_I64: case T_U64: case T_F64: sz = 8; break;
        case T_BOOL: case T_CHAR: sz = 1; break;
        default: fatal_at(t->line, "write supports scalar types only, got %s", type_name(ty)); return true;
        }
        size_t o = (size_t)get_i64(vm, off, t);
        if (o + sz > m->len) fatal_at(t->line, "write out of bounds");
        switch (ty) {
        case T_I64: memcpy(m->data + o, &vm->i64.v[val.idx], 8); break;
        case T_U64: memcpy(m->data + o, &vm->u64.v[val.idx], 8); break;
        case T_F64: memcpy(m->data + o, &vm->f64.v[val.idx], 8); break;
        default: m->data[o] = vm->bytes.v[val.idx]; break;
        }
        return true;
    }

    return false;
}

static void do_stdin_read(VM *vm, const Token *t) {
    TypeKind ty = token_read_type(t, "read");
    switch (ty) {
    case T_I64: {
        int64_t x;
        if (scanf("%" SCNd64, &x) != 1) fatal_at(t->line, "failed to read i64");
        valstack_push(&vm->stack, make_i64(&vm->i64, x));
        break;
    }
    case T_U64: {
        uint64_t x;
        if (scanf("%" SCNu64, &x) != 1) fatal_at(t->line, "failed to read u64");
        valstack_push(&vm->stack, make_u64(&vm->u64, x));
        break;
    }
    case T_F64: {
        double x;
        if (scanf("%lf", &x) != 1) fatal_at(t->line, "failed to read f64");
        valstack_push(&vm->stack, make_f64(&vm->f64, x));
        break;
    }
    case T_CHAR: {
        unsigned char c;
        if (scanf(" %c", &c) != 1) fatal_at(t->line, "failed to read char");
        valstack_push(&vm->stack, make_char(&vm->bytes, c));
        break;
    }
    case T_BOOL: {
        char buf[64];
        if (scanf("%63s", buf) != 1) fatal_at(t->line, "failed to read bool");
        if (word_is(buf, "true") || word_is(buf, "1")) {
            valstack_push(&vm->stack, make_bool(&vm->bytes, true));
        } else if (word_is(buf, "false") || word_is(buf, "0")) {
            valstack_push(&vm->stack, make_bool(&vm->bytes, false));
        } else {
            fatal_at(t->line, "invalid bool input '%s'", buf);
        }
        break;
    }
    default:
        fatal_at(t->line, "read@%s is not supported by the MVP input module", strchr(t->text, '@') + 1);
    }
}

static bool op_io(VM *vm, const Token *t) {
    const char *s = t->text;

    if (strncmp(s, "read@", 5) == 0) {
        do_stdin_read(vm, t);
        return true;
    }
    if (word_is(s, "print") || word_is(s, "printn")) {
        print_value(vm, valstack_pop(&vm->stack, s));
        return true;
    }
    if (word_is(s, "println")) {
        putchar('\n');
        return true;
    }
    if (word_is(s, "printstr")) {
        Value v = valstack_pop(&vm->stack, s);
        uint64_t mid = mem_id_of(vm, v, t, "printstr");
        MemObj *m = require_mem(vm, mid, t);
        for (size_t k = 0; k < m->len && m->data[k]; ++k) putchar((char)m->data[k]);
        return true;
    }
    return false;
}

static bool op_jump(VM *vm, Frame *fr, FuncSym *fn, const Token *t) {
    const char *s = t->text;
    if (!word_is(s, "jz") && !word_is(s, "jmp") && !word_is(s, "jump")) return false;

    Value target = valstack_pop(&vm->stack, s);
    if (target.type != T_LABEL)
        fatal_at(t->line, "%s expects label, got %s", s, type_name(target.type));
    if (target.idx >= fn->labels.n) fatal_at(t->line, "invalid label id");

    if (word_is(s, "jz")) {
        // MVP convention: jz jumps when the condition is true/non-zero.
        Value cond = valstack_pop(&vm->stack, s);
        if (!is_true(vm, cond, t)) return true;
    }
    fr->pc = fn->labels.v[target.idx].token_index;
    return true;
}

// Pushes a first-class label or func reference for '&name'-style lookups.
static bool try_push_named_ref(VM *vm, FuncSym *fn, const char *name) {
    LabelSym *ls = vm_find_label(fn, name);
    if (ls) {
        valstack_push(&vm->stack, (Value){T_LABEL, (uint64_t)(ls - fn->labels.v)});
        return true;
    }
    FuncSym *ff = vm_find_func(&vm->funcs, name);
    if (ff) {
        valstack_push(&vm->stack, (Value){T_FUNC, (uint64_t)(ff - vm->funcs.v)});
        return true;
    }
    return false;
}

static void declare_variable(VM *vm, Frame *fr, const char *base, TypeKind ty,
                             Value init, const Token *t) {
    if (is_top_level(fr)) {
        if (vm_find_var(&vm->globals, base))
            fatal_at(t->line, "global variable '%s' already exists", base);
        vm_add_var(&vm->globals, base, ty, true, init);
    } else {
        if (vm_find_var(&vm->globals, base))
            fatal_at(t->line, "local variable '%s' conflicts with global variable", base);
        vm_add_var(&fr->locals, base, ty, true, init);
    }
}

// Fallback for plain words: load an existing variable or implicitly declare
// one by consuming the stack top.
static void handle_variable_word(VM *vm, Frame *fr, const Token *t) {
    const char *s = t->text;
    char base[MAX_NAME];
    bool has_ty = false;
    TypeKind ann = T_I64;
    split_annotated_name(s, base, sizeof(base), &has_ty, &ann);

    if (!is_var_token(base)) fatal_at(t->line, "unknown token '%s'", s);

    bool unused = false;
    Var *v = resolve_var(vm, fr, base, &unused);
    if (v) {
        if (has_ty && v->type != ann)
            fatal_at(t->line, "variable '%s' already has type %s, not %s",
                     base, type_name(v->type), type_name(ann));
        require_initialized(v, base, t);
        valstack_push(&vm->stack, v->value);
        return;
    }

    if (global_declared_in(fr, base))
        fatal_at(t->line, "declared global variable '%s' does not exist", base);

    Value init = valstack_pop(&vm->stack, s);
    TypeKind ty = has_ty ? ann : init.type;
    if (ty != init.type) {
        fatal_at(t->line, "initializer type %s does not match declared type %s",
                 type_name(init.type), type_name(ty));
    }
    declare_variable(vm, fr, base, ty, init, t);
}

static void execute_function(VM *vm, FuncSym *fn) {
    const size_t frame_id = vm->frames.n - 1;
    while (!vm->halted) {
        // Re-fetch the frame every iteration: nested calls may realloc the vector.
        Frame *fr = &vm->frames.v[frame_id];
        if (fr->pc >= fn->body_end) break;

        size_t ip = fr->pc++;
        Token *t = &vm->toks.v[ip];

        if (t->kind == TOK_SEMI) return; // implicit ret
        if (t->kind == TOK_PARAM_END) continue;

        if (t->kind == TOK_COLON && is_top_level(fr)) {
            // Function bodies are skipped during top-level execution.
            size_t j = ip + 1;
            while (j < fn->body_end && vm->toks.v[j].kind != TOK_SEMI) ++j;
            if (j >= fn->body_end) fatal_at(t->line, "unterminated function definition");
            fr->pc = j + 1;
            continue;
        }

        bool literal = t->kind == TOK_INT || t->kind == TOK_UINT ||
                       t->kind == TOK_FLOAT || t->kind == TOK_STRING;
        if (!literal && t->kind != TOK_WORD) continue;
        if (literal) {
            valstack_push(&vm->stack, parse_literal(vm, t));
            continue;
        }

        const char *s = t->text;
        size_t len = strlen(s);
        if (len > 0 && s[len - 1] == ':') continue; // label definition

        if (s[0] == '&') {
            const char *name = s + 1;
            bool g = false;
            Var *vv = resolve_var(vm, fr, name, &g);
            if (vv) {
                if (vv->type == T_MEM) {
                    // MVP: &mem yields a memptr to the memory object stored in that variable.
                    require_initialized(vv, name, t);
                    if (vv->value.type != T_MEM)
                        fatal_at(t->line, "internal mem variable type error");
                    valstack_push(&vm->stack,
                                  (Value){T_MEMPTR, memptr_new(&vm->memptrs, vv->value.idx)});
                } else {
                    valstack_push(&vm->stack, make_ptr_value(vm, fr, name, t));
                }
                continue;
            }
            if (try_push_named_ref(vm, fn, name)) continue;
            fatal_at(t->line, "unknown reference '&%s'", name);
        }

        if (s[0] == '*' && s[1]) {
            Value pv = load_var(vm, fr, s + 1, t);
            Var *target = ptr_target(vm, pv, t);
            if (!target->initialized)
                fatal_at(t->line, "dereferenced uninitialized pointer '%s'", s + 1);
            valstack_push(&vm->stack, target->value);
            continue;
        }

        if (strncmp(s, "!@", 2) == 0) {
            TypeKind ty;
            if (!is_type_name(s + 2, &ty)) fatal_at(t->line, "unknown cast type '%s'", s + 2);
            Value v = valstack_pop(&vm->stack, s);
            valstack_push(&vm->stack, cast_value(vm, v, ty, t));
            continue;
        }

        // Bare label names and function names are looked up before builtins,
        // so user definitions shadow builtin words.
        LabelSym *ls = vm_find_label(fn, s);
        if (ls) {
            valstack_push(&vm->stack, (Value){T_LABEL, (uint64_t)(ls - fn->labels.v)});
            continue;
        }
        FuncSym *callee = vm_find_func(&vm->funcs, s);
        if (callee) {
            call_by_value(vm, callee, t);
            continue;
        }

        if (word_is(s, "ret")) return;
        if (word_is(s, "halt")) { vm->halted = true; return; }

        if (word_is(s, "dup")) {
            valstack_push(&vm->stack, valstack_peek(&vm->stack, s));
            continue;
        }
        if (word_is(s, "drop")) {
            (void)valstack_pop(&vm->stack, s);
            continue;
        }
        if (word_is(s, "swap")) {
            Value a = valstack_pop(&vm->stack, s);
            Value b = valstack_pop(&vm->stack, s);
            valstack_push(&vm->stack, a);
            valstack_push(&vm->stack, b);
            continue;
        }

        if (op_arithmetic(vm, t)) continue;
        if (op_comparison(vm, t)) continue;

        if (word_is(s, "=")) {
            Value pv = valstack_pop(&vm->stack, s);
            Value val = valstack_pop(&vm->stack, s);
            assign_var(ptr_target(vm, pv, t), val, t);
            continue;
        }

        if (word_is(s, "call")) {
            Value fv = valstack_pop(&vm->stack, s);
            if (fv.type != T_FUNC)
                fatal_at(t->line, "call expects func, got %s", type_name(fv.type));
            if (fv.idx >= vm->funcs.n) fatal_at(t->line, "invalid func id");
            call_by_value(vm, &vm->funcs.v[fv.idx], t);
            continue;
        }

        if (op_jump(vm, fr, fn, t)) continue;
        if (op_memory(vm, fr, t)) continue;
        if (op_io(vm, t)) continue;

        if (word_is(s, "assert")) {
            Value v = valstack_pop(&vm->stack, s);
            if (!is_true(vm, v, t)) fatal_at(t->line, "assertion failed");
            continue;
        }

        handle_variable_word(vm, fr, t);
    }
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
    fr->pc = 0;
    fr->frame_id = vm->frames.n;
    const size_t frame_id = vm->frames.n;
    vm->frames.n++;

    execute_function(vm, &top);

    vm->frames.n = frame_id;
    frame_release_local_mem(vm, &vm->frames.v[frame_id]);
    frame_release_locals(&vm->frames.v[frame_id]);
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
    }
    free(vm->funcs.v);
    free(vm->stack.v);
    for (size_t i = 0; i < vm->frames.n; ++i) {
        frame_release_locals(&vm->frames.v[i]);
        free(vm->frames.v[i].local_mems);
    }
    free(vm->frames.v);
}
