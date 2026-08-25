// tcode.c — thin wrapper: bridges the IR pipeline to the runtime Op array.
#include "tcode.h"
#include "ir.h"
#include "common.h"
#include "vm.h"
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
//  Public interface called by exec.c / vm_run_top_level
// ---------------------------------------------------------------------------

void tcode_compile_func(VM *vm, FuncSym *fn) {
    size_t n = 0;
    size_t *ir_map = NULL;
    IrNode *ir = ir_build(vm, fn, &n, &ir_map);
    n = ir_optimize(ir, n);
    ir_check(ir, n, fn);
    ir_lower(ir, n, fn, ir_map, fn->body_end - fn->body_start);
    free(ir_map);
    ir_free(ir, n);
}

void tcode_free_sym(FuncSym *fn) {
    free(fn->code);
    fn->code = NULL;
    fn->code_n = 0;
    free(fn->code_map);
    fn->code_map = NULL;
    fn->map_n = 0;
    for (size_t i = 0; i < fn->owned_n; ++i)
        free(fn->owned[i]);
    free(fn->owned);
    fn->owned = NULL;
    fn->owned_n = fn->owned_cap = 0;
}
