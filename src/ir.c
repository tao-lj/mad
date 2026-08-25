// ir.c — MAD intermediate representation: builder, optimizer, checker, lower.
//
// The IR is a flat, linear instruction stream emitted from the same token
// classification logic the old compile_func used.  After building, a small
// constant-folding pass runs, then the IR is lowered into the threaded Op
// array consumed by the execute_code() dispatch loop.
//
// ir.c owns all compilation helpers (compile_string_literal, etc.) that
// were previously static in the old monolithic tcode.c.

#include "ir.h"
#include "common.h"
#include "vm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Helpers (moved from old tcode.c / exec.c)
// ---------------------------------------------------------------------------

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

static TypeKind compile_type_suffix(const Token *t, const char *what) {
    const char *at = strchr(t->text, '@');
    if (!at || !at[1])
        fatal_at(t->line, "%s requires type, e.g. %si64", what, t->text);
    TypeKind ty;
    if (!is_type_name(at + 1, &ty))
        fatal_at(t->line, "unknown %s type '%s'", what, at + 1);
    return ty;
}

// ---------------------------------------------------------------------------
//  Builder internals
// ---------------------------------------------------------------------------

typedef struct {
    size_t ir_idx;  // jump op whose aux needs patching
    size_t map_idx; // body-relative destination token index
} IrFixup;

typedef struct {
    VM *vm;
    FuncSym *fn;
    bool top_level;
    IrNode *v;
    size_t n, cap;
    size_t *map;   // body-relative token index -> first ir emitted there
    size_t map_n;
    IrFixup *fix;
    size_t fix_n, fix_cap;
    int pending_label;
    const Token *pending_tok;
    const Token *prev_tok;  // previous non-dead token (for ':' after "name")
    // — declared-name set for LOAD/DECLARE disambiguation —
    char **declared;
    size_t declared_n, declared_cap;
    // — compile-time type stack for typed opcode emission —
    TypeKind *ty_stack;
    size_t ty_n, ty_cap;
} IrBuilder;

// Compile-time type stack helpers.
static void tys_push(IrBuilder *b, TypeKind t) {
    VEC_GROW(b->ty_stack, b->ty_n, b->ty_cap, TypeKind);
    b->ty_stack[b->ty_n++] = t;
}
static bool tys_pop2(IrBuilder *b, TypeKind *a, TypeKind *bb) {
    if (b->ty_n < 2) return false;
    *bb = b->ty_stack[--b->ty_n];
    *a  = b->ty_stack[--b->ty_n];
    return true;
}

static void irb_own(IrBuilder *b, char *s) {
    VEC_GROW(b->fn->owned, b->fn->owned_n, b->fn->owned_cap, char *);
    b->fn->owned[b->fn->owned_n++] = s;
}

static bool irb_name_in_list(const char *name, char **list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(name, list[i]) == 0) return true;
    return false;
}

static bool irb_is_loaded(IrBuilder *b, const char *name) {
    if (irb_name_in_list(name, b->fn->params, b->fn->param_count)) return true;
    if (irb_name_in_list(name, b->fn->globals, b->fn->global_count)) return true;
    for (size_t i = 0; i < b->declared_n; ++i)
        if (strcmp(name, b->declared[i]) == 0) return true;
    return false;
}

static void irb_declare(IrBuilder *b, const char *name) {
    VEC_GROW(b->declared, b->declared_n, b->declared_cap, char *);
    b->declared[b->declared_n++] = (char *)name; // borrows from ir->u.name
}

static size_t irb_emit(IrBuilder *b, IrKind kind, const Token *t) {
    VEC_GROW(b->v, b->n, b->cap, IrNode);
    IrNode *ir = &b->v[b->n++];
    memset(ir, 0, sizeof *ir);
    ir->kind = kind;
    ir->text = t ? t->text : "<end>";
    ir->line = t ? t->line : 0;
    ir->aux  = -1;
    ir->aux2 = -1;
    ir->label_idx = -1;
    return b->n - 1;
}

static void irb_flush(IrBuilder *b) {
    if (b->pending_label < 0) return;
    size_t idx = irb_emit(b, IR_PUSH_LABEL, b->pending_tok);
    b->v[idx].u.u = (uint64_t)b->pending_label;
    b->v[idx].label_idx = b->pending_label;
    b->pending_label = -1;
    b->pending_tok = NULL;
    b->ty_n = 0;  // clear compile-time type stack at control flow boundary
}

static void irb_const(IrBuilder *b, IrKind kind, const Token *t, uint64_t payload) {
    size_t idx = irb_emit(b, kind, t);
    b->v[idx].u.u = payload;
}

static void irb_static_jump(IrBuilder *b, IrKind kind, const Token *t,
                            LabelSym *target, int64_t label_id) {
    size_t idx = irb_emit(b, kind, t);
    b->v[idx].aux = label_id;
    b->v[idx].label_idx = label_id;
    b->ty_n = 0;  // clear compile-time type stack at control flow boundary
    VEC_GROW(b->fix, b->fix_n, b->fix_cap, IrFixup);
    b->fix[b->fix_n].ir_idx  = idx;
    b->fix[b->fix_n].map_idx = target->token_index - b->fn->body_start;
    b->fix_n++;
}

static void irb_ref_or_deref(IrBuilder *b, const Token *t) {
    const char *s = t->text;
    if (s[0] == '&') {
        const char *name = s + 1;
        size_t idx = irb_emit(b, IR_REF, t);
        b->v[idx].u.name = xstrdup(name);
        irb_own(b, b->v[idx].u.name);
        LabelSym *ls = vm_find_label(b->fn, name);
        if (ls) b->v[idx].aux = (int64_t)(ls - b->fn->labels.v);
        FuncSym *fs = vm_find_func(&b->vm->funcs, name);
        if (fs) b->v[idx].aux2 = (int64_t)(fs - b->vm->funcs.v);
        return;
    }
    // '*name'
    size_t idx = irb_emit(b, IR_DEREF, t);
    b->v[idx].u.name = xstrdup(s + 1);
    irb_own(b, b->v[idx].u.name);
}

