# Examples

Each subdirectory is one self-contained example:

```text
examples/<name>/main.mad     entry program (imports siblings by relative path)
examples/<name>/input        optional stdin fed to the program
examples/<name>/expected     reference output; make test diffs against it
```

Run any example directly:

```sh
make                                        # build ./mad at the repo root
./mad examples/nqueens/main.mad < examples/nqueens/input
```

`make test` runs every `examples/*/main.mad`, feeding `input` when present,
and diffs stdout against `expected`.

## branch

Conditional jumps in both polarities: `jz` branches on zero/false, `jnz`
on non-zero/true. Four cases cover taken and not-taken for each.

## import

Module imports with relative-path resolution against the importing file's
directory: `main.mad` → `lib.mad` → `helper.mad` defines a global constant;
`late.mad` is imported after code that already references it, exercising the
runtime fallback that resolves such names to function calls.

## nqueens

N-Queens solver. Reads `N` from stdin (`4` in `input`), prints every solution
as an `N x N` board (`Q` marks a queen), then the solution count.

Features exercised: recursion; a `mem` block as an array (queen column for
row `r` at byte offset `r * 8`); `mread@i64` / `write@i64`; explicit global
capture `:{n board solutions}solve`; reversed `[]` parameter groups; label
control flow with `jnz` / `jmp`.

## p1038

MAD implementation of 洛谷 P1038 [NOIP 2003 提高组] 神经网络
(<https://www.luogu.com.cn/problem/P1038>). A layered DAG of neurons with
`C_i = (Σ W_ji * C_j) - U_i`; prints every output neuron whose final state
is positive, in id order, or `NULL`. `input` holds the official sample,
which produces `3 1`, `4 1`, `5 1`.

Implementation notes: states, thresholds, degree tables, worklist queue and
dense matrices are all flat `mem` blocks (element `i` at offset `i * 8`,
cell `(i,j)` at `(i * (n + 1) + j) * 8`); worklist-based propagation where
calm neurons still release successors so zero-signal paths never stall.

## fileio

File I/O round trip: opens a file for writing with `fopen`, writes a string
with `fwrite`, closes it, reopens for reading with `fopen`, queries size with
`fsize`, reads back with `fread`, prints with `printstr`, and closes. Exercises
the `file` handle type and all five file I/O built-in words.
