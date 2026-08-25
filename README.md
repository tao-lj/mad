# MAD — MAD Ain't Disciplined

MAD is a deliberately small postfix / data-stack language. The implementation is a
C17 interpreter that **never builds an AST**.

Full language reference: [`MANUAL.md`](MANUAL.md) — [`中文版`](MANUAL_zh.md)

## Design philosophy

MAD achieves strong functionality with minimal implementation cost by combining
ideas from three traditions:

- **Data flow: FORTH-style stack thinking.** No variables in the traditional sense —
  values live on a data stack, words pop operands and push results,书写顺序即执行顺序.
  Parentheses are visual grouping only (`(a b +) c *` ≡ `a b + c *`).

- **Control flow: assembly-style labels and jumps.** `label condition jnz` follows
  the assembler convention — condition before jump, `jz`/`jnz` for conditional,
  `jmp` for unconditional. No while/for/if — the programmer thinks in branches.

- **Data structures: C/assembly memory management + LISP-style manual interpretation.**
  `alloc`/`halloc` give raw byte buffers with bounds checking; `mread@T`/`write@T`
  access typed slots; `sizeof` queries size. The programmer builds and interprets
  data structures explicitly — arrays, trees, graphs — the way you would in C,
  but with runtime safety nets (bounds checks, dangling-pointer detection, type checks).

The result is a language that is trivial to implement (no parser beyond a tokenizer,
no AST, no tree-walking) yet supports recursion, first-class functions, pointers,
memory management, module imports, and a typed IR with compile-time constant folding
and typed opcode specialization.

## Build

```sh
make            # build ./mad from src/
make test       # build and run the example-based regression tests
make clean      # remove build artifacts
```

`CC`, `OPTFLAGS` and `CFLAGS` can be overridden, e.g. `make CC=gcc OPTFLAGS="-O0 -g"`.

## Project layout

```text
src/          interpreter sources
  common.*    shared utilities: grow-only vectors, diagnostics, file IO
  lexer.*     tokenizer ([] group expansion, :{...} global capture, typed literals)
  value.*     runtime values (inline scalars), memory objects, pointers, data stack
  vm.h        VM state and symbol-table structures
  symtab.c    function/label discovery over the token stream
  ir.*        intermediate representation: build, check, optimize, lower
  tcode.*     threaded-code compiler (token classification, op emission, jump fusion)
  exec.c      runtime: frames, variable resolution, dispatch loop (labels-as-values), builtins
  main.c      CLI entry point
examples/     one directory per program: main.mad + input + expected,
              all run by `make test`
tests/        checker unit tests (tests/checker/)
```

## Quick example

```mad
:fact [n@i64]
    (n 2 <) base jnz
    (n 1 -) fact
    n *
    ret
base:
    1
;

5 fact println       // prints 120
```

## Examples

Each `examples/<name>/` directory holds one self-contained program:
`main.mad` (the entry, importing siblings by relative path), an optional
`input` fed to stdin, and an `expected` reference output. See
[`examples/README.md`](examples/README.md) for details:

- `branch/` — conditional jumps (`jz` / `jnz`) in both polarities
- `import/` — nested module imports, including late-bound function references
- `nqueens/` — N-Queens solver: recursion, `mem` arrays, label-based control flow
- `p1038/` — graph processing: flat i64 arrays in `mem`, worklist traversal

## Tests

`make test` runs every `examples/*/main.mad` — feeding its `input` when one
exists — and diffs stdout against its `expected`.

## License

Public domain. Use however you like.