static void irb_word(IrBuilder *b, const Token *t) {
    VM *vm = b->vm;
    const char *s = t->text;

    // Label definitions are pure position markers; emit IR_LABEL_DEF.
    size_t len = strlen(s);
    if (len && s[len - 1] == ':') {
        char tmp[MAX_NAME];
        memcpy(tmp, s, len - 1);
        tmp[len - 1] = '\0';
        LabelSym *ls = vm_find_label(b->fn, tmp);
        if (ls) {
            size_t idx = irb_emit(b, IR_LABEL_DEF, t);
            b->v[idx].label_idx = (int64_t)(ls - b->fn->labels.v);
        }
        b->ty_n = 0;  // clear compile-time type stack at basic block boundary
        return;
    }

    LabelSym *ls = vm_find_label(b->fn, s);
    if (ls) {
        if (b->pending_label >= 0) irb_flush(b);
        b->pending_label = (int)(ls - b->fn->labels.v);
        b->pending_tok = t;
        return;
    }

    FuncSym *fs = vm_find_func(&vm->funcs, s);
    if (fs) {
        irb_flush(b);
        size_t idx = irb_emit(b, IR_CALL, t);
        b->v[idx].u.i  = (int64_t)(fs - vm->funcs.v);
        b->v[idx].aux2 = (int64_t)(fs - vm->funcs.v);
        return;
    }

    bool is_jz = word_is(s, "jz"), is_jnz = word_is(s, "jnz");
    bool is_jmp = word_is(s, "jmp") || word_is(s, "jump");
    if ((is_jz || is_jnz || is_jmp) && b->pending_label >= 0) {
        LabelSym *target = &b->fn->labels.v[b->pending_label];
        IrKind k = is_jmp ? IR_JMP : (is_jz ? IR_JZ : IR_JNZ);
        irb_static_jump(b, k, t, target, b->pending_label);
        b->pending_label = -1;
        b->pending_tok = NULL;
        return;
    }
    irb_flush(b);

    if (word_is(s, "ret"))    { irb_emit(b, IR_RET, t); b->ty_n = 0; return; }
    if (word_is(s, "halt"))   { irb_emit(b, IR_HALT, t); b->ty_n = 0; return; }
    if (word_is(s, "dup"))    {
        irb_emit(b, IR_DUP, t);
        if (b->ty_n >= 1) { TypeKind top = b->ty_stack[b->ty_n - 1]; tys_push(b, top); }
        return;
    }
    if (word_is(s, "drop"))   { irb_emit(b, IR_DROP, t); if (b->ty_n >= 1) b->ty_n--; return; }
    if (word_is(s, "swap"))   {
        irb_emit(b, IR_SWAP, t);
        if (b->ty_n >= 2) { TypeKind a = b->ty_stack[b->ty_n - 2]; b->ty_stack[b->ty_n - 2] = b->ty_stack[b->ty_n - 1]; b->ty_stack[b->ty_n - 1] = a; }
        return;
    }

    static const char *arith_words[] = {"+", "-", "*", "/", "%"};
    for (int k = 0; k <= 4; ++k) {
        if (word_is(s, arith_words[k])) {
            TypeKind a, bb;
            if (tys_pop2(b, &a, &bb) && a == bb) {
                if (type_is_i64(a)) {
                    static const IrKind tk[] = {IR_ADD_I64, IR_SUB_I64, IR_MUL_I64, IR_DIV_I64, IR_MOD_I64};
                    size_t idx = irb_emit(b, tk[k], t);
                    b->v[idx].ty = T_I64; tys_push(b, T_I64);
                    return;
                }
                if (type_is_f64(a) && k < 4) {
                    static const IrKind fk[] = {IR_ADD_F64, IR_SUB_F64, IR_MUL_F64, IR_DIV_F64};
                    size_t idx = irb_emit(b, fk[k], t);
                    b->v[idx].ty = T_F64; tys_push(b, T_F64);
                    return;
                }
            }
            // Generic fallback.
            static const IrKind gk[] = {IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD};
            irb_emit(b, gk[k], t);
            tys_push(b, T_NONE);
            return;
        }
    }
    static const char *cmp_words[] = {"==", "!=", "<", ">", "<=", ">="};
    for (int k = 0; k <= 5; ++k) {
        if (word_is(s, cmp_words[k])) {
            TypeKind a, bb;
            if (tys_pop2(b, &a, &bb) && a == bb) {
                if (type_is_i64(a)) {
                    static const IrKind tk[] = {IR_EQ_I64, IR_NE_I64, IR_LT_I64, IR_GT_I64, IR_LE_I64, IR_GE_I64};
                    size_t idx = irb_emit(b, tk[k], t);
                    b->v[idx].ty = T_BOOL; tys_push(b, T_BOOL);
                    return;
                }
                if (type_is_f64(a)) {
                    static const IrKind fk[] = {IR_EQ_F64, IR_NE_F64, IR_LT_F64, IR_GT_F64, IR_LE_F64, IR_GE_F64};
                    size_t idx = irb_emit(b, fk[k], t);
                    b->v[idx].ty = T_BOOL; tys_push(b, T_BOOL);
                    return;
                }
            }
            // Generic fallback.
            static const IrKind gk[] = {IR_EQ, IR_NE, IR_LT, IR_GT, IR_LE, IR_GE};
            irb_emit(b, gk[k], t);
            tys_push(b, T_BOOL);
            return;
        }
    }

    if (word_is(s, "="))   { irb_emit(b, IR_ASSIGN, t); return; }

    if (word_is(s, "~")) {
        TypeKind a;
        if (b->ty_n >= 1 && type_is_integer(b->ty_stack[b->ty_n - 1])) {
            a = b->ty_stack[b->ty_n - 1];
            size_t idx = irb_emit(b, IR_BITNOT, t);
            b->v[idx].ty = type_is_i64(a) ? T_I64 : T_I64;
            b->ty_stack[b->ty_n - 1] = T_I64;
        } else {
            irb_emit(b, IR_BITNOT, t);
            if (b->ty_n >= 1) b->ty_stack[b->ty_n - 1] = T_I64;
        }
        return;
    }
    if (word_is(s, "!")) {
        irb_emit(b, IR_LOGNOT, t);
        if (b->ty_n >= 1) b->ty_stack[b->ty_n - 1] = T_BOOL;
        return;
    }
    if (word_is(s, "<<") || word_is(s, ">>")) {
        TypeKind a, bb;
        bool is_shl = word_is(s, "<<");
        if (tys_pop2(b, &a, &bb) && type_is_integer(a) && type_is_integer(bb)) {
            size_t idx = irb_emit(b, is_shl ? IR_SHL_I64 : IR_SHR_I64, t);
            b->v[idx].ty = T_I64; tys_push(b, T_I64);
        } else {
            irb_emit(b, is_shl ? IR_SHL : IR_SHR, t);
            tys_push(b, T_NONE);
        }
        return;
    }
    if (word_is(s, "&") || word_is(s, "|") || word_is(s, "^")) {
        TypeKind a, bb;
        bool is_and = word_is(s, "&"), is_or = word_is(s, "|");
        if (tys_pop2(b, &a, &bb) && type_is_integer(a) && type_is_integer(bb)) {
            IrKind k = is_and ? IR_AND_I64 : (is_or ? IR_OR_I64 : IR_XOR_I64);
            size_t idx = irb_emit(b, k, t);
            b->v[idx].ty = T_I64; tys_push(b, T_I64);
        } else {
            IrKind k = is_and ? IR_AND : (is_or ? IR_OR : IR_XOR);
            irb_emit(b, k, t);
            tys_push(b, T_NONE);
        }
        return;
    }
    if (word_is(s, "&&"))  { irb_emit(b, IR_LOGAND, t); tys_push(b, T_BOOL); return; }
    if (word_is(s, "||"))  { irb_emit(b, IR_LOGOR, t);  tys_push(b, T_BOOL); return; }

    if (word_is(s, "call")){ irb_emit(b, IR_CALL_IND, t); tys_push(b, T_NONE); return; }
    if (is_jz)  { irb_emit(b, IR_JZ_DYN, t);  b->ty_n = 0; return; }
    if (is_jnz) { irb_emit(b, IR_JNZ_DYN, t); b->ty_n = 0; return; }
    if (is_jmp) { irb_emit(b, IR_JMP_DYN, t); b->ty_n = 0; return; }

    if (strncmp(s, "mread@", 6) == 0) {
        size_t idx = irb_emit(b, IR_MREAD, t);
        b->v[idx].ty = compile_type_suffix(t, "mread");
        return;
    }
    if (strncmp(s, "write@", 6) == 0) {
        size_t idx = irb_emit(b, IR_WRITE, t);
        b->v[idx].ty = compile_type_suffix(t, "write");
        return;
    }
    if (strncmp(s, "read@", 5) == 0) {
        size_t idx = irb_emit(b, IR_READ, t);
        b->v[idx].ty = compile_type_suffix(t, "read");
        return;
    }

    if (word_is(s, "alloc"))  { irb_emit(b, IR_ALLOC, t); return; }
    if (word_is(s, "halloc")) {
        size_t idx = irb_emit(b, IR_HALLOC, t);
        b->v[idx].has_ty = true;
        return;
    }
    if (word_is(s, "free"))   { irb_emit(b, IR_FREE, t); return; }
    if (word_is(s, "sizeof")) { irb_emit(b, IR_SIZEOF, t); tys_push(b, T_I64); return; }

    if (word_is(s, "print") || word_is(s, "printn")) {
        irb_emit(b, IR_PRINT, t); return;
    }
    if (word_is(s, "println"))  { irb_emit(b, IR_PRINTLN, t); return; }
    if (word_is(s, "printstr")) { irb_emit(b, IR_PRINTSTR, t); return; }
    if (word_is(s, "assert"))   { irb_emit(b, IR_ASSERT, t); return; }
    if (word_is(s, "import"))   { irb_emit(b, IR_IMPORT, t); return; }

    // Fallback: variable load or implicit declaration from the stack top.
    char base[MAX_NAME];
    bool has_ty = false;
    TypeKind ann = T_I64;
    split_annotated_name(s, base, sizeof(base), &has_ty, &ann);
    if (!is_var_token(base))
        fatal_at(t->line, "unknown token '%s'", s);
    IrKind vk = irb_is_loaded(b, base) ? IR_LOAD : IR_DECLARE;
    size_t idx = irb_emit(b, vk, t);
    b->v[idx].u.name = xstrdup(base);
    irb_own(b, b->v[idx].u.name);
    b->v[idx].ty = ann;
    b->v[idx].has_ty = has_ty;
    if (vk == IR_DECLARE) irb_declare(b, b->v[idx].u.name);
    // Push compile-time type for typed opcode emission.
    tys_push(b, has_ty ? ann : T_NONE);
}

