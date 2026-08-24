# MAD — MAD Ain't Disciplined

MAD is a deliberately small postfix / data-stack language prototype.
The MVP is a C17 interpreter and **does not build an AST**.

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
  lexer.*     tokenizer ([] group expansion, :{...} global capture)
  value.*     runtime values, typed pools, memory objects, pointers, data stack
  vm.h        VM state and symbol-table structures
  symtab.c    function/label discovery over the token stream
  exec.c      interpreter core: frames, opcode dispatch, variable resolution
  main.c      CLI entry point
examples/     runnable .mad programs
tests/        input/expected pairs used by `make test`
```

## Core syntax

### Functions

```text
:foo
    ...
;
```

A function definition is registered but its body is not executed while top-level code is running. `;` performs an implicit `ret`.

Global dependencies must be attached directly to `:`:

```text
:{counter limit}check [x@i64]
    ...
;
```

There may be no whitespace between `:`, `{`, and the global list. `{}` is compile-time metadata, not a runtime operation.

### Parameters

```text
:foo [a@i64 b@i64]
    ...
;
```

`[]` reverses its contents and is not nestable. It is intended to make stack-order parameter binding readable.

### Parentheses

`(` and `)` are whitespace-like grouping markers. They have no semantic effect.

Thus these are equivalent:

```text
((a b +) c *)
a b + c *
(a b +) c *
```

### Variables

```text
3 a@i64       // declare a and consume 3
&a            // reference a
 a            // load a
3 &a =        // assign a = 3
```

An unknown valid variable name consumes the stack top and creates a local/global variable depending on the current environment. A type annotation fixes the variable type permanently.

A local variable may not have the same name as a global variable. A function may only access globals explicitly listed in `:{...}`.

### Pointers

`&name` produces a `ptr`. `*name` dereferences a pointer variable and loads the referenced value.

Pointers are runtime references rather than raw machine addresses. They can refer to locals or globals; dereferencing a local after its frame has returned is rejected as a dangling pointer.

### Memory

```text
100 alloc       // local-lifetime mem
100 halloc      // heap-lifetime mem
free
```

Scalar memory access:

```text
m offset mread@i64
value m offset write@i64
```

`memptr` points to a `mem` object. String literals are read-only, NUL-terminated `memptr` values.

### Type conversion

```text
3 !@f64
```

The cast consumes the old value and pushes the converted value. Assignment remains strictly typed.

### Control flow

```text
loop:
    ...
    condition loop jnz    // jump when condition is non-zero/true
```

Conditional jumps follow the assembly convention: `jz` pops a condition and jumps **when it is zero/false**, `jnz` jumps **when it is non-zero/true**. `jmp` is unconditional. A label is a first-class `label` value when referenced with `&label`.

Example:

```text
:countdown [n@i64]
    n 0 == done jnz
    n print println
    n 1 - countdown
    ret
done:
;
```

### Functions as values

```text
&foo
```

produces a `func`. It can later be called with:

```text
call
```

## Standard input

Typed input is a normal stack operation:

```text
read@i64
read@u64
read@f64
read@char
read@bool
```

The value is pushed onto the data stack. `bool` accepts `true`, `false`, `1`, or `0`.

## Output

```text
print       // print value, no newline
println     // print newline
printn      // compatibility alias for print
printstr    // print NUL-terminated mem/memptr
```

## Runtime model

A data-stack value is represented as:

```text
(type, idx)
```

`idx` indexes a type-specific runtime pool. The stack therefore does not need a single fixed machine width.

The current MVP implements these runtime types:

```text
i64 u64 f64 bool char
mem memptr ptr label func
```

The language design additionally reserves the smaller integer widths and `f32`; they are planned for the next type-system expansion rather than silently pretending that `i64` has all widths.

## Lifetime

`alloc` creates frame-lifetime memory and is released when the current function returns. `halloc` creates heap-lifetime memory and remains until `free` or program shutdown.

## Tests

`make test` runs the programs under `examples/` against fixed input/expected
pairs in `tests/`. The examples cover:

- recursion / GCD
- assignment and type errors
- pointers
- indirect function calls
- memory access
- typed standard input
- P1038-style graph processing
- frame growth beyond the initial frame-vector capacity

## Design philosophy

MAD intentionally avoids an AST in the MVP. The interpreter works with tokens, symbol tables, function slices, runtime pools, frames, and a data stack. A future bytecode VM can retain the same source semantics while compiling the token stream into compact opcodes.
