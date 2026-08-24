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
//   exec.c    interpreter core
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
    lex_source(src, &vm.toks);
    free(src);

    vm_discover_functions(&vm);
    vm_run_top_level(&vm);
    vm_free(&vm);
    return 0;
}