// ---------------------------------------------------------------------------
//  Public: build IR from function body tokens
// ---------------------------------------------------------------------------

IrNode *ir_build(VM *vm, FuncSym *fn, size_t *out_n, size_t **out_map) {
    IrBuilder b = {0};
    b.vm        = vm;
    b.fn        = fn;
    b.top_level = fname_is_module(fn->name);
    b.pending_label = -1;
    b.map_n = fn->body_end - fn->body_start;
    b.map = malloc((b.map_n ? b.map_n : 1) * sizeof *b.map);
    if (!b.map) die_oom();
    for (size_t i = 0; i < b.map_n; ++i) b.map[i] = SIZE_MAX;

    size_t i = fn->body_start;
    while (i < fn->body_end) {
        Token *t = &vm->toks.v[i];
        b.map[i - fn->body_start] = b.n;

        switch (t->kind) {
        case TOK_INT: {
            irb_flush(&b);
            size_t idx = irb_emit(&b, IR_CONST_I64, t);
            b.v[idx].u.i = strtoll(t->text, NULL, 10);
            b.prev_tok = t;
            tys_push(&b, T_I64);
            ++i;
            break;
        }
        case TOK_UINT: {
            irb_flush(&b);
            size_t idx = irb_emit(&b, IR_CONST_U64, t);
            b.v[idx].u.u = strtoull(t->text + 2, NULL, 10);
            b.prev_tok = t;
            tys_push(&b, T_U64);
            ++i;
            break;
        }
        case TOK_FLOAT: {
            irb_flush(&b);
            size_t idx = irb_emit(&b, IR_CONST_F64, t);
            b.v[idx].u.d = strtod(t->text, NULL);
            b.prev_tok = t;
            tys_push(&b, T_F64);
            ++i;
            break;
        }
        case TOK_STRING:
            irb_flush(&b);
            irb_const(&b, IR_CONST_STR, t, compile_string_literal(vm, t));
            b.prev_tok = t;
            tys_push(&b, T_MEM);
            ++i;
            break;
        case TOK_CHAR: {
            irb_flush(&b);
            size_t idx = irb_emit(&b, IR_CONST_I8, t);
            b.v[idx].u.i = (int64_t)(uint8_t)t->text[0];
            b.prev_tok = t;
            tys_push(&b, T_CHAR);
            ++i;
            break;
        }
        case TOK_TYPED: {
            irb_flush(&b);
            const char *colon = strchr(t->text, ':');
            size_t plen = (size_t)(colon - t->text);
            char tname[8];
            memcpy(tname, t->text, plen);
            tname[plen] = '\0';
            TypeKind ty;
            is_type_name(tname, &ty);
            const char *val = colon + 1;
            IrKind ik;
            switch (ty) {
            case T_I8:  ik = IR_CONST_I8;  break;
            case T_U8:  ik = IR_CONST_U8;  break;
            case T_I16: ik = IR_CONST_I16; break;
            case T_U16: ik = IR_CONST_U16; break;
            case T_I32: ik = IR_CONST_I32; break;
            case T_U32: ik = IR_CONST_U32; break;
            case T_I64: ik = IR_CONST_I64; break;
            case T_U64: ik = IR_CONST_U64; break;
            case T_F32: ik = IR_CONST_F32; break;
            case T_F64: ik = IR_CONST_F64; break;
            case T_CHAR: ik = IR_CONST_I8; break;
            default:    ik = IR_CONST_I64; break;
            }
            size_t idx = irb_emit(&b, ik, t);
            switch (ty) {
            case T_I8: case T_I16: case T_I32: case T_I64:
            case T_CHAR:
                b.v[idx].u.i = strtoll(val, NULL, 10); break;
            case T_U8: case T_U16: case T_U32: case T_U64:
                b.v[idx].u.u = strtoull(val, NULL, 10); break;
            case T_F32: case T_F64:
                b.v[idx].u.d = strtod(val, NULL); break;
            default: break;
            }
            b.prev_tok = t;
            tys_push(&b, ty);
            ++i;
            break;
        }
        case TOK_COLON: {
            irb_flush(&b);
            // Emit IR_LABEL_DEF for the preceding word token (e.g. "t1" before ":")
            if (b.prev_tok && b.prev_tok->kind == TOK_WORD) {
                const char *pn = b.prev_tok->text;
                size_t plen = strlen(pn);
                if (plen > 0 && pn[plen - 1] == ':') {
                    char tmp[MAX_NAME];
                    memcpy(tmp, pn, plen - 1);
                    tmp[plen - 1] = '\0';
                    LabelSym *ls = vm_find_label(fn, tmp);
                    if (ls) {
                        size_t idx = irb_emit(&b, IR_LABEL_DEF, b.prev_tok);
                        b.v[idx].label_idx = (int64_t)(ls - fn->labels.v);
                    }
                }
            }
            if (b.top_level) {
                while (i < fn->body_end && vm->toks.v[i].kind != TOK_SEMI)
                    ++i;
            }
            b.prev_tok = t;
            ++i;
            break;
        }
        case TOK_SEMI:
            ++i;
            goto done;
        case TOK_WORD:
            if ((t->text[0] == '&' && t->text[1] && t->text[1] != '&') || (t->text[0] == '*' && t->text[1])) {
                irb_flush(&b);
                irb_ref_or_deref(&b, t);
            } else if (strncmp(t->text, "!@", 2) == 0) {
                irb_flush(&b);
                size_t idx = irb_emit(&b, IR_CAST, t);
                if (!is_type_name(t->text + 2, &b.v[idx].ty))
                    fatal_at(t->line, "unknown cast type '%s'", t->text + 2);
                b.v[idx].has_ty = true;
            } else {
                irb_word(&b, t);
            }
            b.prev_tok = t;
            ++i;
            break;
        default:
            b.prev_tok = t;
            ++i;
            break;
        }
    }
done:
    irb_flush(&b);

    // Sentinel: past-end landing pad for jumps.
    irb_emit(&b, IR_RET, NULL);

    for (size_t j = 0; j < b.fix_n; ++j) {
        size_t d = b.map[b.fix[j].map_idx];
        b.v[b.fix[j].ir_idx].aux = (d == SIZE_MAX) ? (int64_t)(b.n - 1)
                                                     : (int64_t)d;
    }
    for (size_t j = 0; j < b.map_n; ++j) {
        if (b.map[j] == SIZE_MAX) b.map[j] = b.n - 1;
    }
    free(b.fix);

    *out_n = b.n;
    *out_map = b.map;
    return b.v;
}

