// Threaded-code compiler: lazily translates one function body (or the top
// level) into an array of Op records.  Nothing here touches the runtime
// stack; the dispatch loop and all value helpers live in exec.c.

#include "tcode.h"

#include "common.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

// ---------- Helpers shared only with the compiler ----------

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

// ---------- Compiler data structures ----------

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
    if (word_is(s, "import")) { emit_op(c, OP_IMPORT, t); return; }

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
void tcode_compile_func(VM *vm, FuncSym *fn) {
    Compiler c = {0};
    c.vm = vm;
    c.fn = fn;
    c.top_level = fname_is_module(fn->name);
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
    free(c.fix); // fixups are consumed above; only c.v/c.map transfer to fn

    fn->code = c.v;
    fn->code_n = c.n;
    fn->code_map = c.map;
    fn->map_n = c.map_n;
    fn->compiled = true;
}

// Releases compiled code owned by a FuncSym (used for the synthetic <top>).
void tcode_free_sym(FuncSym *fn) {
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
