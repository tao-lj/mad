// MAD Ain't Disciplined - MVP interpreter
// Version: 0.3.0
// Prototype semantics: postfix/data-stack language, no AST.
//
// Module layout under src/:
//   common.*  shared utilities (vectors, diagnostics, file IO)
//   lexer.*   tokenizer
//   value.*   runtime values, typed pools, memory, pointers, data stack
//   vm.h      VM state and symbol tables
//   symtab.c  function/label discovery
//   tcode.*   threaded-code compiler
//   exec.c    runtime dispatch loop and builtins
#include "common.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s file.mad\n", argv[0]);
        return 2;
    }
    char *src = read_file(argv[1]);

    VM vm = {0};
    vm.file_dir = path_dir_of(argv[1]);
    // Register the entry file so importing it is a no-op (#pragma once).
    char *canon = path_canonical(".", argv[1]);
    if (!canon) {
        fprintf(stderr, "MAD: path too deep: %s\n", argv[1]);
        return 1;
    }
    VEC_GROW(vm.imported, vm.imported_n, vm.imported_cap, char *);
    vm.imported[vm.imported_n++] = canon;

    lex_source(src, &vm.toks);
    free(src);

    vm_discover_functions(&vm);
    vm_run_top_level(&vm);
    vm_free(&vm);
    return 0;
}