// ---------------------------------------------------------------------------
//  Optimizer — in-place constant folding
// ---------------------------------------------------------------------------

static int64_t arith_fold(int op, int64_t a, int64_t b) {
    switch (op) {
    case AR_ADD: return a + b;
    case AR_SUB: return a - b;
    case AR_MUL: return a * b;
    case AR_DIV: return b ? a / b : 0;
    case AR_MOD: return b ? a % b : 0;
    }
    return 0;
}

static bool cmp_fold(int op, int64_t a, int64_t b) {
    switch (op) {
    case CMP_EQ: return a == b;
    case CMP_NE: return a != b;
    case CMP_LT: return a <  b;
    case CMP_GT: return a >  b;
    case CMP_LE: return a <= b;
    case CMP_GE: return a >= b;
    }
    return false;
}

static bool cmp_fold_double(int op, double a, double b) {
    switch (op) {
    case CMP_EQ: return a == b;
    case CMP_NE: return a != b;
    case CMP_LT: return a <  b;
    case CMP_GT: return a >  b;
    case CMP_LE: return a <= b;
    case CMP_GE: return a >= b;
    }
    return false;
}

size_t ir_optimize(IrNode *ir, size_t n) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i + 2 < n; ++i) {
            if (ir[i].kind == IR_DEAD || ir[i+1].kind == IR_DEAD ||
                ir[i+2].kind == IR_DEAD)
                continue;
            // i64 arithmetic fold (typed path)
            if (ir[i].kind == IR_CONST_I64 && ir[i+1].kind == IR_CONST_I64) {
                IrKind k = ir[i+2].kind;
                bool is_div = (k == IR_DIV_I64 || k == IR_MOD_I64);
                if (is_div && ir[i+1].u.i == 0) continue;
                if (k == IR_ADD_I64 || k == IR_SUB_I64 || k == IR_MUL_I64 ||
                    k == IR_DIV_I64 || k == IR_MOD_I64) {
                    int op = (k == IR_ADD_I64) ? AR_ADD : (k == IR_SUB_I64) ? AR_SUB :
                             (k == IR_MUL_I64) ? AR_MUL : (k == IR_DIV_I64) ? AR_DIV : AR_MOD;
                    ir[i].kind   = IR_DEAD;
                    ir[i+1].kind = IR_DEAD;
                    ir[i+2].kind = IR_CONST_I64;
                    ir[i+2].u.i  = arith_fold(op, ir[i].u.i, ir[i+1].u.i);
                    changed = true;
                }
            }
            // i64 comparison fold (typed path)
            if (ir[i].kind == IR_CONST_I64 && ir[i+1].kind == IR_CONST_I64) {
                IrKind k = ir[i+2].kind;
                int op = -1;
                if (k == IR_EQ_I64) op = CMP_EQ;
                else if (k == IR_NE_I64) op = CMP_NE;
                else if (k == IR_LT_I64) op = CMP_LT;
                else if (k == IR_GT_I64) op = CMP_GT;
                else if (k == IR_LE_I64) op = CMP_LE;
                else if (k == IR_GE_I64) op = CMP_GE;
                if (op >= 0) {
                    ir[i].kind   = IR_DEAD;
                    ir[i+1].kind = IR_DEAD;
                    ir[i+2].kind = IR_CONST_BOOL;
                    ir[i+2].u.i  = cmp_fold(op, ir[i].u.i, ir[i+1].u.i);
                    changed = true;
                }
            }
            // f64 arithmetic fold (typed path)
            if (ir[i].kind == IR_CONST_F64 && ir[i+1].kind == IR_CONST_F64) {
                IrKind k = ir[i+2].kind;
                bool is_div = (k == IR_DIV_F64);
                if (is_div && ir[i+1].u.d == 0.0) continue;
                if (k == IR_ADD_F64 || k == IR_SUB_F64 || k == IR_MUL_F64 || k == IR_DIV_F64) {
                    ir[i].kind   = IR_DEAD;
                    ir[i+1].kind = IR_DEAD;
                    ir[i+2].kind = IR_CONST_F64;
                    double a = ir[i].u.d, b = ir[i+1].u.d;
                    switch (k) {
                    case IR_ADD_F64: ir[i+2].u.d = a + b; break;
                    case IR_SUB_F64: ir[i+2].u.d = a - b; break;
                    case IR_MUL_F64: ir[i+2].u.d = a * b; break;
                    default:         ir[i+2].u.d = a / b; break;
                    }
                    changed = true;
                }
            }
            // f64 comparison fold (typed path)
            if (ir[i].kind == IR_CONST_F64 && ir[i+1].kind == IR_CONST_F64) {
                IrKind k = ir[i+2].kind;
                int op = -1;
                if (k == IR_EQ_F64) op = CMP_EQ;
                else if (k == IR_NE_F64) op = CMP_NE;
                else if (k == IR_LT_F64) op = CMP_LT;
                else if (k == IR_GT_F64) op = CMP_GT;
                else if (k == IR_LE_F64) op = CMP_LE;
                else if (k == IR_GE_F64) op = CMP_GE;
                if (op >= 0) {
                    ir[i].kind   = IR_DEAD;
                    ir[i+1].kind = IR_DEAD;
                    ir[i+2].kind = IR_CONST_BOOL;
                    double a = ir[i].u.d, b = ir[i+1].u.d;
                    ir[i+2].u.i = cmp_fold_double(op, a, b);
                    changed = true;
                }
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
//  Stack state — simple grow-only TypeKind stack
// ---------------------------------------------------------------------------

void stack_push(StackState *s, TypeKind t) {
    VEC_GROW(s->v, s->n, s->cap, TypeKind);
    s->v[s->n++] = t;
}

bool stack_pop(StackState *s, TypeKind *out) {
    if (s->n == 0) return false;
    *out = s->v[--s->n];
    return true;
}

bool stack_peek(const StackState *s, TypeKind *out) {
    if (s->n == 0) return false;
    *out = s->v[s->n - 1];
    return true;
}

StackState stack_clone(const StackState *s) {
    StackState r = {0};
    if (s->n) {
        r.v = malloc(s->n * sizeof(TypeKind));
        if (!r.v) die_oom();
        memcpy(r.v, s->v, s->n * sizeof(TypeKind));
        r.n = r.cap = s->n;
    }
    return r;
}

bool stack_equal(const StackState *a, const StackState *b) {
    if (a->n != b->n) return false;
    for (size_t i = 0; i < a->n; ++i)
        if (a->v[i] != b->v[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------
//  Type helpers
// ---------------------------------------------------------------------------


static bool is_condition(TypeKind t) {
    return t == T_BOOL || t == T_I64 || t == T_U64;
}

// ---------------------------------------------------------------------------
//  ir_apply — apply one IR instruction to a StackState
// ---------------------------------------------------------------------------

static void ir_error(const IrNode *ir, const char *msg) {
    fprintf(stderr, "check: %s before '%s'\n",
            msg, ir->text ? ir->text : "?");
    if (ir->line) fprintf(stderr, "  (line %zu)\n", ir->line);
}

bool ir_apply(const IrNode *ir, StackState *stack, const FuncSym *fn) {
    TypeKind a, b;
    (void)fn;

    switch (ir->kind) {
    case IR_DEAD:
    case IR_LABEL_DEF:
        return true;

    // — push one value —
    case IR_CONST_I8:   stack_push(stack, T_I8);   return true;
    case IR_CONST_U8:   stack_push(stack, T_U8);   return true;
    case IR_CONST_I16:  stack_push(stack, T_I16);  return true;
    case IR_CONST_U16:  stack_push(stack, T_U16);  return true;
    case IR_CONST_I32:  stack_push(stack, T_I32);  return true;
    case IR_CONST_U32:  stack_push(stack, T_U32);  return true;
    case IR_CONST_I64:  stack_push(stack, T_I64);  return true;
    case IR_CONST_U64:  stack_push(stack, T_U64);  return true;
    case IR_CONST_F32:  stack_push(stack, T_F32);  return true;
    case IR_CONST_F64:  stack_push(stack, T_F64);  return true;
    case IR_CONST_STR:  stack_push(stack, T_MEM);   return true;
    case IR_CONST_BOOL: stack_push(stack, T_BOOL);  return true;
    case IR_PUSH_LABEL: stack_push(stack, T_LABEL);  return true;
    case IR_REF:        stack_push(stack, T_PTR);    return true;
    case IR_DEREF:      stack_push(stack, T_I64);   return true;

    // — IR_LOAD: push named variable (+1) —
    case IR_LOAD:
        stack_push(stack, ir->ty);
        return true;
    // — IR_DECLARE: pop stack top, bind to name (-1) —
    case IR_DECLARE:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before declare"); return false; }
        return true;

    // — cast: consume top, push cast type —
    case IR_CAST:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow"); return false; }
        stack_push(stack, ir->ty);
        return true;

    // — typed arithmetic: pops two, pushes result type —
    case IR_ADD_I64: case IR_SUB_I64: case IR_MUL_I64:
    case IR_DIV_I64: case IR_MOD_I64:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before arith"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before arith"); return false; }
        if (a != T_I64 || b != T_I64) {
            fprintf(stderr, "check: expected i64 operands for '%s', got %s %s\n",
                    ir->text, type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_I64);
        return true;
    case IR_ADD_F64: case IR_SUB_F64: case IR_MUL_F64: case IR_DIV_F64:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before arith"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before arith"); return false; }
        if (a != T_F64 || b != T_F64) {
            fprintf(stderr, "check: expected f64 operands for '%s', got %s %s\n",
                    ir->text, type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_F64);
        return true;

    // — generic arithmetic: T T → T (runtime type check) —
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before arith"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before arith"); return false; }
        if (a != b) {
            fprintf(stderr, "check: type mismatch in '%s': %s %s\n",
                    ir->text ? ir->text : "arith",
                    type_name(a), type_name(b));
            return false;
        }
        if (!is_numeric(a)) {
            fprintf(stderr, "check: arith on non-numeric type %s\n", type_name(a));
            return false;
        }
        stack_push(stack, a);
        return true;

    // — typed comparison: pops two, pushes bool —
    case IR_EQ_I64: case IR_NE_I64: case IR_LT_I64: case IR_GT_I64:
    case IR_LE_I64: case IR_GE_I64:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before cmp"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before cmp"); return false; }
        if (a != T_I64 || b != T_I64) {
            fprintf(stderr, "check: expected i64 operands for '%s', got %s %s\n",
                    ir->text, type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_BOOL);
        return true;
    case IR_EQ_F64: case IR_NE_F64: case IR_LT_F64: case IR_GT_F64:
    case IR_LE_F64: case IR_GE_F64:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before cmp"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before cmp"); return false; }
        if (a != T_F64 || b != T_F64) {
            fprintf(stderr, "check: expected f64 operands for '%s', got %s %s\n",
                    ir->text, type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_BOOL);
        return true;

    // — generic comparison: T T → bool (runtime type check) —
    case IR_EQ: case IR_NE: case IR_LT: case IR_GT: case IR_LE: case IR_GE:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before cmp"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before cmp"); return false; }
        if (a != b) {
            fprintf(stderr, "check: type mismatch in '%s': %s %s\n",
                    ir->text ? ir->text : "cmp",
                    type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_BOOL);
        return true;

    // — bitwise NOT: T → i64 (integer types only) —
    case IR_BITNOT:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before '~'"); return false; }
        if (!type_is_integer(a)) {
            fprintf(stderr, "check: '~' on non-integer type %s\n", type_name(a));
            return false;
        }
        stack_push(stack, T_I64);
        return true;

    // — logical NOT: T → bool (any scalar) —
    case IR_LOGNOT:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before '!'"); return false; }
        if (!is_numeric(a)) {
            fprintf(stderr, "check: '!' on non-scalar type %s\n", type_name(a));
            return false;
        }
        stack_push(stack, T_BOOL);
        return true;

    // — typed shifts: i64 i64 → i64 —
    case IR_SHL_I64: case IR_SHR_I64:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before shift"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before shift"); return false; }
        if (a != T_I64 || b != T_I64) {
            fprintf(stderr, "check: expected i64 operands for '%s', got %s %s\n",
                    ir->text, type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_I64);
        return true;

    // — generic shifts: T T → i64 (integer types only) —
    case IR_SHL:
    case IR_SHR:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before shift"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before shift"); return false; }
        if (!type_is_integer(a)) {
            fprintf(stderr, "check: shift on non-integer type %s\n", type_name(a));
            return false;
        }
        if (!type_is_integer(b)) {
            fprintf(stderr, "check: shift amount must be integer, got %s\n", type_name(b));
            return false;
        }
        stack_push(stack, T_I64);
        return true;

    // — typed bitwise binary: i64 i64 → i64 —
    case IR_AND_I64: case IR_OR_I64: case IR_XOR_I64:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before bitwise op"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before bitwise op"); return false; }
        if (a != T_I64 || b != T_I64) {
            fprintf(stderr, "check: expected i64 operands for '%s', got %s %s\n",
                    ir->text, type_name(a), type_name(b));
            return false;
        }
        stack_push(stack, T_I64);
        return true;

    // — generic bitwise binary: T T → i64 (integer types only) —
    case IR_AND:
    case IR_OR:
    case IR_XOR:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before bitwise op"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before bitwise op"); return false; }
        if (!type_is_integer(a)) {
            fprintf(stderr, "check: bitwise op on non-integer type %s\n", type_name(a));
            return false;
        }
        if (!type_is_integer(b)) {
            fprintf(stderr, "check: bitwise op on non-integer type %s\n", type_name(b));
            return false;
        }
        stack_push(stack, T_I64);
        return true;

    // — logical binary: T T → bool (any scalar) —
    case IR_LOGAND:
    case IR_LOGOR:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before logical op"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before logical op"); return false; }
        if (!is_numeric(a)) {
            fprintf(stderr, "check: logical op on non-scalar type %s\n", type_name(a));
            return false;
        }
        if (!is_numeric(b)) {
            fprintf(stderr, "check: logical op on non-scalar type %s\n", type_name(b));
            return false;
        }
        stack_push(stack, T_BOOL);
        return true;

    // — assignment: ptr T → —
    case IR_ASSIGN:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before '='"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before '='"); return false; }
        return true;

    // — memory: alloc/halloc —
    case IR_ALLOC:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before alloc"); return false; }
        stack_push(stack, T_MEMPTR);
        return true;
    case IR_HALLOC:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before halloc"); return false; }
        stack_push(stack, T_MEMPTR);
        return true;
    case IR_FREE:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before free"); return false; }
        return true;
    // — sizeof: T → i64 —
    case IR_SIZEOF:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before sizeof"); return false; }
        stack_push(stack, T_I64);
        return true;

    // — mread: T mem → val —
    case IR_MREAD:
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before mread"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before mread"); return false; }
        stack_push(stack, ir->ty);
        return true;

    // — write: T mem val → —
    case IR_WRITE:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before write"); return false; }
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before write"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before write"); return false; }
        return true;

    // — print / printn: T → —
    case IR_PRINT:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before print"); return false; }
        return true;
    // — println: —
    case IR_PRINTLN:
        return true;
    // — printstr: mem → —
    case IR_PRINTSTR:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before printstr"); return false; }
        return true;

    // — read: → val —
    case IR_READ:
        stack_push(stack, ir->ty);
        return true;

    // — stack ops —
    case IR_DUP:
        if (!stack_peek(stack, &a)) { ir_error(ir, "stack underflow before dup"); return false; }
        stack_push(stack, a);
        return true;
    case IR_DROP:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before drop"); return false; }
        return true;
    case IR_SWAP: {
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before swap"); return false; }
        if (!stack_pop(stack, &b)) { ir_error(ir, "stack underflow before swap"); return false; }
        stack_push(stack, a);
        stack_push(stack, b);
        return true;
    }

    // — assert: condition → —
    case IR_ASSERT:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before assert"); return false; }
        return true;

    // — calls / imports: unknown effect (handled by ir_check) —
    case IR_CALL:
    case IR_CALL_IND:
    case IR_IMPORT:
        return true;

    // — control flow —
    case IR_JMP:
        return true;
    case IR_JZ:
    case IR_JNZ:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before branch"); return false; }
        if (!is_condition(a)) {
            fprintf(stderr, "check: branch on non-condition type %s\n", type_name(a));
            return false;
        }
        return true;
    case IR_JMP_DYN:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before jump"); return false; }
        return true;
    case IR_JZ_DYN:
    case IR_JNZ_DYN:
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before branch"); return false; }
        if (!stack_pop(stack, &a)) { ir_error(ir, "stack underflow before branch"); return false; }
        return true;

    case IR_RET:
    case IR_HALT:
        return true;

    default:
        return true;
    }
}

