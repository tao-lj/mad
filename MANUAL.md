# MAD Language Manual

MAD (**MAD Ain't Disciplined**) is a postfix data-stack language.
Literals push onto the stack; words pop operands and push results.
Writing order is execution order.

Stack effect notation: `( pop order -- push order )`.
For example `+` is `( a b -- sum )` (b is popped first).

Full type / literal / operator tables: [`REFERENCE.md`](REFERENCE.md)

## 1. Lexical structure

- Comments: `//` to end of line; whitespace separates tokens only.
- `( )` are discarded by the lexer — pure visual grouping:
  `(a b +) c *` ≡ `a b + c *`.
- `[ ]` collect their tokens, **reverse** them, and push back (not nestable).
  Used for parameter lists and batch declarations.
- Variable names start with a letter or `_`, followed by letters, digits, `_`, `@`.
- Prefix notation as separate tokens:

  | Form | Meaning |
  |------|---------|
  | `&name` | reference: variable→ptr, label→label, function→func |
  | `*name` | dereference pointer variable |
  | `!@T` | type cast |
  | `name:` | label definition |
  | `u:N` | unsigned literal |
  | `T:N` | sized literal (e.g. `i8:10`, `f32:3.14`, `char:65`) |

- Literals: `123` / `-3` → i64; `3.14` → f64; `u:42` → u64; `"text"` → memptr
  (escapes: `\n \r \t \0 \\ \"`). `'c'` → char literal (escapes: `\n \0 \\ \'`),
  stored as i8. No boolean literal. Max token length: 255 bytes.
  Note: `-3` is a single token; for negation write `(0 x -)`.
- Definitions: `:name ... ;` registers a function (body runs on call, not at definition);
  `:{globals}name ... ;` captures listed globals; `;` at top level stops execution.
- Type annotation: any variable name can carry `@type` suffix (e.g. `x@i64`).

## 2. Types

Common scalar types (see [`REFERENCE.md`](REFERENCE.md) for full list):

| Type | Width | Example |
|------|-------|---------|
| `i64` | 8 | `123` (default for integers) |
| `u64` | 8 | `u:42` |
| `f64` | 8 | `3.14` (default for floats) |
| `bool` | 1 | result of comparisons, `!`, `&&`, `\|\|` |
| `char` | 1 | `'c'` |

Handle types: `mem`, `memptr`, `ptr`, `label`, `func`, `file`.

All scalar values are stored inline in the `Value` union — no pool indirection.
Handles store a uint64 id in the same union.

### Arithmetic and comparison rules

- Same-type: `i64 + i64 → i64`, `f64 * f64 → f64`.
- Mixed numeric: smaller integer promotes to i64; any f64 promotes both to f64.
- `%` is integer-only (no f32/f64).
- Bitwise ops (`~ << >> & | ^`) accept integer types only (i8..u64, bool, char), result i64.
- Logical ops (`! && ||`) accept any scalar, result bool.
- Comparisons require numeric types, result bool.
- **No implicit assignment conversion** — exact type match required.

## 3. Variables and scope

**Declaration consumes the stack top:**

```mad
3 x@i64          // declare x as i64 with value 3
x                // load x
3 &x =           // assign x = 3
```

An unknown valid variable name pops the stack top as initial value and declares it.
An existing name loads the value. Double declaration is an error.

- Top-level declarations are global; inside a function they are local (frame-lifetime).
- Locals and parameters must not shadow globals; parameters must have unique names.
- Functions may only access globals listed in their `:{...}` clause.

```mad
1 2 3 [a b c]    // batch declaration: a=1 b=2 c=3
```

## 4. Built-in words

User-defined functions/labels shadow built-in words of the same name.

| Word | Stack effect | Description |
|------|-------------|-------------|
| `dup` / `drop` / `swap` | stack ops | duplicate / discard / exchange top |
| `+ - * / %` | `( a b -- r )` | arithmetic; divide-by-zero is an error |
| `== != < > <= >=` | `( a b -- flag )` | comparison, result bool |
| `~` | `( a -- r )` | bitwise NOT (integer only, → i64) |
| `!` | `( a -- flag )` | logical NOT (any scalar, → bool) |
| `<<` / `>>` | `( a b -- r )` | shift (integer only, → i64) |
| `&` `\|` `^` | `( a b -- r )` | bitwise AND/OR/XOR (integer only, → i64) |
| `&&` `\|\|` | `( a b -- flag )` | logical AND/OR (any scalar, → bool) |
| `=` | `( ptr value -- )` | assign through pointer |
| `alloc` / `halloc` | `( bytes -- mem )` | frame / heap lifetime memory |
| `free` | `( mem/memptr -- )` | release memory |
| `sizeof` | `( v -- i64 )` | scalar: type byte size; mem/memptr: mem length |
| `mread@T` | `( mem off -- v )` | read scalar from mem |
| `write@T` | `( val mem off -- )` | write scalar to mem |
| `print` / `printn` | `( v -- )` | print value without newline |
| `println` | `( -- )` | print newline only |
| `printstr` | `( mem/memptr -- )` | print NUL-terminated string |
| `read@T` | `( -- v )` | read typed value from stdin |
| `fopen` | `( path mode -- file )` | open file; fatal on failure |
| `fclose` | `( file -- )` | close file handle |
| `fsize` | `( file -- i64 )` | query file size without moving position |
| `fread` | `( file n -- count mem )` | read up to n bytes; count≤n |
| `fwrite` | `( buf file -- written )` | write buf contents; returns bytes written |
| `system` | `( cmd -- rc )` | execute shell command; rc is raw `waitpid` status |
| `call` | `( func -- )` | indirect function call |
| `ret` / `halt` | `( -- )` | return / terminate program |
| `assert` | `( flag -- )` | abort if false |
| `import` | `( path -- )` | import module file |

`T` ∈ {i8, u8, i16, u16, i32, u32, i64, u64, f32, f64, bool, char}.

## 5. Control flow and functions

**Labels and jumps:**

```mad
loop:
    ...
    (i 0 >) loop jnz    // condition before target; jz=jump-if-zero, jnz=jump-if-nonzero
```

`jmp`/`jump` is unconditional. `&label` produces a first-class label value.
Bare label names adjacent to jump words are **fused** at compile time into
direct branches; stored labels use dynamic jumps (target popped from stack).

**Functions:**

```mad
:fact [n@i64]
    (n 2 <) base jnz
    (n 1 -) fact
    n *
    ret
base:
    1
;
```

Parameters are bound via `[]` reversal in writing order, passed by value with
exact type matching. `;` at the end of a function body is an implicit `ret`.

`&foo` produces a `func` value for indirect calls via `call`.

## 6. Pointers and memory

`&variable` produces a `ptr` (or `memptr` if the variable holds a mem).
`=` assigns through a pointer. `*p` dereferences a pointer variable.

```mad
3 x@i64   &x p@ptr   5 &x =   *p print println    // prints 5
```

Pointers track their target frame; dereferencing after the frame returns
is rejected as a dangling pointer.

**Memory:**

| Creation | Lifetime |
|----------|----------|
| `alloc` | frame-lifetime; freed on function return |
| `halloc` | heap-lifetime; freed by `free` or program exit |
| string literal | read-only, program-lifetime |

Scalar access: `m offset mread@T` / `value m offset write@T`.
Array convention: offset by element size — `(a (i ELEM_SIZE *) mread@T)` reads `a[i]`.

## 7. Type conversion

`!@T` pops old value, pushes converted value. Allowed conversions:

| Source → Target | Notes |
|----------------|-------|
| integer ↔ integer | truncate to target width |
| integer ↔ f32/f64 | truncate or convert |
| bool → integer/f32/f64 | false→0, true→1 |
| integer/char/f32/f64 → bool | non-zero → true |
| integer ↔ char | truncate to single byte |
| label/func → u64 | extract internal id |
| same type | no-op |

Integer → label is intentionally forbidden (prevents forged jump targets).

## 8. Module imports

```mad
"lib.mad" import    // consumes a memptr path from the stack
```

Pops a memptr path, reads and executes another MAD file: its function definitions
are registered, and its top-level code runs, making globals and functions visible
to the importer.

- Relative paths resolve against the **importing file's directory**, not the process CWD.
- Only top-level code may `import`.
- Each file is imported at most once (`#pragma once` semantics, always on).
- Nesting depth is capped at 64.

## 9. I/O and errors

stdin is parsed by `read@T`: integers decimal, floats by format, char reads one
byte skipping whitespace, bool accepts `true`/`false`/`1`/`0`. Failure is an error.
stdout adds no automatic separators — use `" " printstr` if needed.
`print` renders handles as `<type:id>`.

**File I/O:**

`fopen` takes two mem strings (path and mode, e.g. `"r"`, `"w"`) and returns a
`file` handle. All file operations fatal on error (open failure, closed handle,
invalid id). `fread` allocates a frame-lifetime buffer and reads up to `n` bytes;
`fwrite` writes the entire contents of a mem/memptr buffer.

`fsize` queries the file size via `fseek`/`ftell` without moving the current
position. File handles are automatically closed at program exit.

```mad
"/tmp/out.txt" "w" fopen f@file
"hello" f fwrite drop       // drop byte-count result
f fclose

"/tmp/out.txt" "r" fopen g@file
g fsize n@i64
g n fread data@mem drop     // drop byte-count result
data printstr println
g fclose
```

All errors print `line: message` and exit immediately. No exceptions, no recovery.

## 10. Implementation

**Two-pass, no AST:** Scan once to register function slices (name, params,
captures, token range) and label positions. Compile remaining top-level as an
anonymous `<top>` function. `import` appends tokens at runtime and re-scans.

**Threaded code:** Each function body is lazily compiled to an `Op` array on
first execution. Instructions use GCC labels-as-values (`&&label`); dispatch
is a single computed goto (`goto *(++ip)->code`). Literals, names, call targets,
and jump destinations are resolved once at compile time. `label jz/jnz/jmp`
sequences are fused into direct branches.

**Compile-time type stack:** The builder tracks operand types during IR construction.
When both operands of an arithmetic/comparison/bitwise op have known compatible
types, it emits **typed IR** (e.g. `IR_ADD_I64`, `IR_EQ_F64`), which lowers to
**typed opcodes** (`OP_ADD_I64`, `OP_EQ_F64`) — zero-overhead fast path with no
runtime type dispatch. Unknown types fall through to **generic opcodes** (`OP_ADD`,
`OP_EQ`) which do runtime dispatch. This gives hot-path performance for common
types (i64, f64) while keeping the opcode space compact.

**IR pipeline:** `ir_build` → `ir_optimize` (constant folding) → `ir_check`
(worklist type checking via `ir_apply`) → `ir_lower` (IR → threaded Op array).
