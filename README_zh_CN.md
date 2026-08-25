# MAD — MAD Ain't Disciplined

MAD 是一门刻意保持极简的后缀/数据栈语言。实现为 C17 解释器，**不构建 AST**。

完整语言参考：[`MANUAL_zh_CN.md`](MANUAL_zh_CN.md) — [`English`](MANUAL.md)

## 设计理念

MAD 以最小的实现成本换取较强的功能，融合三种传统：

- **数据流：FORTH 式栈思维。** 没有传统意义上的变量——值存在于数据栈上，
  字（word）弹出操作数并压入结果，书写顺序即执行顺序。括号仅为视觉分组
  （`(a b +) c *` ≡ `a b + c *`）。

- **控制流：汇编式标签与跳转。** `label condition jnz` 遵循汇编约定——
  条件在前、`jz`/`jnz` 条件跳转、`jmp` 无条件跳转。没有 while/for/if——
  程序员以分支的方式思考。

- **数据结构：C/汇编式内存管理 + LISP 式手工解释。** `alloc`/`halloc` 提供
  带边界检查的原始字节缓冲；`mread@T`/`write@T` 访问类型化槽位；`sizeof` 查询大小。
  程序员像在 C 中一样手动构建和解释数据结构——数组、树、图——但有运行时安全网
  （边界检查、悬垂指针检测、类型检查）。

结果：一门实现极其简单的语言（只需词法器，不需要语法分析器、AST、树遍历），
却支持递归、一等函数、指针、内存管理、模块导入、带类型 IR 的编译期常量折叠
和类型化 opcode 特化。

## 构建

```sh
make            # 从 src/ 构建 ./mad
make test       # 构建并运行基于示例的回归测试
make clean      # 清理构建产物
```

`CC`、`OPTFLAGS`、`CFLAGS` 可覆盖，如 `make CC=gcc OPTFLAGS="-O0 -g"`。

## 项目结构

```text
src/          解释器源码
  common.*    共享工具：只增向量、诊断、文件 IO
  lexer.*     词法器（[] 组反转、:{...} 全局捕获、类型字面量）
  value.*     运行时值（内联标量）、内存对象、指针、数据栈
  vm.h        VM 状态与符号表结构
  symtab.c    函数/标签发现（token 流扫描）
  ir.*        中间表示：构建、检查、优化、降级
  tcode.*     线程码编译器（token 分类、指令发射、跳转融合）
  exec.c      运行时：帧、变量解析、dispatch 循环（labels-as-values）、内建字
  main.c      CLI 入口
examples/     每个程序一个目录：main.mad + input + expected，
              全部由 `make test` 运行
tests/        checker 单元测试（tests/checker/）
```

## 快速示例

```mad
:fact [n@i64]
    (n 2 <) base jnz
    (n 1 -) fact
    n *
    ret
base:
    1
;

5 fact println       // 输出 120
```

## 示例

每个 `examples/<name>/` 目录包含一个独立程序：
`main.mad`（入口，通过相对路径导入同级文件）、可选的 `input`（stdin 输入）、
`expected`（参考输出）。详见 [`examples/README.md`](examples/README.md)：

- `branch/` — 条件跳转（`jz` / `jnz`）两种极性
- `import/` — 嵌套模块导入，含晚绑定函数引用
- `nqueens/` — N 皇后求解器：递归、`mem` 数组、标签控制流
- `p1038/` — 图处理：`mem` 中的扁平 i64 数组、工作表遍历

## 测试

`make test` 运行每个 `examples/*/main.mad`——存在 `input` 时喂入 stdin——
并将 stdout 与 `expected` 做 diff。

## 许可

公共领域。随意使用。