// ---------------------------------------------------------------------------
//  Checker — label-aware stack propagation using ir_apply()
// ---------------------------------------------------------------------------

typedef struct { size_t pos; StackState stack; } CfgFrame;

bool ir_check(const IrNode *ir, size_t n, FuncSym *fn) {
    if (!fn) return true;

    // 1. Build label → IR position index.
    size_t *label_pos = malloc(fn->labels.n * sizeof(size_t));
    for (size_t i = 0; i < fn->labels.n; ++i) label_pos[i] = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) {
        if (ir[i].kind == IR_LABEL_DEF && ir[i].label_idx >= 0)
            label_pos[ir[i].label_idx] = i;
    }

    // 2. Per-label stack state (NULL = unknown).
    StackState *label_stack = calloc(fn->labels.n, sizeof(StackState));

    // 3. Worklist.
    CfgFrame *wl = NULL;
    size_t wl_n = 0, wl_cap = 0;
    #define WL_PUSH(p, s) do { \
        VEC_GROW(wl, wl_n, wl_cap, CfgFrame); \
        wl[wl_n++] = (CfgFrame){(p), stack_clone(&(s))}; \
    } while (0)

    StackState init = {0};
    WL_PUSH(0, init);

    bool ok = true;

    // 4. Process worklist.
    while (wl_n > 0) {
        CfgFrame cur = wl[--wl_n];
        size_t pos = cur.pos;
        StackState stack = cur.stack;

        while (pos < n) {
            if (ir[pos].kind == IR_DEAD) { ++pos; continue; }

            // — label definition: record or verify stack depth —
            if (ir[pos].kind == IR_LABEL_DEF) {
                int id = (int)ir[pos].label_idx;
                if (id >= 0) {
                    if (label_stack[id].v == NULL) {
                        label_stack[id] = stack_clone(&stack);
                        WL_PUSH(pos + 1, stack);
                    } else if (label_stack[id].n != stack.n) {
                        fprintf(stderr, "check %s:%zu: label '%s' reached with "
                                "depth %zu, expected %zu\n",
                                fn->name, ir[pos].line,
                                fn->labels.v[id].name,
                                stack.n, label_stack[id].n);
                        ok = false;
                        label_stack[id].n = stack.n;
                        WL_PUSH(pos + 1, stack);
                    } else {
                        // Same depth already explored. Don't re-walk.
                        break;
                    }
                }
                ++pos;
                continue;
            }

            // — apply instruction —
            if (!ir_apply(&ir[pos], &stack, fn))
                ok = false;

            // — terminal —
            if (ir[pos].kind == IR_RET || ir[pos].kind == IR_HALT) break;

            // — calls / imports: fork both possibilities —
            if (ir[pos].kind == IR_CALL || ir[pos].kind == IR_CALL_IND ||
                ir[pos].kind == IR_IMPORT) {
                ++pos;
                // Assume call consumed nothing (keep stack as-is)
                WL_PUSH(pos, stack);
                // Assume call consumed all known args (empty stack)
                StackState empty = {0};
                WL_PUSH(pos, empty);
                break;
            }

            // — static jump —
            if ((ir[pos].kind == IR_JMP || ir[pos].kind == IR_JZ ||
                 ir[pos].kind == IR_JNZ) && ir[pos].aux >= 0) {
                int tid = (int)ir[pos].aux;
                if (tid >= 0 && (size_t)tid < fn->labels.n &&
                    label_pos[tid] != SIZE_MAX) {
                    WL_PUSH(label_pos[tid], stack);
                }
                if (ir[pos].kind != IR_JMP) break; // jz/jnz fall-through ends
                break;
            }

            ++pos;
        }
        free(stack.v);
    }
    #undef WL_PUSH

    for (size_t i = 0; i < fn->labels.n; ++i)
        free(label_stack[i].v);
    free(label_stack);
    free(label_pos);
    free(wl);
    return ok;
}

