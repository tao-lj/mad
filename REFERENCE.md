# MAD Reference

Complete tables of types, literals, and operators. See [`MANUAL.md`](MANUAL.md) for usage.

## Scalar types

| Type | Width (bytes) | Signed | Range / Notes |
|------|---------------|--------|---------------|
| `i8` | 1 | yes | -128..127 |
| `u8` | 1 | no | 0..255 |
| `i16` | 2 | yes | -32768..32767 |
| `u16` | 2 | no | 0..65535 |
| `i32` | 4 | yes | -2^31..2^31-1 |
| `u32` | 4 | no | 0..2^32-1 |
| `i64` | 8 | yes | -2^63..2^63-1 (default integer) |
| `u64` | 8 | no | 0..2^64-1 |
| `f32` | 4 | — | IEEE 754 single |
| `f64` | 8 | — | IEEE 754 double (default float) |
| `bool` | 1 | — | false / true |
| `char` | 1 | — | single byte (alias for i8 in I/O) |

## Handle types

| Type | Meaning | Created by |
|------|---------|------------|
| `mem` | byte buffer handle | `alloc` / `halloc` |
| `memptr` | pointer to mem | string literal, `&mem_variable` |
| `ptr` | variable pointer | `&variable` |
| `label` | first-class label | `&label` |
| `func` | first-class function | `&function_name` |
| `file` | open file handle | `fopen` |

## Literals

| Syntax | Type | Example |
|--------|------|---------|
| `123` / `-3` | i64 | `123`, `-7` |
| `u:42` | u64 | `u:0xFF` |
| `3.14` | f64 | `-1.0`, `2e10` |
| `"text"` | memptr | `"hello\n"` |
| `'c'` | char (i8) | `'A'`, `'\n'` |
| `i8:N` | i8 | `i8:10` |
| `u8:N` | u8 | `u8:255` |
| `i16:N` | i16 | `i16:-1000` |
| `u16:N` | u16 | `u16:65535` |
| `i32:N` | i32 | `i32:100000` |
| `u32:N` | u32 | `u32:4000000000` |
| `u64:N` | u64 | `u64:100` |
| `f32:N` | f32 | `f32:3.14` |
| `f64:N` | f64 | `f64:3.14` |
| `char:N` | char (i8) | `char:65` |

### Escape sequences (string and char literals)

| Sequence | Meaning |
|----------|---------|
| `\n` | newline |
| `\r` | carriage return |
| `\t` | tab |
| `\0` | null byte |
| `\\` | backslash |
| `\"` | double quote |
| `\'` | single quote |

## Operators

### Arithmetic

| Op | Stack effect | Types | Result |
|----|-------------|-------|--------|
| `+` | `( a b -- r )` | numeric | same type |
| `-` | `( a b -- r )` | numeric | same type |
| `*` | `( a b -- r )` | numeric | same type |
| `/` | `( a b -- r )` | numeric | same type |
| `%` | `( a b -- r )` | integer only | i64 |

Typed opcodes: `OP_ADD_I64`, `OP_ADD_F64`, `OP_SUB_I64`, `OP_SUB_F64`,
`OP_MUL_I64`, `OP_MUL_F64`, `OP_DIV_I64`, `OP_DIV_F64`, `OP_MOD_I64`.
Generic fallback: `OP_ADD`, `OP_SUB`, `OP_MUL`, `OP_DIV`, `OP_MOD`.

### Comparison

| Op | Stack effect | Types | Result |
|----|-------------|-------|--------|
| `==` | `( a b -- flag )` | numeric | bool |
| `!=` | `( a b -- flag )` | numeric | bool |
| `<` | `( a b -- flag )` | numeric | bool |
| `>` | `( a b -- flag )` | numeric | bool |
| `<=` | `( a b -- flag )` | numeric | bool |
| `>=` | `( a b -- flag )` | numeric | bool |

