# Examples

Runnable MAD programs. Build the interpreter first (`make` at the repository
root), then run any example with:

```sh
./mad examples/<program>.mad          # reads input from stdin
printf '4\n' | ./mad examples/nqueens.mad
```

Each program is also wired into `make test` with fixed input/expected pairs
under `tests/`.

## Style: grouping with parentheses

`( )` have no semantics — the lexer treats them as whitespace. Both examples
use them purely for readability, following one convention:

- `(a b op)` — the operands of one action, e.g. `(c 1 +)`, `(r n >=)`
- `(mem (i 8 *) mread@i64) &x =` — a load feeding an assignment
- `(v mem off) write@i64` — all operands of a memory write
- `(cond...) label jnz` — condition grouped, jump target outside

So `(n 0 ==) done jnz` reads as *if (n == 0) goto done*, and each line stays
one or two visual statements instead of flat assembler soup.

## nqueens.mad

Solves the N-Queens problem: reads `N` from stdin, prints every solution as
an `N x N` board where `Q` marks a queen, then prints the total number of
solutions.

```sh
$ printf '4\n' | ./mad examples/nqueens.mad
.Q..
...Q
Q...
..Q.

..Q.
Q...
...Q
.Q..

2
```

Language features exercised:

- recursion — `solve` calls itself with `row 1 +`
- a `mem` block used as an array: the queen column for each row lives at
  byte offset `row * 8`
- scalar memory access via `mread@i64` / `write@i64`
- explicit global capture on function headers: `:{n board solutions}solve`
- parameter binding through reversed `[]` groups: `[row@i64]`
- label-based control flow with `jnz` / `jmp`

## p1038.mad

A MAD implementation of 洛谷 P1038 [NOIP 2003 提高组] 神经网络
(<https://www.luogu.com.cn/problem/P1038>). The network is a layered DAG of
neurons; each non-input neuron's state follows
`C_i = (Σ W_ji * C_j) - U_i`, and only neurons with `C_i > 0` propagate
their signal. The program prints every output-layer neuron (out-degree 0)
whose final state is greater than zero, in increasing id order, or `NULL`.

Input follows the problem statement:

```text
n p              // neuron count, edge count
c_1 u_1 ...      // n lines: initial state and threshold
i j w_ij         // p lines: directed edge with weight
```

The official sample from the problem statement produces:

```sh
$ printf '5 6\n1 0\n1 0\n0 1\n0 1\n0 1\n1 3 1\n1 4 1\n1 5 1\n2 3 1\n2 4 1\n2 5 1\n' | ./mad examples/p1038.mad
3 1
4 1
5 1
```

Implementation notes:

- all data structures — states `C`, thresholds `U`, in/out degree tables,
  the worklist queue, and dense matrices `W`/`E` — are flat `mem` blocks;
  element `i` is stored at byte offset `i * 8`, matrix cell `(i,j)` at
  offset `(i * (n + 1) + j) * 8`
- worklist-based topological propagation; calm neurons (`C <= 0`) still
  release their successors so zero-signal paths never stall the traversal
- each threshold is subtracted exactly once, after the last predecessor
  has contributed

This example doubles as an end-to-end regression test: `tests/p1038.in`
holds the official sample shown above.