// ---------------------------------------------------------------------------
//  Lower — IR → threaded Op array
// ---------------------------------------------------------------------------

void ir_lower(const IrNode *ir, size_t n, FuncSym *fn,
              const size_t *ir_map, size_t ir_map_n) {
    // 1. remap: IR index → Op index (skip IR_DEAD and IR_LABEL_DEF)
    size_t *remap = calloc(n, sizeof(size_t));
    size_t op_n = 0;
    for (size_t i = 0; i < n; ++i) {
        remap[i] = op_n;
        if (ir[i].kind != IR_DEAD && ir[i].kind != IR_LABEL_DEF)
            op_n++;
    }

    // 2. allocate Op array + token→op map
    Op *ops = calloc(op_n, sizeof(Op));
    size_t *map = malloc(ir_map_n * sizeof(size_t));
    for (size_t j = 0; j < ir_map_n; ++j) {
        size_t ir_idx = ir_map[j];
        map[j] = (ir_idx < n) ? remap[ir_idx] : op_n; // sentinel
    }

    void **dt = tcode_dispatch_table();

    // 3. emit Ops, build token→op map
    for (size_t i = 0; i < n; ++i) {
        if (ir[i].kind == IR_DEAD || ir[i].kind == IR_LABEL_DEF) continue;
        size_t oi = remap[i];
        Op *op = &ops[oi];
        memset(op, 0, sizeof *op);
        op->text = ir[i].text;
        op->line = ir[i].line;
        op->aux  = -1;
        op->aux2 = -1;

        switch (ir[i].kind) {
        case IR_CONST_I8:  op->code = dt[OP_PUSH_I8];  op->u.i = ir[i].u.i; break;
        case IR_CONST_U8:  op->code = dt[OP_PUSH_U8];  op->u.u = ir[i].u.u; break;
        case IR_CONST_I16: op->code = dt[OP_PUSH_I16]; op->u.i = ir[i].u.i; break;
        case IR_CONST_U16: op->code = dt[OP_PUSH_U16]; op->u.u = ir[i].u.u; break;
        case IR_CONST_I32: op->code = dt[OP_PUSH_I32]; op->u.i = ir[i].u.i; break;
        case IR_CONST_U32: op->code = dt[OP_PUSH_U32]; op->u.u = ir[i].u.u; break;
        case IR_CONST_I64: op->code = dt[OP_PUSH_I64]; op->u.i = ir[i].u.i; break;
        case IR_CONST_U64: op->code = dt[OP_PUSH_U64]; op->u.u = ir[i].u.u; break;
        case IR_CONST_F32: op->code = dt[OP_PUSH_F32]; op->u.d = ir[i].u.d; break;
        case IR_CONST_F64: op->code = dt[OP_PUSH_F64]; op->u.d = ir[i].u.d; break;
        case IR_CONST_STR: op->code = dt[OP_PUSH_STR]; op->u.u = ir[i].u.u; break;
        case IR_CONST_BOOL: op->code = dt[OP_PUSH_BOOL]; op->u.i = ir[i].u.i; break;
        case IR_PUSH_LABEL: op->code = dt[OP_PUSH_LABEL]; op->u.u = ir[i].u.u; break;
        case IR_LOAD:
        case IR_DECLARE:
            op->code = dt[OP_WORD_VAR];
            op->u.name = ir[i].u.name;
            op->ty = ir[i].ty;
            op->has_ty = ir[i].has_ty;
            break;
        case IR_REF:
            op->code = dt[OP_REF_NAME];
            op->u.name = ir[i].u.name;
            op->aux  = ir[i].aux;
            op->aux2 = ir[i].aux2;
            break;
        case IR_DEREF:
            op->code = dt[OP_DEREF_NAME];
            op->u.name = ir[i].u.name;
            break;
        case IR_CAST:
            op->code = dt[OP_CAST];
            op->ty = ir[i].ty;
            op->has_ty = ir[i].has_ty;
            break;
        // Typed arithmetic
        case IR_ADD_I64: op->code = dt[OP_ADD_I64]; break;
        case IR_ADD_F64: op->code = dt[OP_ADD_F64]; break;
        case IR_SUB_I64: op->code = dt[OP_SUB_I64]; break;
        case IR_SUB_F64: op->code = dt[OP_SUB_F64]; break;
        case IR_MUL_I64: op->code = dt[OP_MUL_I64]; break;
        case IR_MUL_F64: op->code = dt[OP_MUL_F64]; break;
        case IR_DIV_I64: op->code = dt[OP_DIV_I64]; break;
        case IR_DIV_F64: op->code = dt[OP_DIV_F64]; break;
        case IR_MOD_I64: op->code = dt[OP_MOD_I64]; break;
        // Generic arithmetic
        case IR_ADD: op->code = dt[OP_ADD]; break;
        case IR_SUB: op->code = dt[OP_SUB]; break;
        case IR_MUL: op->code = dt[OP_MUL]; break;
        case IR_DIV: op->code = dt[OP_DIV]; break;
        case IR_MOD: op->code = dt[OP_MOD]; break;
        // Typed comparison
        case IR_EQ_I64: op->code = dt[OP_EQ_I64]; break;
        case IR_EQ_F64: op->code = dt[OP_EQ_F64]; break;
        case IR_NE_I64: op->code = dt[OP_NE_I64]; break;
        case IR_NE_F64: op->code = dt[OP_NE_F64]; break;
        case IR_LT_I64: op->code = dt[OP_LT_I64]; break;
        case IR_LT_F64: op->code = dt[OP_LT_F64]; break;
        case IR_GT_I64: op->code = dt[OP_GT_I64]; break;
        case IR_GT_F64: op->code = dt[OP_GT_F64]; break;
        case IR_LE_I64: op->code = dt[OP_LE_I64]; break;
        case IR_LE_F64: op->code = dt[OP_LE_F64]; break;
        case IR_GE_I64: op->code = dt[OP_GE_I64]; break;
        case IR_GE_F64: op->code = dt[OP_GE_F64]; break;
        // Generic comparison
        case IR_EQ: op->code = dt[OP_EQ]; break;
        case IR_NE: op->code = dt[OP_NE]; break;
        case IR_LT: op->code = dt[OP_LT]; break;
        case IR_GT: op->code = dt[OP_GT]; break;
        case IR_LE: op->code = dt[OP_LE]; break;
        case IR_GE: op->code = dt[OP_GE]; break;
        case IR_ASSIGN: op->code = dt[OP_ASSIGN]; break;
        // Bitwise (typed i64 + generic)
        case IR_BITNOT: op->code = dt[OP_BITNOT]; break;
        case IR_LOGNOT: op->code = dt[OP_LOGNOT]; break;
        case IR_SHL_I64: op->code = dt[OP_SHL_I64]; break;
        case IR_SHR_I64: op->code = dt[OP_SHR_I64]; break;
        case IR_AND_I64: op->code = dt[OP_AND_I64]; break;
        case IR_OR_I64:  op->code = dt[OP_OR_I64];  break;
        case IR_XOR_I64: op->code = dt[OP_XOR_I64]; break;
        case IR_SHL: op->code = dt[OP_SHL]; break;
        case IR_SHR: op->code = dt[OP_SHR]; break;
        case IR_AND: op->code = dt[OP_AND]; break;
        case IR_OR:  op->code = dt[OP_OR];  break;
        case IR_XOR: op->code = dt[OP_XOR]; break;
        case IR_LOGAND: op->code = dt[OP_LOGAND]; break;
        case IR_LOGOR:  op->code = dt[OP_LOGOR];  break;
        case IR_ALLOC:  op->code = dt[OP_ALLOC];  break;
        case IR_HALLOC: op->code = dt[OP_HALLOC]; op->has_ty = true; break;
        case IR_FREE:   op->code = dt[OP_FREE];   break;
        case IR_SIZEOF: op->code = dt[OP_SIZEOF]; break;
        case IR_MREAD:
            op->code = dt[OP_MREAD];
            op->ty = ir[i].ty;
            break;
        case IR_WRITE:
            op->code = dt[OP_WRITE];
            op->ty = ir[i].ty;
            break;
        case IR_PRINT:   op->code = dt[OP_PRINT];   break;
        case IR_PRINTLN: op->code = dt[OP_PRINTLN];  break;
        case IR_PRINTSTR:op->code = dt[OP_PRINTSTR]; break;
        case IR_READ:
            op->code = dt[OP_READ];
            op->ty = ir[i].ty;
            break;
        case IR_DUP:    op->code = dt[OP_DUP];    break;
        case IR_DROP:   op->code = dt[OP_DROP];   break;
        case IR_SWAP:   op->code = dt[OP_SWAP];   break;
        case IR_ASSERT: op->code = dt[OP_ASSERT]; break;
        case IR_IMPORT: op->code = dt[OP_IMPORT]; break;
        case IR_CALL:
            op->code = dt[OP_CALL_FUNC];
            op->u.i  = ir[i].u.i;
            op->aux2 = ir[i].aux2;
            break;
        case IR_CALL_IND: op->code = dt[OP_CALL_IND]; break;
        case IR_JMP:
            op->code = dt[OP_JMP];
            op->u.u  = remap[ir[i].aux];
            break;
        case IR_JZ:
            op->code = dt[OP_JZ];
            op->u.u  = remap[ir[i].aux];
            break;
        case IR_JNZ:
            op->code = dt[OP_JNZ];
            op->u.u  = remap[ir[i].aux];
            break;
        case IR_JMP_DYN: op->code = dt[OP_JMP_DYN]; break;
        case IR_JZ_DYN:  op->code = dt[OP_JZ_DYN];  break;
        case IR_JNZ_DYN: op->code = dt[OP_JNZ_DYN]; break;
        case IR_RET:  op->code = dt[OP_RET];  break;
        case IR_HALT: op->code = dt[OP_HALT]; break;
        default: break;
        }
    }

    free(remap);

    fn->code   = ops;
    fn->code_n = op_n;
    fn->code_map = map;
    fn->map_n = ir_map_n;
    fn->compiled = true;
}

// ---------------------------------------------------------------------------
//  Cleanup
// ---------------------------------------------------------------------------

void ir_free(IrNode *ir, size_t n) {
    // All strdup'd names were transferred to fn->owned during build.
    (void)n;
    free(ir);
}
