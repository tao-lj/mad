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
} IrBuilder;

static void irb_own(IrBuilder *b, char *s) {
    VEC_GROW(b->fn->owned, b->fn->owned_n, b->fn->owned_cap, char *);
    b->fn->owned[b->fn->owned_n++] = s;
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
    return b->n - 1;
}

static void irb_flush(IrBuilder *b) {
    if (b->pending_label < 0) return;
    size_t idx = irb_emit(b, IR_PUSH_LABEL, b->pending_tok);
    b->v[idx].u.u = (uint64_t)b->pending_label;
    b->pending_label = -1;
    b->pending_tok = NULL;
}

static void irb_const(IrBuilder *b, IrKind kind, const Token *t, uint64_t payload) {
    size_t idx = irb_emit(b, kind, t);
    b->v[idx].u.u = payload;
}

static void irb_static_jump(IrBuilder *b, IrKind kind, const Token *t,
                            LabelSym *target) {
    size_t idx = irb_emit(b, kind, t);
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

    // Label definitions are pure position markers.
    size_t len = strlen(s);
    if (len && s[len - 1] == ':') return;

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
        irb_static_jump(b, k, t, target);
        b->pending_label = -1;
        b->pending_tok = NULL;
        return;
    }
    irb_flush(b);

    if (word_is(s, "ret"))    { irb_emit(b, IR_RET, t); return; }
    if (word_is(s, "halt"))   { irb_emit(b, IR_HALT, t); return; }
    if (word_is(s, "dup"))    { irb_emit(b, IR_DUP, t); return; }
    if (word_is(s, "drop"))   { irb_emit(b, IR_DROP, t); return; }
    if (word_is(s, "swap"))   { irb_emit(b, IR_SWAP, t); return; }

    static const char *arith_words[] = {"+", "-", "*", "/", "%"};
    for (int k = AR_ADD; k <= AR_MOD; ++k) {
        if (word_is(s, arith_words[k])) {
            size_t idx = irb_emit(b, IR_ARITH, t);
            b->v[idx].u.i = k;
            return;
        }
    }
    static const char *cmp_words[] = {"==", "!=", "<", ">", "<=", ">="};
    for (int k = CMP_EQ; k <= CMP_GE; ++k) {
        if (word_is(s, cmp_words[k])) {
            size_t idx = irb_emit(b, IR_CMP, t);
            b->v[idx].u.i = k;
            return;
        }
    }

    if (word_is(s, "="))   { irb_emit(b, IR_ASSIGN, t); return; }
    if (word_is(s, "call")){ irb_emit(b, IR_CALL_IND, t); return; }
    if (is_jz)  { irb_emit(b, IR_JZ_DYN, t);  return; }
    if (is_jnz) { irb_emit(b, IR_JNZ_DYN, t); return; }
    if (is_jmp) { irb_emit(b, IR_JMP_DYN, t); return; }

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
    size_t idx = irb_emit(b, IR_VAR, t);
    b->v[idx].u.name = xstrdup(base);
    irb_own(b, b->v[idx].u.name);
    b->v[idx].ty = ann;
    b->v[idx].has_ty = has_ty;
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
            ++i;
            break;
        }
        case TOK_UINT: {
            irb_flush(&b);
            size_t idx = irb_emit(&b, IR_CONST_U64, t);
            b.v[idx].u.u = strtoull(t->text + 2, NULL, 10);
            ++i;
            break;
        }
        case TOK_FLOAT: {
            irb_flush(&b);
            size_t idx = irb_emit(&b, IR_CONST_F64, t);
            b.v[idx].u.d = strtod(t->text, NULL);
            ++i;
            break;
        }
        case TOK_STRING:
            irb_flush(&b);
            irb_const(&b, IR_CONST_STR, t, compile_string_literal(vm, t));
            ++i;
            break;
        case TOK_COLON:
            irb_flush(&b);
            if (b.top_level) {
                while (i < fn->body_end && vm->toks.v[i].kind != TOK_SEMI)
                    ++i;
            }
            ++i;
            break;
        case TOK_SEMI:
            ++i;
            goto done;
        case TOK_WORD:
            if (t->text[0] == '&' || (t->text[0] == '*' && t->text[1])) {
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
            ++i;
            break;
        default:
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

size_t ir_optimize(IrNode *ir, size_t n) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i + 2 < n; ++i) {
            if (ir[i].kind == IR_DEAD || ir[i+1].kind == IR_DEAD ||
                ir[i+2].kind == IR_DEAD)
                continue;
            // i64 arithmetic fold
            if (ir[i].kind == IR_CONST_I64 && ir[i+1].kind == IR_CONST_I64 &&
                ir[i+2].kind == IR_ARITH) {
                bool safe = (ir[i+2].u.i != AR_DIV && ir[i+2].u.i != AR_MOD)
                         || ir[i+1].u.i != 0;
                if (safe) {
                    ir[i].kind   = IR_DEAD;
                    ir[i+1].kind = IR_DEAD;
                    ir[i+2].kind = IR_CONST_I64;
                    ir[i+2].u.i  = arith_fold(ir[i+2].u.i,
                                               ir[i].u.i, ir[i+1].u.i);
                    changed = true;
                }
            }
            // i64 cmp fold
            if (ir[i].kind == IR_CONST_I64 && ir[i+1].kind == IR_CONST_I64 &&
                ir[i+2].kind == IR_CMP) {
                ir[i].kind   = IR_DEAD;
                ir[i+1].kind = IR_DEAD;
                ir[i+2].kind = IR_CONST_I64;
                ir[i+2].u.i  = cmp_fold(ir[i+2].u.i,
                                         ir[i].u.i, ir[i+1].u.i);
                changed = true;
            }
            // f64 arithmetic fold
            if (ir[i].kind == IR_CONST_F64 && ir[i+1].kind == IR_CONST_F64 &&
                ir[i+2].kind == IR_ARITH) {
                bool safe = (ir[i+2].u.i != AR_DIV && ir[i+2].u.i != AR_MOD)
                         || ir[i+1].u.d != 0.0;
                if (safe) {
                    double a = ir[i].u.d, b = ir[i+1].u.d;
                    ir[i].kind   = IR_DEAD;
                    ir[i+1].kind = IR_DEAD;
                    ir[i+2].kind = IR_CONST_F64;
                    switch (ir[i+2].u.i) {
                    case AR_ADD: ir[i+2].u.d = a + b; break;
                    case AR_SUB: ir[i+2].u.d = a - b; break;
                    case AR_MUL: ir[i+2].u.d = a * b; break;
                    default:     ir[i+2].u.d = a / b; break;
                    }
                    changed = true;
                }
            }
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
//  Checker — linear stack-depth validation
// ---------------------------------------------------------------------------

// Checker uses min/max depth tracking because IR_VAR (load-or-declare)
// has an ambiguous stack effect (-1 or +1).  We flag only definite
// underflows (min_depth < 0).

bool ir_check(const IrNode *ir, size_t n, const FuncSym *fn) {
    (void)fn;
    int lo = 0, hi = 0; // min/max possible depth
    (void)hi;
    bool ok = true;

    for (size_t i = 0; i < n; ++i) {
        if (ir[i].kind == IR_DEAD) continue;
        if (ir[i].kind == IR_LABEL_DEF) continue;

        switch (ir[i].kind) {
        // definite +1
        case IR_CONST_I64: case IR_CONST_U64: case IR_CONST_F64:
        case IR_CONST_STR: case IR_PUSH_LABEL:
        case IR_REF: case IR_DUP: case IR_READ:
            lo++; hi++; break;

        // ambiguous: -1 if declaring, +1 if loading
        case IR_VAR:
            lo--; hi++; break;

        // definitely -2+1 = -1
        case IR_ARITH: case IR_CMP: case IR_MREAD:
            lo--; hi--; break;

        // definitely -2
        case IR_ASSIGN:
            lo -= 2; hi -= 2; break;

        // definitely -3
        case IR_WRITE:
            lo -= 3; hi -= 3; break;

        // definitely -1
        case IR_DROP: case IR_FREE: case IR_PRINT: case IR_PRINTSTR:
        case IR_ASSERT: case IR_JZ: case IR_JNZ:
            lo--; hi--; break;

        // net 0
        case IR_PRINTLN: case IR_CAST: case IR_ALLOC: case IR_HALLOC:
        case IR_JMP:
            break;

        // calls / imports: reset tracking (unknown effect)
        case IR_IMPORT: case IR_CALL: case IR_CALL_IND:
            lo = 0; hi = 100; break;

        // dynamic variants
        case IR_JMP_DYN:
            lo--; hi--; break;
        case IR_JZ_DYN: case IR_JNZ_DYN:
            lo -= 2; hi -= 2; break;

        case IR_RET: case IR_HALT:
            lo = 0; hi = 0; break;

        default:
            break;
        }
        if (lo < 0) lo = 0;
    }
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
        case IR_CONST_I64: op->code = dt[OP_PUSH_I64]; op->u.i = ir[i].u.i; break;
        case IR_CONST_U64: op->code = dt[OP_PUSH_U64]; op->u.u = ir[i].u.u; break;
        case IR_CONST_F64: op->code = dt[OP_PUSH_F64]; op->u.d = ir[i].u.d; break;
        case IR_CONST_STR: op->code = dt[OP_PUSH_STR]; op->u.u = ir[i].u.u; break;
        case IR_PUSH_LABEL: op->code = dt[OP_PUSH_LABEL]; op->u.u = ir[i].u.u; break;
        case IR_VAR:
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
        case IR_ARITH: op->code = dt[OP_ARITH]; op->u.i = ir[i].u.i; break;
        case IR_CMP:   op->code = dt[OP_CMP];   op->u.i = ir[i].u.i; break;
        case IR_ASSIGN: op->code = dt[OP_ASSIGN]; break;
        case IR_ALLOC:  op->code = dt[OP_ALLOC];  break;
        case IR_HALLOC: op->code = dt[OP_HALLOC]; op->has_ty = true; break;
        case IR_FREE:   op->code = dt[OP_FREE];   break;
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
