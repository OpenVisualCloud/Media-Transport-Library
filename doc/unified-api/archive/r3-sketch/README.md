# Unified API sketch: header and examples

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Reads with | [10 — API sketch](../10-api-sketch.md) (the readable companion), [11 — ABI](../11-abi-compatibility-and-migration.md) |

This directory holds the normative header set of the proposed unified MTL API, experimental
revision 0.1, and the worked examples of [10](../10-api-sketch.md) as files that compile. It is
design material: no library code implements it yet `[C5 §2.2, A1]`.

| Path | What |
|---|---|
| `include/mtl/experimental/mtl_unified.h` | the full API: every constant, enum, struct, handle and function named in the design, with call classes, `since` markers, nullable annotations and C99 size checks |
| `include/mtl/experimental/mtl_simple.h` | the L4 "simple" layer (A13) |
| `include/mtl/experimental/mtl_debug.h` | test time source, fault injection, null-backend note (A12) |
| `examples/ex01_*.c` … `examples/ex12_*.c` | one C99 file per example section of 10 |
| `examples/examples_cpp.cpp` | the C++17 twin: `*_init()` functions, handles, stride-safe reads |
| `check.sh` | the doc test |

## Running the check

```bash
doc/unified-api/sketch/check.sh
```

It compiles every header (plain, `-DMTL_UNIFIED_NO_INLINE`, `-DMTL_UNIFIED_LATER`) and every
example with `gcc -std=c99 -Wall -Wextra -Wpadded -Werror` (headers also with `-pedantic`) and
`g++ -std=c++17 -Wall -Wextra -Werror`, and again with clang and clang++ when installed. It then
checks that every public struct and union has a size check and every `struct_size`-bearing
struct has an exported `*_init()`, and prints the exported-function count per header and per
call class. It exits non-zero on any failure. The examples only have to compile; they are never
linked.

## Header wins over prose

If a document in `doc/unified-api/` and this header disagree about a name, a type, a layout or a
call class, the header is right and the document is fixed. A change to the design that adds or
renames a symbol is not complete until it is in the header and `check.sh` passes.
