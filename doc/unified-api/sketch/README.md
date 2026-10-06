# Unified API sketch: headers and examples

| | |
|---|---|
| Status | Normative design sketch, experimental revision 0.2. Nothing is implemented |
| Date | 2026-10-02 |
| Reads with | [examples.md](../examples.md) (every example, generated from here), [concepts.md](../concepts.md), [contract.md](../contract.md) |

This directory holds the normative header set of the unified MTL API and the worked examples as
files that compile. In task H1b of MS1 ([implementation plan §5.2](../implementation-plan.md#52-tasks))
the headers move to `include/mtl/experimental/`, and `check.sh` runs in CI.

| Path | What |
|---|---|
| `include/mtl/experimental/mtl.h` | the main header: everything a sender or receiver with MTL's buffers needs |
| `include/mtl/experimental/mtl_*.h` | the optional headers and their jobs ([examples.md §1](../examples.md)) |
| `examples/ex01_*.c` … `examples/ex17_*.c`, `examples/examples_cpp.cpp`, `examples/ex_common.h` | the examples |
| `examples.md.in` | the prose of [examples.md](../examples.md), with `@@EXAMPLE <file>@@` and `@@HEADER_TABLE@@` markers |
| `gen_api_doc.py` | writes [examples.md](../examples.md) from the template (every example copied verbatim, the header table from the headers), and from task H1b every marked table and enum block from its `.def` ([README §1, §4](../README.md)); `--check` only compares |
| `numbers.txt` | the load-bearing numbers and their homes, for the number lint ([README §1](../README.md)) |
| `check.sh` | the doc test, and the one source of the counts: exported functions per header, per call class and per milestone, and the number of frozen names |

## The headers and the library

There is one library, libmtl, with the unified functions in version nodes per milestone,
sealed at every release ([migration.md §7.2](../migration.md#72-the-unified-library)).

The sketch holds the whole design, and the installed header is the same file. Every exported
function carries its milestone twice: the tag that ends its comment, `(MS1)` to `(MS7)`, and the
argument of its call-class macro, `MTL_API_DP(1)`; it is exported from that milestone on.
`mtl.h` defines `MTL_LEVEL`, the last milestone whose exit passed; a call to a function of a
later milestone fails to compile with GCC and Clang 14 or later, naming the milestone, and at
link time elsewhere. A function tagged `(Phase 7)` or `(later)` is declared only under
`MTL_LATER`, with the argument `LATER`; `MTL_LATER` is for design checks, and a call to such a
function never compiles.

## Running the check

```bash
doc/unified-api/sketch/check.sh
python3 doc/unified-api/sketch/gen_api_doc.py   # after changing an example, a header or the template
```

`check.sh` compiles:

- every header alone: plain, with `-DMTL_LATER` for the declarations of later milestones, and as a
  binding generator sees it;
- all headers together in one file, in both orders, and as C++;
- the legacy headers and the new ones together, so a gradual port can mix them (with the
  build tree's `mtl_build_config.h`, or a stub of it); ex16, which includes `mtl_api.h`, is
  compiled against the legacy headers too;
- every example, with `gcc -std=c99 -Wall -Wextra -Wpadded -Werror` (headers also with
  `-pedantic`) and `g++ -std=c++17 -Wall -Wextra -Werror`, and again with clang and clang++
  when installed; an example compiles to object code with `MTL_TARGET_LEVEL` set to the
  milestone of its `Needs: MSn` line, so it fails if it calls a later function;
- all headers together to object code at levels 0 and 7, and probes that a call to an MS3
  function, directly or through its inline wrapper, fails at level 2 naming MS3 and compiles at
  level 3 (with each compiler that has the `error` attribute).

It then checks that:

- every public struct has a size check;
- `mtl.h` includes only the C library, and every other header only its siblings (`mtl_util.h`,
  inline only, may also include `<string.h>`);
- no identifier says "passthrough" (packet mode is `MTL_UNIT_PACKETS`);
- the comment above every exported function ends with a milestone tag: `(MS1)` to `(MS7)`
  outside `MTL_LATER`, `(Phase 7)` or `(later)` inside it, and the call-class argument is the
  same milestone (`LATER` inside `MTL_LATER`);
- `examples.md` shows every example verbatim, and `concepts.md` its copies of ex01 and ex02;
- the reasons of `mtl_reasons.h` ascend and equal the marked table of contract.md §8.3 (until
  task H1b replaces this lint with `gen_api_doc.py --check`, which also compares every marked
  table with its `.def`, and from H1b every enum constant and integer macro outside
  `MTL_LATER` has exactly one row in `reasons.def`, `mtl_options.def` or `availability.def`);
- no number of `numbers.txt` is stated outside its home;
- every `ST_CORE_LEGACY_*` name of `lib/src/st2110/core/` has a row in migration.md §6.5.

It prints the exported functions per header, per call class and per milestone, and the number
of frozen names (exported functions, inline functions, enum constants, macros and structs outside
`MTL_LATER`, without `mtl_debug.h` and `mtl_legacy.h`, neither of which is part of
`MTL_1.0`; macros ending in `_` are internal and not counted), and exits
non-zero on any failure. `check.sh --tags` prints each exported function with its milestone and
header, for the export check of libmtl. The examples only have to
compile; they are never linked. Documents do not repeat these counts; they point here.

Not in `check.sh` yet: every header with `-Wconversion -Wsign-conversion -Wcast-qual` (clean on
2026-10-02, with and without `-DMTL_LATER`). It is worth adding: `-Wcast-qual` catches an inline
helper that returns a writable pointer from a `const struct mtl_unit*`.

## Rules for changing the sketch

- **Header wins over prose.** If a document and a header disagree about a name, a type, a
  layout or a call class, the header is right and the document is fixed. A design change that
  adds or renames a symbol is complete only when it is in a header and `check.sh` passes.
- **Milestone tags.** Every exported function's comment ends with the milestone that implements
  it, and its call-class macro takes the same number; a function's milestone is this tag, a
  value's its row in `availability.def`, `mtl_options.def` or `reasons.def`, and a feature's the
  U-row of [coverage.md](../coverage.md), the latest milestone of what it names (D-137). An inline wrapper carries
  no tag: it works from the latest milestone it calls. `MTL_LEVEL` changes only in the exit
  commit of a milestone (implementation-plan.md §5.6).
- **`MTL_LATER`.** Everything of Phase 7 (functions, types, enum values, flags, option keys,
  events, reasons) and the features out of v1 (created timelines, the locked phase snap) are declared under `#if defined(MTL_LATER)`, with their values kept. A Phase 7 name
  leaves `MTL_LATER` in the milestone that implements it (the RTCP sender-report keys in MS5,
  `MTL_TIME_SOURCE_FREERUN` in MS6, as their comments say). Every other value (enum values,
  flags, option keys, port prefixes, `when` kinds, wait masks, update parts) is declared
  unconditionally, so the freeze reserves it and a later milestone adds code, not layout.
- **Not implemented yet** is `-MTL_ENOTSUP` with reason `NOT_IMPLEMENTED` for a known value of
  an exported call, and `-MTL_EINVAL` for an unknown value (mtl.h R1). A function that is not
  implemented is not exported.
- **Values follow the legacy enums** (D-97): legacy + 1 where 0 must mean "not set", the same
  values where 0 is a real default.
- **Sizes are checked.** Every public struct has `MTL_SIZE_CHECK`; padding is explicit
  (`-Wpadded`). Each essence member of `mtl_session_config` is 128 bytes, so an essence can grow
  inside its own member.
- **Export only what needs the library.** A function built only on public calls is `static
  inline` and exports no symbol (`mtl_session_open`, `mtl_stat_get`, `mtl_time_convert`,
  `mtl_flow_parse`, `mtl_util.h`). The exception is a parser of bytes from the network, a
  function that walks lengths, counts or offsets it reads from the data (an RTP payload, an
  RTCP block, SDP text): it is never inline but one exported function, so libmtl and every
  application run one implementation, a fix reaches them all by replacing the library, and
  one fuzz harness covers it (D-195). libmtl exports it when libmtl uses it too
  (`mtl_anc_rfc8331_decode`, `_encode`, `mtl_rtcp_mib_next`); a parser libmtl does not need
  lives in a companion library on public calls (SDP: `libmtl_sdp`, `mtl_sdp.h`). Fixed-offset
  accessors of a header the caller has bounded (`mtl_rtp_get_ts`, `mtl_anc_udw_get`) stay
  inline. A verb several objects share is one exported function over
  `struct mtl_object` (`mtl_close`, `mtl_interrupt`, `mtl_wait`, `mtl_reap`,
  `mtl_read_events`; `mtl_release` over a lease), with `static inline` typed wrappers
  that keep each object's own name and pass the record size (`mtl_tx_reap`, `mtl_tx_reap_full`,
  `mtl_session_read_events`).