Typed opcodes: `OP_EQ_I64`, `OP_EQ_F64`, `OP_NE_I64`, `OP_NE_F64`,
`OP_LT_I64`, `OP_LT_F64`, `OP_GT_I64`, `OP_GT_F64`,
`OP_LE_I64`, `OP_LE_F64`, `OP_GE_I64`, `OP_GE_F64`.
Generic fallback: `OP_EQ`, `OP_NE`, `OP_LT`, `OP_GT`, `OP_LE`, `OP_GE`.

### Bitwise

| Op | Stack effect | Types | Result |
|----|-------------|-------|--------|
| `~` | `( a -- r )` | integer | i64 |
| `<<` | `( a b -- r )` | integer | i64 |
| `>>` | `( a b -- r )` | integer | i64 |
| `&` | `( a b -- r )` | integer | i64 |
| `\|` | `( a b -- r )` | integer | i64 |
| `^` | `( a b -- r )` | integer | i64 |

Integer types: i8, u8, i16, u16, i32, u32, i64, u64, bool, char.
Typed opcodes: `OP_SHL_I64`, `OP_SHR_I64`, `OP_AND_I64`, `OP_OR_I64`, `OP_XOR_I64`.
Generic fallback: `OP_SHL`, `OP_SHR`, `OP_AND`, `OP_OR`, `OP_XOR`.

### Logical

| Op | Stack effect | Types | Result |
|----|-------------|-------|--------|
| `!` | `( a -- flag )` | any scalar | bool |
| `&&` | `( a b -- flag )` | any scalar | bool |
| `\|\|` | `( a b -- flag )` | any scalar | bool |

### Assignment and memory

| Word | Stack effect | Description |
|------|-------------|-------------|
| `=` | `( ptr value -- )` | assign through pointer |
| `alloc` | `( bytes -- mem )` | frame-lifetime allocation |
| `halloc` | `( bytes -- mem )` | heap-lifetime allocation |
| `free` | `( mem/memptr -- )` | release memory |
| `sizeof` | `( v -- i64 )` | byte size of value |
| `mread@T` | `( mem off -- v )` | read scalar from mem |
| `write@T` | `( val mem off -- )` | write scalar to mem |

### Cast

| Word | Stack effect | Description |
|------|-------------|-------------|
| `!@T` | `( old -- new )` | cast to type T |

Allowed conversions: integer↔integer (truncate), integer↔float, bool→integer,
integer/char/float→bool, integer↔char, label/func→u64, same→same (no-op).
Integer→label is forbidden.

### I/O

| Word | Stack effect | Description |
|------|-------------|-------------|
| `print` / `printn` | `( v -- )` | print value, no newline |
| `println` | `( -- )` | print newline only |
| `printstr` | `( mem/memptr -- )` | print NUL-terminated string |
| `read@T` | `( -- v )` | read typed value from stdin |

### Control flow

| Word | Stack effect | Description |
|------|-------------|-------------|
| `ret` | `( -- )` | return from function |
| `halt` | `( -- )` | terminate program |
| `assert` | `( flag -- )` | abort if false |
| `call` | `( func -- )` | indirect function call |
| `import` | `( path -- )` | import module file |
| `fopen` | `( path mode -- file )` | open file; fatal on failure |
| `fclose` | `( file -- )` | close file handle |
| `fsize` | `( file -- i64 )` | query file size without moving position |
| `fread` | `( file n -- count mem )` | read up to n bytes; count≤n |
| `fwrite` | `( buf file -- written )` | write buf contents; returns bytes written |
| `system` | `( cmd -- rc )` | execute shell command; rc is raw `waitpid` status |
| `jmp` / `jump` | `( -- )` | unconditional jump (static or dynamic) |
| `jz` | `( condition -- )` | jump if zero/false |
| `jnz` | `( condition -- )` | jump if nonzero/true |

Conditions accept bool and all integer types (i8..u64). Float and handle types
are rejected.

### Stack manipulation

| Word | Description |
|------|-------------|
| `dup` | `( a -- a a )` duplicate top |
| `drop` | `( a -- )` discard top |
| `swap` | `( a b -- b a )` exchange top two |
