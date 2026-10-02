# Unified API sketch: headers and examples

| | |
|---|---|
| Status | Normative design sketch, experimental revision 0.2. Nothing is implemented |
| Date | 2026-10-02 |
| Reads with | [examples.md](../examples.md) (every example, generated from here), [concepts.md](../concepts.md), [contract.md](../contract.md), [diagrams.md](../diagrams.md) |
| Previous | revision 3's single header: what it looked like and what replaced it, [history.md §4](../history.md#4-what-revision-3-looked-like) |

This directory holds the normative header set of the proposed unified MTL API and the worked
examples as files that compile. In milestone M0 of the
[implementation plan](../implementation-plan.md) the headers move to `include/mtl/experimental/`.

| Path | What |
|---|---|
| `include/mtl/experimental/mtl.h` | the core: everything a sender or receiver with MTL's buffers needs (32 functions) |
| `include/mtl/experimental/mtl_*.h` | 16 optional headers, each with one job ([examples.md §1](../examples.md)) |
| `examples/ex01_*.c` … `examples/ex13_*.c`, `examples/examples_cpp.cpp`, `examples/ex_common.h` | the examples |
| `examples.md.in` | the prose of [examples.md](../examples.md), with `@@EXAMPLE <file>@@` and `@@HEADER_TABLE@@` markers |
| `gen_api_doc.py` | writes [examples.md](../examples.md) from the template: every example copied verbatim, the header table counted from the headers |
| `check.sh` | the doc test |

## Running the check

```bash
doc/unified-api/sketch/check.sh
python3 doc/unified-api/sketch/gen_api_doc.py   # after changing an example, a header or the template
```

`check.sh` compiles:

- every header alone: plain, with `-DMTL_LATER` for the later-phase declarations, and as a
  binding generator sees it;
- all headers together in one file, in both orders, and as C++;
- the legacy headers and the new ones together, when a configured build tree
  (`build/mtl_build_config.h`) exists, so a gradual port can mix them;
- every example, with `gcc -std=c99 -Wall -Wextra -Wpadded -Werror` (headers also with
  `-pedantic`) and `g++ -std=c++17 -Wall -Wextra -Werror`, and again with clang and clang++
  when installed.

It then checks that:

- every public struct has a size check;
- `mtl.h` includes only the C library, and every other header only its siblings;
- no identifier says "passthrough" (packet mode is `MTL_UNIT_PACKETS`);
- `examples.md` shows every example verbatim.

It prints the function count per header and per call class, and exits non-zero on any
failure. The examples only have to compile; they are never linked.

Not in `check.sh` yet: every header with `-Wconversion -Wsign-conversion -Wcast-qual` (clean on
2026-10-02, with and without `-DMTL_LATER`). The R4 header review ran it once; `-Wcast-qual` is
what caught `mtl_pkt_tx_table()` returning a writable table from a `const struct mtl_unit*`
(RV-44), so it is worth adding.

## Rules for changing the sketch

- **Header wins over prose.** If a document and a header disagree about a name, a type, a
  layout or a call class, the header is right and the document is fixed. A design change that
  adds or renames a symbol is complete only when it is in a header and `check.sh` passes.
- **`MTL_LATER`.** Functions of later phases, and the types only those functions use, are
  declared under `#if defined(MTL_LATER)`. Enum values, flags, option keys, events and struct
  fields that the always-present headers carry are declared unconditionally, so the ABI freeze
  reserves them and a later phase adds code, not layout changes.
- **Values follow the legacy enums** (D-97): legacy + 1 where 0 must mean "not set", the same
  values where 0 is a real default.
- **Sizes are checked.** Every public struct has `MTL_SIZE_CHECK`; padding is explicit
  (`-Wpadded`).
