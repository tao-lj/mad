// Function and label discovery over the token stream.
#include "vm.h"

#include "common.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

Var *vm_find_var(VarVec *vv, const char *name) {
    for (size_t i = 0; i < vv->n; ++i) {
        if (strcmp(vv->v[i].name, name) == 0) return &vv->v[i];
    }
    return NULL;
}

Var *vm_add_var(VarVec *vv, const char *name, TypeKind type, bool initialized, Value value) {
    if (vm_find_var(vv, name)) return NULL;
    VEC_GROW(vv->v, vv->n, vv->cap, Var);
    Var *v = &vv->v[vv->n++];
    v->name = xstrdup(name);
    v->type = type;
    v->initialized = initialized;
    v->value = value;
    return v;
}

FuncSym *vm_find_func(FuncVec *fv, const char *name) {
    for (size_t i = 0; i < fv->n; ++i) {
        if (strcmp(fv->v[i].name, name) == 0) return &fv->v[i];
    }
    return NULL;
}

LabelSym *vm_find_label(FuncSym *fn, const char *name) {
    for (size_t i = 0; i < fn->labels.n; ++i) {
        if (strcmp(fn->labels.v[i].name, name) == 0) return &fn->labels.v[i];
    }
    return NULL;
}

void vm_collect_labels(VM *vm, FuncSym *fn) {
    for (size_t i = fn->body_start; i < fn->body_end; ++i) {
        Token *t = &vm->toks.v[i];
        if (t->kind != TOK_WORD) continue;
        const char *s = t->text;
        size_t len = strlen(s);
        if (len <= 1 || s[len - 1] != ':') continue;

        char *name = xstrdup(s);
        name[len - 1] = '\0';
        if (!is_var_token(name)) {
            free(name);
            continue;
        }
        if (vm_find_label(fn, name)) {
            fatal_at(t->line, "duplicate label '%s' in function '%s'", name, fn->name);
        }
        VEC_GROW(fn->labels.v, fn->labels.n, fn->labels.cap, LabelSym);
        fn->labels.v[fn->labels.n].name = name;
        fn->labels.v[fn->labels.n].token_index = i + 1;
        fn->labels.n++;
    }
}

// Splits a TOK_GLOBAL_REF payload such as "a, b c" into individual names.
static void split_global_names(const char *s, char ***names, size_t *n) {
    char *tmp = xstrdup(s);
    char *p = tmp;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',') ++p;
        if (!*p) break;
        char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',') ++p;
        char save = *p;
        *p = '\0';
        *names = realloc(*names, (*n + 1) * sizeof(char *));
        if (!*names) die_oom();
        (*names)[(*n)++] = xstrdup(start);
        *p = save;
    }
    free(tmp);
}

static void parse_params(Token *toks, size_t toks_n, size_t *ip,
                         char ***params, TypeKind **ptypes, size_t *pn) {
    // [] expansion leaves TOK_PARAM_END right after the reversed parameter
    // names; names are only parameters when that marker appears here.
    size_t i = *ip;
    size_t j = i;
    while (j < toks_n && toks[j].kind == TOK_WORD) ++j;
    if (j >= toks_n || toks[j].kind != TOK_PARAM_END) return;

    while (i < j) {
        const char *s = toks[i].text;
        char base[MAX_NAME];
        bool has_ty = false;
        TypeKind ty = T_I64;
        split_annotated_name(s, base, sizeof(base), &has_ty, &ty);
        if (!is_var_token(base)) fatal_at(toks[i].line, "bad parameter '%s'", s);

        *params = realloc(*params, (*pn + 1) * sizeof(char *));
        *ptypes = realloc(*ptypes, (*pn + 1) * sizeof(TypeKind));
        if (!*params || !*ptypes) die_oom();
        (*params)[*pn] = xstrdup(base);
        (*ptypes)[*pn] = has_ty ? ty : T_I64;
        ++*pn;
        ++i;
    }
    ++i; // consume TOK_PARAM_END
    *ip = i;
}

// Finds the terminating ';' of the current function definition.
static size_t find_body_end(VM *vm, size_t start, size_t line, const char *fname) {
    // Labels are words ending in ':', so TOK_SEMI is the only terminator.
    size_t end = start;
    while (end < vm->toks.n && vm->toks.v[end].kind != TOK_SEMI) ++end;
    if (end >= vm->toks.n) fatal_at(line, "unterminated function '%s'", fname);
    return end;
}

void vm_discover_functions(VM *vm) {
    size_t i = 0;
    while (i < vm->toks.n) {
        Token *t = &vm->toks.v[i];
        if (t->kind != TOK_COLON) { ++i; continue; }

        if (i + 1 >= vm->toks.n || vm->toks.v[i + 1].kind == TOK_SEMI) {
            fatal_at(t->line, "function name expected after ':'");
        }
        ++i;
        if (vm->toks.v[i].kind == TOK_WORD && vm->toks.v[i].text[0] == '{') {
            fatal_at(vm->toks.v[i].line,
                     "global list must be attached directly to ':' (use :{a b}name)");
        }

        char **globals = NULL;
        size_t global_n = 0;
        if (vm->toks.v[i].kind == TOK_GLOBAL_REF) {
            split_global_names(vm->toks.v[i].text, &globals, &global_n);
            ++i;
            if (i >= vm->toks.n || vm->toks.v[i].kind != TOK_WORD) {
                fatal_at(t->line, "function name expected after global list");
            }
        }

        const char *fname = vm->toks.v[i].text;
        ++i;
        if (vm_find_func(&vm->funcs, fname)) {
            fatal_at(t->line, "duplicate function '%s'", fname);
        }

        char **params = NULL;
        TypeKind *ptypes = NULL;
        size_t pn = 0;
        parse_params(vm->toks.v, vm->toks.n, &i, &params, &ptypes, &pn);

        size_t body_start = i;
        size_t body_end = find_body_end(vm, body_start, t->line, fname);

        VEC_GROW(vm->funcs.v, vm->funcs.n, vm->funcs.cap, FuncSym);
        FuncSym *fn = &vm->funcs.v[vm->funcs.n++];
        *fn = (FuncSym){0};
        fn->name = xstrdup(fname);
        fn->body_start = body_start;
        fn->body_end = body_end;
        fn->params = params;
        fn->param_types = ptypes;
        fn->param_count = pn;
        fn->globals = globals;
        fn->global_count = global_n;
        vm_collect_labels(vm, fn);

        i = body_end + 1;
    }
}
