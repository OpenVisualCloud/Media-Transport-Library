# 11 — ABI, compatibility, bindings and migration

| | |
|---|---|
| Status | Draft for maintainer review — revision 3 (after reviews C1–C5) |
| Date | 2026-09-30 |
| Requirements | R-ABI-1…3, GO-8, GO-9 |
| Research | [R13 lifecycle/ABI §4–7](research/13-lifecycle-errors-abi.md) (primary), [R08 consumers §5](research/08-consumers-ecosystem.md), [R09 libfabric §9](research/09-libfabric.md), [R10 Rivermax §1.2](research/10-rivermax.md), [R11 §5.1, §6.3](research/11-media-io-prior-art.md) |
| Header | [`sketch/include/mtl/experimental/mtl_unified.h`](r3-sketch/include/mtl/experimental/mtl_unified.h) states every rule below as conventions C1–C11 and is compiled by [`sketch/check.sh`](../sketch/check.sh); where this page and the header disagree, the header wins |

> **Revision 4 (2026-10-01).** The rules this document sets still hold, except where
> [REVISION-4.md §3](REVISION-4.md#3-the-decisions) changes them (one "nothing now" code, submit
> failure, completion modes, groups, queues). Its API names are revision 3's; [§6](REVISION-4.md#6-revision-3--revision-4-names)
> maps them to the revision-4 headers, which win wherever the two differ.

## 1. Where MTL stands today `[R13 §4]`

| Item | State |
|---|---|
| soname | `libmtl.so` with no version; never bumped. `lib/meson.build:151-160` builds it with `shared_library(…)` and no `soversion`, `version`, `gnu_symbol_visibility` or version script |
| Visibility | everything exported: 757 dynamic function symbols, 235 of them internal (`mt_*`, `tv_*`, `rv_*`) |
| Version script | none |
| Struct versioning | none; `ops` structs are copied with the library's `sizeof`; no reserved fields |
| Silent breaks already shipped | `rl_burst_size` added inside `port_params[]` embedded in `mtl_init_params` (every later field moved); stats fields renamed in place |
| Enums | ~30 exported `_MAX` sentinels; a 64-bit `enum mtl_init_flag` (non-portable; MSVC keeps enums `int`) |
| Runtime version | `mtl_version()` string only; no header-vs-library check |
| Experimental tier | `include/experimental/` installed, no opt-in, no symbol tagging |
| Windows | `lib/windows/win_posix.h` defines no errno shims; `ESHUTDOWN` and `ESTALE` do not exist in the UCRT |

In practice there is no ABI promise: apps must rebuild for every release, and the
unversioned soname hides mismatches from the dynamic loader.

## 2. Strategy for the new API

A hybrid, chosen so the data path stays free of setter calls and bindings stay simple
`[R13 §7]`:

| Surface | Technique | Why |
|---|---|---|
| Objects | opaque 64-bit handles of distinct struct types; ID 0 is null for every type; a separate `mtl_lease_h` for access (03 §2) | no layouts exposed; stale-safe; misuse does not compile; trivially bindable `[C5 §2.5, §2.8]` |
| Create-time configuration | `struct_size`-prefixed structs; an exported `mtl_<struct>_init()` for every one; C-only value macros `MTL_<STRUCT>_INIT(...)` | statically typed, C++- and binding-clean `[C5 §2.3]`; Rivermax's init-function technique `[R10 §1.2]` |
| Hot-path descriptors (submission, views, hints) | small POD structs with `struct_size`; `view` and `hint` are nullable, since layouts are immutable | no function call per field; no mandatory copy per call `[C5 §2.19]` |
| Completion records | one 256-byte union; every kind ≤ 240 B; times as TAI ns + a `time_valid` mask; array reads take the stride as an argument | fixed entry, safe growth, stride never rewritten `[C5 §2.1, §2.6]` |
| Rare and backend-specific settings and stats | key-addressed, typed per object: `mtl_session_stat_get`, `mtl_port_stat_get`, `mtl_instance_stat_get`; `mtl_session_set_option` with `mtl_session_option_list` | grows without touching structs (DeckLink statistics `[R11 §1.6]`) `[C5 §2.9]` |
| Library | an experimental DSO `libmtl_unified.so.0.<rev>` with version node `MTL_UNIFIED_EXPERIMENTAL` until the freeze, then `MTL_1.0` (§2.6) | the loader rejects stale binaries during the experimental period `[C5 §2.18]` |
| Version check | `api_version` in the versioned `mtl_instance_params` (per instance); `mtl_version_num()`; `mtl_struct_known_size(kind)`; `since 0.N` markers | one version space, no process-global state, field-level feature detection `[C5 §2.11, §2.12]` |
| Error vocabulary | `MTL_E*` constants with the Linux errno values; `mtl_last_error()` into caller memory | one ABI on every OS `[C5 §2.13, §2.16]` |

### 2.1 `struct_size` and layout rules

1. **Which structs carry a size.** Every top-level public struct that crosses the API
   starts with `uint32_t struct_size`. Exempt are value types that never grow
   (`mtl_time`, `mtl_rational`, `mtl_launch`, `mtl_anc_packet`, `mtl_wait_object`,
   `mtl_format_pair`, the handle types) and fixed-size sub-structs (rule 5). CQ records
   carry `hdr.size` and EQ records `mtl_event.size`, both **outputs** (rule 4).
2. **Input structs.** `struct_size == 0`, or smaller than the first published (0.1) size, is
   `-MTL_EINVAL` with reason `NONZERO_TAIL`, so zero-initialised structs from SWIG `new_*()`
   or Rust `Default` never silently mean "all defaults" `[C3 P1-4, B-2]`. The library reads
   `min(struct_size, known)`; fields beyond the caller's size take their defaults. Non-zero
   bytes beyond the library's known size are `-MTL_EINVAL` with reason `NONZERO_TAIL` —
   the strict rule, documented as a compatibility policy in §2.3.
3. **Output structs.** The caller sets `struct_size` (with the `*_init()` function); the
   library writes `min(struct_size, known)` bytes. The library **never rewrites
   `struct_size`**, in or out `[C5 §2.6]`.
4. **Array reads.** `mtl_tx_reap`, `mtl_cq_read`, `mtl_eq_read` take the caller's record
   size as an argument and write `min(record_size, native)` bytes per record at that pitch.
   In each written record `hdr.size` (or `mtl_event.size`) says how many bytes were written.
   `mtl_rx_dequeue` takes `unit_size` the same way; a 48-byte size gives a header-only fill.
   Hoisting an initialisation out of a loop can no longer shift the pitch of the next read
   `[C5 §2.6, §2.19]`.
5. **Growth.** Structs only grow at the end. Embedded sub-structs (`flows[]`, `pool`,
   `timing`, `plane[]`, stats blocks) are fixed size with a reserved tail and version with
   their parent. **There is no implicit padding**: every hole is a named `reserved` field
   that must be zero, and `-Wpadded -Werror` in `check.sh` proves it; otherwise stack
   garbage in an old binary's padding would become a field the day it is appended
   `[C5 §2.7]`. Every public struct and union has a C99 size check,
   `typedef char mtl_sz_<name>[(sizeof(struct <name>) == N) ? 1 : -1];`, and every CQ
   record kind is checked against `MTL_CQ_RECORD_MAX` (240) `[C5 §2.1]`. Fixed arrays are
   sized once; enum-indexed counters use frozen slot counts (`MTL_TX_REASON_SLOTS` 32,
   `MTL_RX_REJECT_SLOTS` 32).
6. **Enumerations.** Enumerated fields are `uint32_t`; enum types only name the constants,
   and arguments that take them are `uint32_t` too (`mtl_time_convert(…, uint32_t
   to_clock, …)`). No `_MAX` sentinel is exported. Status enums start with `*_UNSET = 0`
   so a half-filled record never reads as success `[C5 §2.20]`.
7. **Flags.** Input flag fields and flag arguments are `uint64_t` with `#define`
   constants written as plain integer literals (`0x1u`, `0x2u`), never `(1ull << n)`;
   compact output records use `uint32_t`. Unknown bits are `-MTL_EINVAL` with reason
   `UNKNOWN_BITS`.
8. **Zero-default rule.** A zero-filled input struct (with `struct_size` set) is the
   default configuration: no field has a non-zero default, so a C value macro or a
   binding's zero-initialised struct is always correct `[C5 §2.4]`. Where revision 2 had
   `-1` or "max = off", revision 3 has `numa` (0 = the ports' socket, `MTL_NUMA(n)` = node
   n), `flow.port` (0 = leg *i* on instance port *i*, `MTL_FLOW_PORT(n)` = port n), filter
   flags (`MTL_FLOW_MATCH_FMD_DIT`, `MTL_FLOW_MATCH_FMD_K`) and option flags
   (`MTL_AUDIO_NO_ABSORB`). The fields whose zero means something
   other than the number zero are listed in the defaults tables of
   [09 §9.1 and §9.2](09-media-modes-and-backends.md), and a CI lint checks those tables
   against the header.
9. **Pointers.** A few structs hold pointers (`mtl_session_config.next`,
   `mtl_mem_desc.va`, `mtl_tx_submission.user_meta` and `.anc`, the view addresses), so
   `struct_size` is `sizeof` on the target ABI. The header supports 64-bit targets only
   (LP64 and LLP64; MTL builds for x86-64 today) and says so with an `#error`; bindings
   must build structs with the `*_init()` functions `[C5 §2.20]`.
10. **Handles in structs.** A null handle in a config struct means "the documented
    default" (a null `timing.timeline` is the epoch timeline, which also has a real
    handle, `mtl_timeline_epoch(mt)`); a null handle passed to a call is `-MTL_EBADF`
    `[C5 §2.8]`.

### 2.2 Where a knob lives

Revision 2 had four mechanisms for one session's tunables and no rule for which goes
where `[C5 §2.14]`. The rule (response A8):

| Mechanism | Holds | Example |
|---|---|---|
| `mtl_session_config` field (incl. its fixed sub-structs) | cross-essence settings, validated at create | `flows[]`, `pool`, `timing.media_mode`, `completion.mode` |
| media config (`mtl_video_config`, `mtl_cvideo_config`, `mtl_audio_config`, `mtl_anc_config`, `mtl_fastmeta_config`) | essence-specific settings | `troffset_ns`, `audio_absorb_samples`, `anc_timing_model` |
| `next` extension block | optional feature groups | later: progressive details, RTCP, HDR metadata |
| key-value option (`MTL_OPT_*`) | backend-specific settings only, enumerable through `mtl_session_option_list` | pacing pad interval, RL warm-up, histogram bucket width |
| flag bit | orthogonal booleans | `MTL_SESSION_MT_SUBMIT`, `MTL_FLOW_USER_MAC` |

The seven essence-specific knobs of revision 2 moved into their media configs:
`options.anc_split_by_packet`, `timing.anc_timing_model` and `timing.anc_target_delay_ns` into
`mtl_anc_config`; `options.audio_build_pacing`, `options.audio_fifo_ms` and
`timing.audio_launch_offset_ns` into `mtl_audio_config`; `timing.troffset_ns` into
`mtl_video_config` and `mtl_cvideo_config`. By the same rule `timing.sender_type` moved to the
video and cvideo configs and `timing.progressive_late` to the video config, and the new
per-essence capacities (`rx_unit_samples`, `buffer_capacity_bytes`, `max_udw_bytes`,
`max_packets`) live in the media configs. Reserved tails are sized by observed growth:
`pool.reserved[6]`, `timing.reserved[12]` (96 B), `options.reserved[11]`.

The honest reading of the surface, as C5 put it, stays true: the *name* surface drops
because five essences share one verb set, while the number of decisions one session exposes
roughly doubles as flag bits become typed fields, and usability rests on defaults. That is
why the zero-default rule and the 09 defaults table are CI checks, not prose.

### 2.3 Compatibility policy: the strict rule

The strict rule (rule 2) is stricter than Win32 `cbSize` (which ignores unknown tails) and
Vulkan (which enumerates extensions). It is kept, because an ignored field the app believes
is honoured is a silent timing or memory bug. It is a user-facing policy, so it is written
down with the tools to live with it `[C5 §2.11]`:

- **What happens.** An app built against 0.3 that sets a field introduced in 0.3 fails on a
  0.2 library with `-MTL_EINVAL`, reason `MTL_REASON_NONZERO_TAIL`. An app that leaves new
  fields at zero runs on any library that knows the struct's 0.1 size.
- **Field-level detection.** `mtl_struct_known_size(MTL_STRUCT_<KIND>)` returns the size
  the running library understands; `mtl_version_num()` returns its API version; every item
  in the header carries a `since 0.N` marker (a field inherits its struct's marker unless it
  has its own).
- **The pattern.**

  ```c
  /* new_field: a hypothetical field added in 0.3 at the end of mtl_session_config */
  struct mtl_session_config sc;
  mtl_session_config_init(&sc); /* struct_size = the header's size */
  size_t need = offsetof(struct mtl_session_config, new_field) + sizeof(sc.new_field);
  if (mtl_struct_known_size(MTL_STRUCT_SESSION_CONFIG) >= need) /* or version >= 0.3 */
    sc.new_field = wanted;                   /* the library knows it: set it */
  /* else leave it 0: the call succeeds on the older library, without the feature */
  ```

- **Bindings.** Python and Rust wrappers call `mtl_struct_known_size` once at load and hide
  unsupported fields instead of letting the call fail.

### 2.4 Optional extension chains

`struct_size` couples unrelated extensions into one growing struct. If media types keep
adding optional blocks (HDR metadata, progressive details, new 2022-7 options), a typed
`const void* next` chain on create-time config only (Vulkan `sType/pNext`, where components
skip unknown `sType`s) is the escape hatch `[R11 §5.1]`. v1 reserves `mtl_session_config.next`
and requires it to be NULL (Q-ABI-3). Backend-specific extensions could use named, versioned
extension tables (`mtl_open_ext(mt, name, version, &ops)`, libfabric's `fi_open_ops`) for
DATA_PATH_ONLY, AF_XDP knobs and queue meta `[R09 Q12]`; it is declared in the header's
`MTL_UNIFIED_LATER` block only.

### 2.5 One version space, per instance

Revision 2's `mtl_session_api_init(MTL_API_VERSION)` was process-global, so a GStreamer and
an FFmpeg plugin in one process could not both be honoured, and `MTL_API_VERSION 1u` was a
second version space next to `MTL_VERSION_NUM` `[C5 §2.12]`. It is removed:

- `mtl_instance_params.api_version` states what the caller was written for, per instance
  (0 = the header it was compiled with). libfabric scopes the version per `fi_getinfo` call
  the same way.
- `MTL_UNIFIED_API_VERSION` and `mtl_version_num()` use the `MTL_VERSION_NUM(a, b, c)`
  encoding of `include/mtl_api.h`. During the experimental period the value is
  `MTL_VERSION_NUM(0, rev, 0)`; at the freeze it becomes the library release.
- `mtl_instance_params` replaces the legacy `mtl_init_params` in every new prototype and
  lands before the new API, as Phase 0 work (response A11, Q-ABI-2). It has no 64-bit enum,
  no 2-D `char port[][]`, and is never modified by the library.

### 2.6 Experimental staging: its own DSO

An `MTL_ALLOW_EXPERIMENTAL_API` marker has no teeth when every consumer defines it on day one,
and an unversioned soname lets a mismatched consumer load and misbehave `[C5 §2.18]`. The new
API therefore ships as its own library until the freeze (response 2.18):

| Item | Experimental (Phase 1 to freeze) | After the freeze |
|---|---|---|
| Library | `libmtl_unified.so.0.<rev>`; it links `libmtl` | merged into `libmtl.so.<major>` or kept as `libmtl_unified.so.1` (decided at freeze) |
| soname | bumped on **every** incompatible pre-freeze change, so `ld.so` rejects stale binaries | bumped on ABI breaks only |
| Symbols | version node `MTL_UNIFIED_EXPERIMENTAL` | promoted to `MTL_1.0` |
| Internal entry points | `libmtl` exports what the DSO needs under a private node `MTL_INTERNAL`; apps must not use it | same |
| Header | `include/mtl/experimental/mtl_unified.h`, `mtl_simple.h`, `mtl_debug.h` | `include/mtl/` |
| Debug API | `mtl_debug.h` functions exist only with `-Denable_debug_api=true`; otherwise `-MTL_ENOTSUP` | same |

Q-ABI-1 is decided with this: `libmtl` itself gets a soname and a version script in Phase 0,
with `-fvisibility=hidden` and the `MTL_API` export macro, so internal `mt_*`, `tv_*` and
`rv_*` symbols stop leaking.

### 2.7 Call classes are enforced, not only annotated

The `MTL_API_CP/DP/DPC/WT/AS` macros document the class of every function and all expand to
`MTL_API` (empty under SWIG and bindgen). Revision 2 relied on a header lint that only checked
that a macro name was present `[C5 §2.15]`. Revision 3 enforces the classes in debug builds
(response A9, 04 §3): a thread-local "current class" is set at every entry point, and
`mt_rte_zmalloc`, mutex lock, sleep and `info()`/`warn()` assert that it is not DP, DPC or AS;
`MTL_API_WT` is checked dynamically (DP when `timeout == 0`); a signal-safety test raises the
signal inside every CP call under a `malloc` and `pthread_mutex_lock` interposer. From a library
busy-loop thread only the inline-safe subset and the AS calls are legal; everything else is
`-MTL_EDEADLK`.

## 3. Windows

Windows support in revision 2 was broken by construction: `int* fd` cannot carry a `HANDLE`,
and the error vocabulary used codes the UCRT lacks `[C5 §2.13]`. Revision 3 makes the header
one ABI everywhere:

- **Error codes** are `MTL_E*` constants with fixed values equal to Linux errno. On Linux
  `-MTL_EINVAL == -EINVAL`; on Windows applications compare against `MTL_E*` only. All sixteen
  codes exist on Linux, so no private value is needed today; values ≥ 1000 are reserved for
  MTL-only codes.
- **Wait objects** are `struct mtl_wait_object { intptr_t native; uint32_t kind; … }` with
  `kind = MTL_WAIT_FD` (an eventfd) or `MTL_WAIT_WIN_HANDLE` (an auto-reset event), returned by
  `mtl_session_get_wait_object`, `mtl_cq_get_wait_object` and `mtl_eq_get_wait_object`. The
  `trywait` protocol is the same on both.
- **Thread-local error state** is copied into caller memory by `mtl_last_error()`, so no
  pointer to `__declspec(thread)` storage crosses a DLL boundary.
- **Export macro**: `__declspec(dllexport/dllimport)` under `_WIN32`, visibility attributes
  elsewhere.
- **Scope**: Windows is supported with the DPDK backend only, verified by a CI compile-only job
  of the headers and examples (14). Q-ABI-6 moves from "(b) fd API returns `-ENOTSUP`" to
  "(a) first-class wait objects" at no extra API cost.

## 4. Language bindings

| Construct | SWIG (Python) | bindgen (Rust) | Rule for the new headers |
|---|---|---|---|
| opaque `struct { uint64_t id; }` handles | fine | fine | used for every handle; null = ID 0 |
| `MTL_API_*` call-class macros | must expand to nothing | same | defined empty when `SWIG` or `__bindgen` is defined `[C5 §2.20]` |
| `static inline` helpers (`mtl_*_is_null`, `mtl_*_eq`, `mtl_object_*`) | fine | skipped without `--wrap-static-fns` | the same names are exported functions when `MTL_UNIFIED_NO_INLINE` is defined, which the binding guards set automatically |
| view addresses (`void*`) | needs `ctypes.cast` | raw pointer | `MTL_ADDR(T)` makes them `uintptr_t` in bindings; `mtl_lease_copy_in/out` copy without touching the address `[C5 §3.5]` |
| CQ union arrays | no indexable array of unions | `__BindgenUnionField` | `mtl_cq_read(cq, void* buf, size_t record_size, max, timeout)`: allocate `record_size × max` bytes, index by pitch, dispatch on `hdr.kind` |
| anonymous unions and structs | awkward | `__bindgen_anon_1` | none: every union and sub-struct is named |
| fixed 2-D char arrays (`port[2][64]`) | needs C setter helpers today | fine | none; ports are an array of `struct mtl_port_spec` |
| function-like flag macros (`MTL_BIT32(n)`) | fine | not evaluated | plain integer literals |
| C value initialiser macros | not usable | not usable | C only; bindings call the exported `*_init()` |
| callbacks into Python | unusable (the GIL) | hand-written trampolines | none in the core; the L4 `mtl_cq_dispatch_start` takes a C function pointer |
| blocking calls holding the GIL | a blocking `get_frame` holds it up to the timeout | n/a | release the GIL around WT calls (`%thread`) |
| trywait protocol | easy to invert with `-EAGAIN` | same | 1 / 0 / < 0, so `asyncio` `add_reader(wait_object.native)` + `trywait` cannot be inverted `[C5 §2.10]` |

`[R13 §5, R08 §1.2]`

**Reference Python wrapper — a Phase 1 deliverable** (response 3.5). A thin `pymtl.unified`
package over the SWIG module, tested against the null backend (`"null:1"`, A12) so it runs in CI
without a NIC. Scope: context managers for instance and session; the `*_init()` builders; a CQ
iterator that owns the entry buffer and dispatches on `hdr.kind`; `mtl_time` and `time_valid`
decoding; `asyncio` integration through the wait object and `trywait`; `interrupt` wired to
`KeyboardInterrupt`; buffer-protocol access through `mtl_lease_copy_in/out` (or a `memoryview`
over the `uintptr_t` address for zero-copy). Today's `pymtl` binds the installed headers
(`python/swig/pymtl.i:4`, `:27-32`) and fills frames with `ctypes.cast` plus `mtl_memcpy_action`
(`python/example/misc_util.py:446-454`); the wrapper replaces both.

## 5. Coexistence with the legacy APIs

```text
            app A (legacy st20p)        app B (unified)          same process
                   │                          │
   st20p_* ───────┘                          └────── mtl_video_session_*
      │                                                    │
      │ unchanged                                          ▼
      │                                          L2 session core (libmtl_unified)
      │                                                    │ v1 adapter
      ▼                                                    ▼
   st20p pipeline  ◀───────────────────────────────  st20p pipeline (L2↔engine slot interface)
      │
   L0 engines (shared) ── engine fixes reach legacy users; wire-visible changes behind a legacy flag
```

- Both APIs share one instance: `mtl_instance_from_legacy(legacy_handle, &mt)` wraps an
  `mtl_init` handle, and `mtl_instance_to_legacy(mt, &legacy)` goes the other way.
- Engine plumbing is shared (engines-first, response A14): bugfixes reach legacy users by
  default; wire-visible behaviour changes (RTP ±1, the default video RTP, ANC RTP, ST 2110-22
  CBR) stay behind an opt-in legacy flag and are on by default only in the unified API.
- Later (Q-ABI-4): re-base the legacy pipeline APIs on L2 so fixes land once. The L0 hooks are
  specified as the L2↔engine slot interface (new pipeline entry points), so "wrap now" and
  "extract the common core" are the same code; the Phase 6 re-base has a go/no-go at the Phase 2
  exit. The session-level callback APIs, slice and RTP modes cannot be shimmed without a thread
  hop; they stay on their current implementation and are frozen.

**Known mixed-API hazard** `[C5 §5.15]`. A **legacy ST 2110-20** session with the default
TX-cursor RTP stamps the media time plus the first-packet offset (TRO − VRX0·TRS), while every
epoch-based session — unified, or legacy ANC and audio — stamps the media time (06 §13,
Q-TIME-15/16). At 1080p59.94 that is ≈ 604–619 µs depending on the granted VRX0, **≈ 54.4–55.7
ticks** at 90 kHz (C5's ≈ 608 µs ≈ 54.7 ticks is one point of that range). A **legacy ST 2110-40**
session paired with a unified ST 2110-20 session agrees within ±1 tick, because legacy ANC
already stamps its epoch and only the rounding differs; C5 had that direction reversed. So a
programme must not mix a legacy video session on its default RTP with anything else: use
`USER_TIMESTAMP` with the Phase 0.5 timeline helper on every legacy session, or move the whole
programme to one API.

## 6. Consumer migration

### 6.1 Consumers

| Consumer | Size (call sites) | Effort | What the new API gives them |
|---|---|---|---|
| FFmpeg plugin | 61 in 6 files | small; a rewrite | RX zero-copy via `dequeue` + `av_buffer_create(free = mtl_rx_release)` (5 `/* todo: zero copy */` comments today); TX per packet through `MTL_POOL_DYNAMIC` in Phase 4 (copy until then) `[C3 P2-3, C5 §6.3]`; real timestamps (§6.2) |
| GStreamer plugin | 58 in 8 files | small–medium | `unlock()` / `unlock_stop()` via sticky `mtl_session_interrupt` / `uninterrupt`; stop via deferred destroy and `SESSION_RETIRED`; export pool with the lease-level rules of §6.2; LATENCY from `get_info`; ALLOCATION from the dry-run query; pollable wait object; reference-timestamp meta from `mtl_time` |
| OBS | 10 in 2 files | input trivial; output small | input: `dequeue(timeout)` + `mtl_time_convert(→ MONOTONIC)`; output: the live-sink recipe of §6.2 (today's `tfmt = MEDIA_CLK` with an OBS monotonic timestamp is a bug, `ecosystem/obs_mtl/linux-mtl/mtl-output.c:224-225`) `[C5 §7.8]` |
| Python / Rust bindings | 48 / 36 | small | handles + POD structs bind cleanly (§4); the Rust `priv` / `notify_frame_done` trampolines (`rust/src/imtl/video.rs:427`) become CQ reads, which removes the dangling-`priv` UB `[R08 §1.2]` |
| RxTxApp | 156 in 19 files + JSON | medium–large | keep the JSON schema, change the ops-building code; `rtp_timestamp_delta_us` becomes `media_time_offset_ns` (ns, TAI mode), `ST_EVENT_VSYNC` becomes `EPOCH_TICK` (10 §16) |
| Samples | 260 in 35 files | medium | replace with the canonical examples of 10 (Q-MIG-2) |
| KahawaiTest | 752 in 31 files | very large | keep for legacy; the new API gets its own contract suites, largely on the null backend |
| MXL POC, external engines | 52 in 8 files (MXL POC; external engines not counted) | medium | slot lifetime separate from buffer lifetime; `MTL_RX_SLOT_BY_INDEX` (10 §8) `[R08 §1.2]` |

About 10 private downstream repositories exist whose usage was not inspected; polling them
before freezing the design is Q-MIG-1.

### 6.2 Framework recipes

Revision 2 pointed live framework sinks at INDEX mode, where jittery PTS produce `-EINVAL`
and a just-in-time producer is late on every frame `[C5 §5.4, §5.8, §5.9]`. The recipes are
rewritten around one distinction: **live sinks use TAI, whole-timeline owners use INDEX**.

**Live sinks** (GStreamer sink with `sync=TRUE`, `ffmpeg -re`, OBS output):

- `timing.media_mode = MTL_MEDIA_TAI`, `timing.source_kind = MTL_SOURCE_CAPTURE`, snap
  `MTL_SNAP_NEAREST` (the default): each buffer snaps to the nearest slot, a collision is a
  `DROPPED/DUPLICATE_SLOT` result, a skipped slot follows the underrun policy — frame-rate
  adaptation by drop and repeat, never a permanent relock (06 §4.3).
- `submission.media_tai_ns` = the buffer's time in TAI (06 §10.8). GStreamer: `base_time +
  running_time + latency` is pipeline-clock time; map it to TAI with the clock's own offset or
  with `mtl_time_convert()` / `mtl_time_cross_timestamp()`, and report
  `min_submit_lead_ns` as basesink `render-delay`. FFmpeg: the packet PTS rescaled against
  `start_time_realtime`. OBS: `mtl_time_convert(MONOTONIC → TAI)`. The GStreamer `st40p` sink
  moves to TAI on its video's timeline, so ANC keeps the video RTP.
- `timing.media_time_offset_ns` = the framework's declared latency, at least
  `mtl_session_get_info().min_submit_lead_ns` (also returned by the dry-run query), so frames
  handed over at their running time are not late by the lead `[C5 §5.4]`. RTP moves with it.
- Seek or `FLUSH_STOP`: `mtl_session_discard_queued(s, NULL)`; TAI needs no rebase because
  media times stay absolute.
- Audio: `mtl_audio_config.audio_absorb_samples` defaults to one packet for CAPTURE and for
  TAI mode, so submissions within ±one packet of the expected sample are treated as
  contiguous. Larger deviations keep the packet grid: a forward gap is filled with silence
  (`samples_padded`), an overlap is trimmed (`samples_dropped`); only
  `MTL_SUBMIT_DISCONTINUITY` re-phases (06 §8).

**Whole-timeline owners** (file playout, RxTxApp, the A/V/ANC group of 10 §9): INDEX media mode
on an `AT_START` (one process) or EPOCH timeline (across processes, `mtl_rational_index_at`).
Seek: `mtl_session_discard_queued` with `MTL_DISCARD_REBASE` and `first_index`, which maps the
new first index to the next feasible slot of the shared timeline.

**Processors and forwarders**: TAI + CAPTURE with the input's media time and `min_tx_delay_ns`
= the pipeline budget (10 §11, §12). `MTL_RTP_PASSTHROUGH` with AUTO is rejected.

**Exporting a pool to the framework** (GstBufferPool over MTL TX slots; the lease-level rule is
in 03/05) `[C5 §7.2]`:

| GstBufferPool | Unified API |
|---|---|
| `acquire_buffer` | `mtl_tx_acquire` → wrap the lease |
| `render` | `mtl_tx_submit`, then mark the wrapper in flight |
| `release_buffer` of a wrapper never submitted | `mtl_tx_release` |
| `release_buffer` of a submitted wrapper | nothing; the wrapper returns to the pool when its `TX_RESULT` is reaped |
| completion mode | `MTL_COMPLETE_ALL`, forced for exported pools |
| `set_active(FALSE)` | `mtl_session_stop(s, MTL_STOP_FLUSH, 0)` first, so every in-flight wrapper gets its result, then reap, then free |
| declaration | `MTL_SESSION_EXPORT_POOL` in `mtl_session_config.flags`: it forces ALL and implies `MTL_SESSION_MT_SUBMIT`, which exported pools **require** because `render` and upstream threads differ |

**`MTL_SESSION_MT_SUBMIT` coverage** (response A10): `mtl_tx_acquire`, `mtl_tx_release` and
`mtl_rx_release` are MP-safe by default; only `mtl_tx_submit` and `mtl_tx_publish` need one
submitting context. Set the flag whenever two threads can submit (exported pools, a pipeline
with a queue element before the sink, FFmpeg with frame threading); leave it clear, and set
`MTL_SESSION_SINGLE_READER`, for the classic one-thread loop so the reaper lock is elided.

**Importing the framework's pool** (attached pool, two phases) `[C5 §6.13]`:

1. Negotiation: run `mtl_video_session_query()` (no allocation) and answer the ALLOCATION
   query from it: `size = info.unit_bytes`, `min_buffers = req.min_count_direct` (or
   `req.min_count` when not DIRECT) **plus** the upstream pool's own `min_buffers`, `max_buffers
   ≤ req.max_count`, and a `GstVideoMeta` with `req.plane[i].stride` (any `stride ≥ row_bytes`
   is DIRECT-capable). Answer LATENCY with `info.latency_min_ns` / `latency_max_ns`.
2. Activation: once the upstream pool is active, import its arena as **one** region
   (`mtl_mem_import`, `access = MTL_MEM_READ`, page aligned; one region per arena is the
   required pattern because the region budget is small, 05), create one buffer per surface,
   attach, start. On COPY or CONVERT paths, enable `MTL_CQE_ENABLE_SOURCE_RELEASED` (Phase 4)
   so the framework gets its surface back before the terminal result.
3. Teardown: stop FLUSH, reap, destroy, wait for `SESSION_RETIRED`, destroy buffers and the
   region, and only then let the framework free the arena (10 §6).

**Interlaced** (GStreamer `interlace-mode`): the unit is a field and a field layout has
`rows = height / 2`. `interleaved` frames are two field buffers over one surface (plane offset
0 or one row, `stride = 2 × row_bytes`); `alternate` is one field per buffer; PsF is paced as
interlaced with one RTP timestamp and one index per frame (05 §4.3, 06). Today's sink accepts
only `interleaved` (`ecosystem/gstreamer_plugin/gst_mtl_st20p_tx.c:358-362`). Migrants note that
legacy `ops.fps` of an interlaced session is the **field** rate; `mtl_video_config.fps` is the
frame rate, so halve it (06 §3.4).

**FFmpeg specifics**: `AVFMT_FLAG_NONBLOCK` is a demuxer flag, so `read_packet` uses timeout 0
when it is set and `ff_check_interrupt` between short timeouts otherwise; `write_packet` may
block and uses `mtl_tx_write(…, timeout)` or `mtl_tx_acquire` with a timeout `[C5 §7.9]`. The
st30p muxer rescales `pkt->pts` to 1/48000 and marks encoder gaps with
`MTL_SUBMIT_DISCONTINUITY`. `CLOCK_TAI` without ptp4l (`ecosystem/ffmpeg_plugin/mtl_common.c:39-45`, installed as
`ptp_get_time_fn` at `:125`) becomes time source `MTL_TIME_SOURCE_SYSTEM_TAI`, accepted
but labelled ESTIMATED with a timing warning (reason `TIME_ESTIMATED`) — a behaviour change for
`-f mtl_st20p` users on hosts without ptp4l.

**Behaviour changes a migrating plugin sees**: the TX SSRC is random per session unless set;
`udp_port` is required (0 is `-MTL_EINVAL`); a time source that is only estimated (`SYSTEM_TAI`)
is accepted but labelled ESTIMATED with a timing warning.

**Instance parameters** from several elements (OBS per-source lcores and queue counts, the
first element's `mtl_init_params.flags` winning silently today) go through the merge table of
03 §7: invariant fields mismatch with `-MTL_EINVAL` / `INSTANCE_PARAM_MISMATCH`, queue counts
merge, a second element's `lcores` is ignored, and ignored fields are listed in
`mtl_instance_info.ignored_fields` (the OBS recipe is 09 §8.2).

**Stats exporters** (telegraf, Prometheus): the exporter pattern of
[08 §7](08-observability.md) — `mtl_instance_list_sessions`, then `mtl_session_get_info` for the
name and labels and `mtl_session_get_stats` for cumulative counters, with rates computed from the
exporter's own previous snapshot — replaces `stat_dump_cb_fn`.

### 6.3 Plugin flag parity

Every plugin-visible knob has a unified disposition `[C5 §7.9]`:

| Plugin knob today | Unified API |
|---|---|
| `ST40P_RX_FLAG_DISABLE_AUTO_DETECT` (`ecosystem/gstreamer_plugin/gst_mtl_st40p_rx.c:515`) | `mtl_anc_config.detect = MTL_DETECT_OFF` |
| ST40 test knobs as element properties (`ecosystem/gstreamer_plugin/gst_mtl_st40p_tx_test.h`; `include/st40_api.h:90-96`) | `mtl_debug_inject(…, MTL_FAULT_TX_MUTATE, …)` with `enum mtl_tx_mutation` (debug builds, `mtl_debug.h`) |
| `pts-pacing-offset` in ns (`ecosystem/gstreamer_plugin/gst_mtl_st20p_tx.c:210-217`) | `timing.media_time_offset_ns` (TAI mode) |
| ST22 `pack_type` (`ecosystem/ffmpeg_plugin/mtl_st22p_tx.c:81`) | `mtl_cvideo_config.pack_type` (`MTL_CVIDEO_PACK_CODESTREAM`; SLICE is `-MTL_ENOTSUP` in 0.1) |
| `ptp_get_time_fn = CLOCK_TAI` (`ecosystem/ffmpeg_plugin/mtl_common.c:39-45`) | `time_source = MTL_TIME_SOURCE_CLOCK_TAI` (validated), falling back to `SYSTEM_TAI` labelled ESTIMATED, with a timing warning |
| `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE`, `MTL_FLAG_TX_VIDEO_MIGRATE`, `MTL_FLAG_BIND_NUMA` (`include/mtl_api.h:340-350`) | instance flags under the merge table (03 §7.2): `BIND_NUMA` is invariant; the two legacy scheduler policies are ignored on a second acquire (unified sessions never migrate), so FFmpeg and GStreamer can share an instance |
| `MTL_FLAG_ALLOW_DOWN_PORTS` (`include/mtl_api.h:497`) | legacy only; unified sessions never prune legs and use `mtl_session_set_leg_enabled` |
| `AVFMT_FLAG_NONBLOCK` | demuxer `read_packet` only (§6.2) |
| FFmpeg st30p `pkt->pts` (ignored today) | rescaled to the sample index; gaps with `MTL_SUBMIT_DISCONTINUITY` |
| FFmpeg `GPU_DIRECT` | deferred to the device-memory domain (05); GPU pinned host memory is a supported import today |

## 7. What happens to PR #1610

Recommendation (Q-MIG-3): **keep the PR open as a reference until the new header lands, then
close it as superseded, with credit**, and cherry-pick concepts rather than commits:

| From PR #1610 | Keep as |
|---|---|
| one opaque session, media-specific create, vtable dispatch | L2 + L1 structure |
| value-backed allocation-free event ring + eventfd (`lib/src/new_api/mt_session_event.c`) | the per-session ring implementation (with the reliable/lossy split), and the proposed replacement for the pipelines' tasklet mutex/condvar in Phase 0.5, credited to the contributor |
| "a 0 return yields exactly one asynchronous completion; a negative return yields none" | the normative completion rule (07 §6) |
| `GRACEFUL_SHUTDOWN.md` (destroy as the single safe primitive, generation-tagged handles, async-signal-safe stop) | the destroy design (03 §6), now implemented rather than claimed |
| claim-then-pop bind on the app thread | the submit path |
| parity test suites, samples, RxTxApp migration shape | the test and sample plan (13, 14) — with callback-boundary tests against the real tasklet, not stubs |
| `.github` tooling edits | drop |

Rebasing is not recommended: only 7 textual conflicts, but `main` has since reworked the
pipeline logic the PR duplicates, and the public contract itself is being redesigned
`[R01 §6]`.

## 8. Where the code and headers go

| Item | Proposal | Open |
|---|---|---|
| Public headers | `include/mtl/experimental/mtl_unified.h`, `mtl_simple.h`, `mtl_debug.h` until the freeze, then `include/mtl/`; the sketch in `doc/unified-api/sketch/` is their seed | Q-ABI-5 (location decided by §2.6; the prefix stays `mtl_`) |
| Library | `libmtl_unified.so.0.<rev>`, version node `MTL_UNIFIED_EXPERIMENTAL` (§2.6) | Q-ABI-1 |
| L2 core | `lib/src/unified/` (next to `lib/src/st2110/`; not PR #1610's `new_api/`) | |
| L1 adapters | `lib/src/unified/adapters/{video,cvideo,audio,anc,fastmeta}_{tx,rx}.c` | |
| Exact time math | `lib/src/unified/mt_time_math.[ch]`, exported through the L4 helpers (`mtl_rational_index_at`, `mtl_timeline_index_at`) and the Phase 0.5 `st_timeline_*` helper | |
| Null backend | `lib/src/dev/` next to the other backends; port spec `null:<n>` (A12) | |
| Unit tests | `tests/unit/unified/…` against the null backend and the test time source, plus callback-boundary tests with the real tasklet | 13 |
| Doc test | `doc/unified-api/sketch/check.sh` now; moves to `tests/unit/unified/doc_examples.{c,cpp}` with the headers in Phase 1 and runs in CI | |
| Language baseline | formalise C11 for `lib/` (atomics already used in 13 files) while public headers stay C99/C++-clean | Q-ABI-7 |

## R4. Field map: legacy ops to `mtl_session_config` (revision 4)

Session configuration is typed: each legacy ops field becomes a field of `struct mtl_session_config` (`sc`) or `struct mtl_instance_params` (`ip`), an `MTL_OPT_*` option, a call, or nothing. New enums mirror the legacy ones: value + 1 where 0 must mean "not set", the same values where 0 is a real default.
Migrate with `MTL_INIT(&sc)`, set `sc.direction`, `sc.essence` and `sc.unit`, then copy the fields below; addresses go through `mtl_flow_ipv4()`. Tuning flags become `MTL_OPT_*` entries in `sc.options` (instance flags in `ip.options`).
`MTL_OPT_X = v` below means one `struct mtl_option` {key, scope, value} in that array; "(port i)" means scope i + 1. Leg i is legacy session port i (`MTL_SESSION_PORT_P` = 0, `MTL_SESSION_PORT_R` = 1), that is `sc.flows[i]`.

Conversion words: **same** (same value), **+1** (value + 1), **flag → option**, **callback → call**, **removed (M12)** (on the [REVISION-4 §5](REVISION-4.md#5-what-needs-your-decision) list), **LATER** (declared under `MTL_LATER`), **GAP** (no home; see R4.15).
The stats map is [08 §R4.4](08-observability.md#r44-legacy-stats-fields). Names are those of the revision-4 headers, which win over §1–§8 above.

### R4.1 Fields and flags every session ops shares

Each struct table below lists every field; a row marked "R4.1" converts as here.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `dip_addr[i]` (TX), `ip_addr[i]` (RX) | `sc.flows[i].ip` | `mtl_flow_ipv4(&sc.flows[i], a, b, c, d, udp_port[i])`; RX: a group, or the port's own address (or zero) for unicast |
| `sip_addr[i]` (RX union alias) | `sc.flows[i].ip` | removed (M12, deprecated alias of `ip_addr`) |
| `udp_port[i]` | `sc.flows[i].udp_port` | same; 0 is `-MTL_EINVAL` unless the leg is reserved (§6.2) |
| `num_port` | which `sc.flows[]` exist | 1: `flows[0]` only; 2: `flows[1]` too (ST 2022-7); a leg exists when its `udp_port` is not 0 |
| `port[i]` (BDF of the session port) | `sc.flows[i].port` | 0 = instance port i (legacy P/R order); otherwise `mtl_port_find(mt, port[i], &p)` (mtl_observe.h) and `p + 1` |
| `udp_src_port[i]` | `sc.flows[i].udp_src_port` | same (0 = `udp_port`) |
| `payload_type` | `sc.flows[i].payload_type` | same on every leg; TX 0 = essence default, RX 0 = no check |
| `ssrc` | `sc.flows[i].ssrc` | same; TX 0 = random, RX 0 = no check |
| `mcast_sip_addr[i]` | `sc.flows[i].source_filter` | same bytes (IPv4 in 0..3) |
| `tx_dst_mac[i]`, `*_TX_FLAG_USER_P_MAC`, `*_TX_FLAG_USER_R_MAC` | `sc.flows[i].dst_mac`, `sc.flows[i].flow_flags \|= MTL_FLOWF_USER_MAC` | flag → per-leg flow flag: P → leg 0, R → leg 1 |
| `name` | `sc.name` | copied into `char[MTL_NAME_MAX]`; NULL → "" (generated) |
| `priv` | — | removed: nothing calls back; `unit.cookie` returns in `mtl_tx_result.cookie`, `mtl_tx_result.session` names the stream |
| `framebuff_cnt` | `sc.pool_count` | same (0 = by essence) |
| `socket_id`, `*_FLAG_FORCE_NUMA` | `MTL_OPT_NUMA = socket_id` | flag + field → option (a node, literal) |
| `type` (`*_TYPE_FRAME_LEVEL`, `*_TYPE_RTP_LEVEL`, `ST20_TYPE_SLICE_LEVEL`) | `sc.unit` | `MTL_UNIT_FRAME`, `MTL_UNIT_PACKETS`, `MTL_UNIT_ROWS`; `sc.essence` keeps the media type (no `MTL_RTP` needed) |
| `fps` | `raster.rate` of the essence member | +1 (R4.14); interlaced: legacy `fps` counts fields, halve it (R4.14) |
| `interlaced` | `raster.scan` | `true` → `MTL_INTERLACED`, `false` → `MTL_PROGRESSIVE` (same values as the bool) |
| `get_next_frame` (TX session) | `mtl_tx_acquire` + `mtl_tx_submit` | callback → call: the app pushes; `unit.slot` replaces `frame_idx` |
| `notify_frame_done` | `mtl_tx_reap` (library pools: `sc.flags \|= MTL_SESSION_RESULTS`) | callback → result record; `mtl_tx_result.slot` and `cookie` name the frame |
| `notify_frame_late(epoch_skipped)` | `mtl_tx_result.status` = `MTL_TX_LATE` or `MTL_TX_DROPPED` + `reason`; `mtl_tx_result_full.slots_skipped_before` (mtl_observe.h) | callback → result; counters `tx.units_late`, `tx.units_dropped` |
| `notify_frame_available` | `mtl_session_get_wait_handle` or `mtl_session_wait` with `MTL_WAIT_ACQUIRE` (TX) / `MTL_WAIT_DEQUEUE` (RX) | callback → wait handle |
| `notify_frame_ready` (RX session) | `mtl_rx_dequeue`, then `mtl_rx_release` | callback → call; `*_rx_put_framebuff` → `mtl_rx_release` |
| `notify_event` | `mtl_session_read_events` (or `mtl_queue_bind(q, s, MTL_BIND_EVENTS)`) | `ST_EVENT_VSYNC` → `MTL_EVENT_EPOCH_TICK`; `ST_EVENT_RECOVERY_ERROR` → `MTL_EVENT_RECOVERY`; `ST_EVENT_FATAL_ERROR` → `MTL_EVENT_SESSION_STATE` to `MTL_STATE_ERROR` |
| `rtp_ring_size` (TX, `type` RTP) | `sc.pool_count` × `sc.packet.packets_per_chunk` | packets → chunks of packet slots |
| `rtp_ring_size` (RX, `type` RTP) | `sc.packet.rx_ring_packets` | same (0 = 512) |
| `notify_rtp_done` | `mtl_tx_reap` (one result per chunk) or `MTL_WAIT_ACQUIRE` | callback → call |
| `notify_rtp_ready` | `mtl_rx_dequeue` (a chunk of 1..N packets) or `MTL_WAIT_DEQUEUE` | callback → call |
| `rtp_timestamp_delta_us` | `sc.media_time_offset_ns` | × 1000 |
| `*_TX_FLAG_USER_PACING` | `sc.media_mode = MTL_MEDIA_TAI` + `unit.media_tai_ns`, or per unit `MTL_SUBMIT_NOT_BEFORE` + `unit.launch_tai_ns` | flag → mode; the legacy TAI timestamp moves into the unit; TAI media time is snapped to the grid (`MTL_OPT_SNAP_MODE`) |
| `*_TX_FLAG_EXACT_USER_PACING` | per unit `MTL_SUBMIT_EXACT` + `unit.launch_tai_ns` | flag → per-unit flag; reported with `MTL_INFO_NON_COMPLIANT` |
| `*_TX_FLAG_USER_TIMESTAMP` | `ST10_TIMESTAMP_FMT_TAI`: `unit.media_tai_ns` (RTP = floor(M × rate)); `ST10_TIMESTAMP_FMT_MEDIA_CLK`: `MTL_SUBMIT_RTP_TS` + `unit.rtp` | flag → per-unit field; needs `sc.media_mode` INDEX or TAI (`MTL_REASON_RTP_TS_AUTO` otherwise) |
| `*P_TX_FLAG_DROP_WHEN_LATE` | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` | flag → option; without it, `MTL_LATE_RESLOT` keeps the legacy slip (TAI and INDEX default to DROP) |
| `*_FLAG_ENABLE_VSYNC` | `MTL_OPT_EPOCH_TICK = 1` | flag → option; `MTL_EVENT_EPOCH_TICK` per unit period |
| `*P_*_FLAG_BLOCK_GET`, `*_set_block_timeout()` | `timeout_ns` of `mtl_tx_acquire` / `mtl_rx_dequeue` | flag → argument (no flag = 0); `*_wake_block()` → `mtl_session_interrupt(s, 1)` |
| `*_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` | `MTL_OPT_RX_INCOMPLETE` | the unified default delivers with `unit.status = MTL_RX_INCOMPLETE`; without the legacy flag set `MTL_RX_DISCARD` (M13) |
| `*_RX_FLAG_DATA_PATH_ONLY` | — | removed (M12, queue meta) |
| `*_RX_FLAG_SIMULATE_PKT_LOSS`, `rtcp.burst_loss_max`, `rtcp.sim_loss_rate` | `mtl_debug_inject(obj, MTL_FAULT_DROP_PKTS, &p)` with `drop_every`, `drop_count`, `drop_offset` | debug builds; a pattern, not a random rate: GAP for rate and burst |
| `ST20*_FLAG_ENABLE_RTCP`, `ST22*_FLAG_ENABLE_RTCP` | `MTL_OPT_RTX = 1` | flag → option (MTL's NACK retransmission); `rtcp` members in R4.4 (TX) and R4.5 (RX) |
| `ST30*`, `ST40*`, `ST41*` `_FLAG_ENABLE_RTCP` | — | removed (M12: the RTCP flags on audio, ANC and fastmeta do nothing today) |
| `*_TX_FLAG_DEDICATE_QUEUE` | `MTL_OPT_TX_QUEUE = MTL_TXQ_DEDICATED` | flag → option |

### R4.2 `mtl_init_params`, `mtl_port_init_params`

`mtl_init()` → `mtl_instance_open(&ip, &mt)`; `ip.ports` points at `port_count` entries of `struct mtl_port_spec`. Port arrays index by legacy port i.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `port[i]` | `ports[i].name` | same string; the PMD is the name prefix (`pmd` below) |
| `port_packet_loss[i].tx_stream_loss_id`, `.tx_stream_loss_divider` | `mtl_debug_inject(MTL_OBJ_OF_SESSION(s), MTL_FAULT_DROP_PKTS, &p)`, `p.leg`, `p.drop_every`, `p.drop_offset` | debug; instance-wide → per session and leg |
| `num_ports` | `ip.port_count` | same |
| `net_proto[i]` | DPDK: `MTL_OPT_DHCP = 1` (port i); kernel backends: `ports[i].sip` all zero | `MTL_PROTO_STATIC` = default; `MTL_PROTO_DHCP` → option |
| `pmd[i]` | `ports[i].name` prefix | R4.14 (`mtl_pmd_type`) |
| `tx_queues_cnt[i]`, `rx_queues_cnt[i]` | `ports[i].tx_queues`, `ports[i].rx_queues` | same (0 = auto) |
| `sip_addr[i]` | `ports[i].sip` | same bytes 0..3 (`ip_family` 0) |
| `netmask[i]` | `ports[i].prefix_len` | mask → prefix length (255.255.255.0 → 24; 0 = 24) |
| `gateway[i]` | `ports[i].gateway` | same bytes |
| `flags` | `ip.flags` and options | bit by bit in R4.3 |
| `priv` | — | removed (no instance callbacks) |
| `log_level` | `MTL_OPT_LOG_LEVEL` | +1 (`MTL_LOG_LEVEL_DEBUG` 0 → `MTL_LOG_DEBUG` 1 … `CRIT` 5 → 6) |
| `lcores` | `ip.lcores` | same string |
| `dma_dev_port[]`, `num_dma_dev_port` | `MTL_OPT_DMA_DEVICES` | array → one comma-separated string |
| `rss_mode` | `MTL_OPT_RSS_MODE` | +1 (`NONE` 0 → `MTL_RSS_NONE` 1, `L3` → 2, `L3_L4` → 3) |
| `iova_mode` | `MTL_OPT_IOVA_MODE` | `AUTO` = absent; `VA` 1, `PA` 2: same |
| `nb_tx_desc`, `nb_rx_desc` | `MTL_OPT_TX_DESC`, `MTL_OPT_RX_DESC` | same; instance-wide = scope 0 |
| `ptp_get_time_fn` | `ip.time_source = MTL_TIME_SOURCE_USER` + `mtl_time_user_update()` (mtl_sync.h) | callback → call (pull → push); a `CLOCK_TAI` reader → `MTL_TIME_SOURCE_CLOCK_TAI` |
| `ptp_sync_notify` | stats `time.offset_ns`, `time.utc_offset_s`, `time.last_sync_age_ns`; `MTL_EVENT_TIME_STATE`, `MTL_EVENT_TIME_STEP` | callback per sync → polled stats and events |
| `dump_period_s` | `MTL_OPT_STAT_DUMP_S` | same |
| `stat_dump_cb_fn` | `mtl_stat_list` + `mtl_stat_read` (mtl_observe.h) | callback → the application's own loop |
| `pacing` | `MTL_OPT_PACING` in each session's options | instance-wide → per session; values in R4.14 |
| `data_quota_mbs_per_sch` | `MTL_OPT_SCHED_QUOTA_MBS` | same |
| `tx_audio_sessions_max_per_sch`, `rx_audio_sessions_max_per_sch` | `MTL_OPT_SCHED_TX_AUDIO_MAX`, `MTL_OPT_SCHED_RX_AUDIO_MAX` | same |
| `pkt_udp_suggest_max_size` | `MTL_OPT_MAX_UDP_PAYLOAD` in each session's options | instance-wide → per session |
| `nb_rx_hdr_split_queues` | — | removed (M12, header split) |
| `rx_pool_data_size` | `MTL_OPT_RX_POOL_DATA_SIZE` | same |
| `memzone_max` | `MTL_OPT_MEMZONE_MAX` | same |
| `tasklets_nb_per_sch` | `MTL_OPT_TASKLETS_PER_SCHED` | same |
| `arp_timeout_s` | `MTL_OPT_ARP_TIMEOUT_S` | same |
| `rss_sch_nb[i]` | `MTL_OPT_RSS_SCHEDS` (port i) | same |
| `kp`, `ki` | `MTL_OPT_PTP_PI_KP`, `MTL_OPT_PTP_PI_KI` | double → integer × 1e9 |
| `port_params[i]` | rows below | — |
| `main_lcore` | `MTL_OPT_MAIN_LCORE` | same |
| `tx_sessions_cnt_max`, `rx_sessions_cnt_max` | — | removed (M12, deprecated) |
| `port_params[i].flags` `MTL_PORT_FLAG_FORCE_NUMA` (bit 0) | `ports[i].numa` | flag + `socket_id` → `numa = socket_id + 1` |
| `port_params[i].flags` `MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION` (bit 1) | — | removed: the default; open never waits for links (`mtl_instance_get_health`) |
| `port_params[i].socket_id` | `ports[i].numa` | + 1 (0 = the device's socket) |
| `port_params[i].rl_burst_size` | `MTL_OPT_RL_BURST` (port i) | same |

### R4.3 `enum mtl_init_flag` bit by bit

| Legacy flag (bit) | New field or option | Conversion |
|---|---|---|
| `MTL_FLAG_BIND_NUMA` (0) | — | removed (M12, no effect) |
| `MTL_FLAG_PTP_ENABLE` (1) | `ip.time_source = MTL_TIME_SOURCE_PTP_BUILTIN` | flag → typed field |
| `MTL_FLAG_RX_SEPARATE_VIDEO_LCORE` (2) | `MTL_OPT_RX_SEPARATE_VIDEO_LCORE = 1` | flag → option |
| `MTL_FLAG_TX_VIDEO_MIGRATE` (3) | `MTL_OPT_TX_VIDEO_MIGRATE = 1` | flag → option |
| `MTL_FLAG_RX_VIDEO_MIGRATE` (4) | `MTL_OPT_RX_VIDEO_MIGRATE = 1` | flag → option |
| `MTL_FLAG_TASKLET_THREAD` (5) | `MTL_INSTANCE_TASKLET_THREAD` (0x4) | flag → flag |
| `MTL_FLAG_TASKLET_SLEEP` (6) | `MTL_INSTANCE_TASKLET_SLEEP` (0x8) | flag → flag |
| `MTL_FLAG_RXTX_SIMD_512` (7) | `MTL_OPT_SIMD_512 = 1` | flag → option |
| `MTL_FLAG_ENABLE_HW_TIMESTAMP` (8) | `MTL_INSTANCE_HW_TIMESTAMP` (0x10) | flag → flag |
| `MTL_FLAG_PTP_PI` (9) | `MTL_OPT_PTP_PI = 1` | flag → option |
| `MTL_FLAG_UDP_LCORE` (10) | — | removed (M12, UDP transport remnants) |
| `MTL_FLAG_RANDOM_SRC_PORT` (11) | `MTL_OPT_SRC_PORT_MODE = MTL_SRC_PORT_RANDOM` | flag → option, per session |
| `MTL_FLAG_MULTI_SRC_PORT` (12) | `MTL_OPT_SRC_PORT_MODE = MTL_SRC_PORT_MULTI` | flag → option, per session |
| `MTL_FLAG_SHARED_TX_QUEUE` (13) | `MTL_OPT_SHARED_TX_QUEUE = 1` | flag → option (port default; `MTL_OPT_TX_QUEUE` per session wins) |
| `MTL_FLAG_SHARED_RX_QUEUE` (14) | `MTL_OPT_SHARED_RX_QUEUE = 1` | flag → option |
| `MTL_FLAG_PHC2SYS_ENABLE` (15) | `MTL_OPT_PHC2SYS = 1` | flag → option |
| `MTL_FLAG_VIRTIO_USER` (16) | `MTL_OPT_VIRTIO_USER = 1` | flag → option |
| `MTL_FLAG_DEV_AUTO_START_STOP` (17) | — | removed: the default; open starts the devices, close stops them (no `mtl_start`) |
| `MTL_FLAG_ALLOW_ACROSS_NUMA_CORE` (18) | `MTL_OPT_ACROSS_NUMA_CORES = 1` | flag → option |
| `MTL_FLAG_NO_MULTICAST` (19) | `MTL_OPT_NO_MULTICAST = 1` | flag → option |
| `MTL_FLAG_DEDICATED_SYS_LCORE` (20) | `MTL_OPT_SYS_LCORE = MTL_SYS_DEDICATED` | flag → option |
| `MTL_FLAG_NOT_BIND_NUMA` (21) | `MTL_OPT_NO_BIND_NUMA = 1` | flag → option |
| `MTL_FLAG_CNI_THREAD` (32) | `MTL_OPT_CNI = MTL_CNI_THREAD` | flag → option |
| `MTL_FLAG_CNI_TASKLET` (33) | `MTL_OPT_CNI = MTL_CNI_TASKLET` | flag → option |
| `MTL_FLAG_NIC_RX_PROMISCUOUS` (34) | `MTL_OPT_PROMISCUOUS = 1` | flag → option |
| `MTL_FLAG_PTP_UNICAST_ADDR` (35) | `MTL_OPT_PTP_UNICAST = 1` | flag → option |
| `MTL_FLAG_RX_MONO_POOL` (36) | `MTL_OPT_RX_MONO_POOL = 1` | flag → option |
| `MTL_FLAG_TASKLET_TIME_MEASURE` (38) | `MTL_OPT_TASKLET_TIME_MEASURE = 1` | flag → option |
| `MTL_FLAG_AF_XDP_ZC_DISABLE` (39) | `MTL_OPT_AF_XDP_COPY = 1` | flag → option |
| `MTL_FLAG_TX_MONO_POOL` (40) | `MTL_OPT_TX_MONO_POOL = 1` | flag → option |
| `MTL_FLAG_DISABLE_SYSTEM_RX_QUEUES` (41) | `MTL_OPT_NO_SYSTEM_RX_QUEUES = 1` | flag → option |
| `MTL_FLAG_PTP_SOURCE_TSC` (42) | `MTL_OPT_PTP_SOURCE_TSC = 1` | flag → option |
| `MTL_FLAG_TX_NO_CHAIN` (43) | `MTL_OPT_TX_COPY = 1` | flag → option, per session (payload copied into the packet) |
| `MTL_FLAG_TX_NO_BURST_CHK` (44) | `MTL_OPT_TX_NO_BURST_CHECK = 1` | flag → option |
| `MTL_FLAG_RX_USE_CNI` (45) | `MTL_OPT_RX_USE_CNI = 1` | flag → option |
| `MTL_FLAG_RX_UDP_PORT_ONLY` (46) | `MTL_OPT_RX_UDP_PORT_ONLY = 1` | flag → option |
| `MTL_FLAG_NOT_BIND_PROCESS_NUMA` (47) | `MTL_OPT_NO_BIND_PROCESS_NUMA = 1` | flag → option |
| `MTL_FLAG_ALLOW_DOWN_PORTS` (48) | — | removed: the default (as `MTL_PORT_FLAG_ALLOW_DOWN_INITIALIZATION`) |
| `MTL_FLAG_REDUNDANT_SIMULATE_PACKET_LOSS` (63) | `mtl_debug_inject(…, MTL_FAULT_DROP_PKTS, …)` | debug builds (with `port_packet_loss`) |

### R4.4 `st20p_tx_ops`, `ST20P_TX_FLAG_*`

`st20p_tx_create()` → `mtl_session_open()` (or `mtl_session_create()` + `mtl_session_start()`) with `sc.direction = MTL_TX`, `sc.essence = MTL_VIDEO`, `sc.unit = MTL_UNIT_FRAME`. `v` = `sc.video`.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `port` (`struct st_tx_port`: `dip_addr`, `port`, `num_port`, `udp_port`, `payload_type`, `udp_src_port`, `ssrc`) | `sc.flows[]` | R4.1 |
| `width`, `height` | `v.raster.width`, `v.raster.height` | same |
| `fps`, `interlaced` | `v.raster.rate`, `v.raster.scan` | R4.1 |
| `input_fmt` | `v.app_format` | +1 (R4.14); 0 when it is the transport layout itself (no conversion) |
| `transport_pacing` | `v.sender_type` | same |
| `transport_packing` | `v.packing` | same |
| `transport_fmt` | `v.format` | +1 |
| `device` | `MTL_OPT_VIDEO_CONVERT_DEVICE` | R4.14 (`st_plugin_device`) |
| `framebuff_cnt`, `name`, `priv`, `socket_id` | `sc.pool_count`, `sc.name`, —, `MTL_OPT_NUMA` | R4.1 |
| `flags` | rows below | — |
| `notify_frame_available`, `notify_frame_done`, `notify_frame_late`, `notify_event` | wait handle, `mtl_tx_reap`, results, `mtl_session_read_events` | R4.1 |
| `transport_linesize` | `mtl_attach.stride[0]` (mtl_mem.h) | attached pools only; a library pool has the natural stride: GAP |
| `rtcp.buffer_size` | `MTL_OPT_RTX_BUFFER_PKTS` | same (with `MTL_OPT_RTX = 1`) |
| `tx_dst_mac` | `sc.flows[i].dst_mac` | R4.1 |
| `start_vrx` | `MTL_OPT_VIDEO_START_VRX` | same |
| `pad_interval` | `MTL_OPT_VIDEO_PAD_INTERVAL` | same |
| `rtp_timestamp_delta_us` | `sc.media_time_offset_ns` | × 1000 |
| `tx_hang_detect_ms` | `MTL_OPT_TX_HANG_DETECT_NS` | × 1000000 |
| `ST20P_TX_FLAG_USER_P_MAC` (0), `ST20P_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST20P_TX_FLAG_EXT_FRAME` (2), `st20p_tx_put_ext_frame()` | `sc.flags \|= MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach()`, then `mtl_tx_acquire_slot()` | fixed app slots; an arbitrary address per frame is `mtl_tx_acquire_layout()` (LATER) |
| `ST20P_TX_FLAG_USER_PACING` (3), `ST20P_TX_FLAG_USER_TIMESTAMP` (4) | `sc.media_mode`, `unit.*` | R4.1 |
| `ST20P_TX_FLAG_ENABLE_VSYNC` (5) | `MTL_OPT_EPOCH_TICK = 1` | R4.1 |
| `ST20P_TX_FLAG_ENABLE_STATIC_PAD_P` (6) | `MTL_OPT_VIDEO_STATIC_PAD_P = 1` | flag → option |
| `ST20P_TX_FLAG_ENABLE_RTCP` (7) | `MTL_OPT_RTX = 1` | R4.1 |
| `ST20P_TX_FLAG_EXACT_USER_PACING` (8) | `MTL_SUBMIT_EXACT` | R4.1 |
| `ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH` (9) | — | removed: the unified default RTP is the epoch (D-10); the legacy default (TR offset included) is approximated with `sc.media_time_offset_ns` (M13) |
| `ST20P_TX_FLAG_DISABLE_BULK` (10) | `MTL_OPT_VIDEO_DISABLE_BULK = 1` | flag → option |
| `ST20P_TX_FLAG_FORCE_NUMA` (11) | `MTL_OPT_NUMA` | R4.1 |
| `ST20P_TX_FLAG_DROP_WHEN_LATE` (12) | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` | R4.1 |
| `ST20P_TX_FLAG_EXT_FRAME_MANUAL_RELEASE` (13), `st20p_tx_notify_ext_frame_free()` | — | removed: the default for app memory (mtl_mem.h M3: a slot is reused only after its result is reaped) |
| `ST20P_TX_FLAG_BLOCK_GET` (15) | `timeout_ns` of `mtl_tx_acquire` | R4.1 |

### R4.5 `st20p_rx_ops`, `ST20P_RX_FLAG_*`

`sc.direction = MTL_RX`, `sc.essence = MTL_VIDEO`, `sc.unit = MTL_UNIT_FRAME`.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `port` (`struct st_rx_port`: `ip_addr` / `sip_addr`, `num_port`, `port`, `udp_port`, `payload_type`, `ssrc`, `mcast_sip_addr`) | `sc.flows[]` | R4.1 |
| `width`, `height`, `fps`, `interlaced` | `v.raster` | as R4.4 |
| `transport_fmt` | `v.format` | +1 |
| `output_fmt` | `v.app_format` | +1; 0 = no conversion |
| `device` | `MTL_OPT_VIDEO_CONVERT_DEVICE` | R4.14 |
| `framebuff_cnt`, `name`, `priv`, `socket_id` | R4.1 | R4.1 |
| `flags` | rows below | — |
| `rx_burst_size` | `MTL_OPT_RX_BURST` | same |
| `notify_frame_available`, `notify_event` | wait handle, events | R4.1 |
| `transport_linesize` | `mtl_attach.stride[0]` | as R4.4 (GAP for library pools) |
| `ext_frames` (`struct st_ext_frame[]`: `addr[]`, `iova[]`, `linesize[]`, `size`, `opaque`) | `mtl_session_attach()` with `count`, `slot_offset[]`, `plane_offset[]`, `stride[]` | array → one attach; `iova` dropped (MTL maps the region); `opaque` → the app's own table by `unit.slot` |
| `rtcp` (`nack_interval_us`, `seq_bitmap_size`, `seq_skip_window`, `burst_loss_max`, `sim_loss_rate`) | `MTL_OPT_RTX_NACK_INTERVAL_US`, `MTL_OPT_RTX_SEQ_BITMAP`, `MTL_OPT_RTX_SEQ_SKIP`; the last two: debug | same; loss simulation: R4.1 |
| `query_ext_frame` | `mtl_rx_provide()` | LATER (per-unit RX destinations) |
| `notify_detected` (meta: `width`, `height`, `fps`, `packing`, `interlaced`; reply: `slice_lines`, `uframe_size`) | `MTL_EVENT_RX_FORMAT` + stats `rx.detected.*` | callback → event; no detected packing key: GAP; reply `slice_lines` → `MTL_OPT_RX_ROWS_STEP`, `uframe_size` removed (M12) |
| `gpu_context` | `mtl_mem_import_device()` | LATER (S1 U-417 lists the raw GPU field as a cut) |
| `ST20P_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |
| `ST20P_RX_FLAG_ENABLE_VSYNC` (1) | `MTL_OPT_EPOCH_TICK = 1` | R4.1 |
| `ST20P_RX_FLAG_EXT_FRAME` (2) | with `query_ext_frame`: `mtl_rx_provide()` | LATER; fixed frames: `MTL_SESSION_POOL_ATTACHED` + attach |
| `ST20P_RX_FLAG_PKT_CONVERT` (3) | `MTL_OPT_RX_CONVERT_PER_PACKET = 1` | flag → option (M12 keeps it as this option) |
| `ST20P_RX_FLAG_ENABLE_RTCP` (4) | `MTL_OPT_RTX = 1` | R4.1 |
| `ST20P_RX_FLAG_SIMULATE_PKT_LOSS` (5) | `MTL_FAULT_DROP_PKTS` | R4.1 |
| `ST20P_RX_FLAG_FORCE_NUMA` (6) | `MTL_OPT_NUMA` | R4.1 |
| `ST20P_RX_FLAG_BLOCK_GET` (15) | `timeout_ns` of `mtl_rx_dequeue` | R4.1 |
| `ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (16) | `MTL_OPT_RX_INCOMPLETE` | R4.1 (inverted default) |
| `ST20P_RX_FLAG_DMA_OFFLOAD` (17) | `MTL_OPT_DMA = MTL_REQ_PREFER` | flag → option (falls back to the CPU, as today) |
| `ST20P_RX_FLAG_AUTO_DETECT` (18) | `v.detect = MTL_DETECT_ON` | flag → typed field |
| `ST20P_RX_FLAG_HDR_SPLIT` (19) | — | removed (M12, header split) |
| `ST20P_RX_FLAG_DISABLE_MIGRATE` (20) | — | GAP (R4.15) |
| `ST20P_RX_FLAG_TIMING_PARSER_STAT` (21) | `MTL_OPT_RX_TIMING_PARSER = 1` | flag → option; stats `tp.*` |
| `ST20P_RX_FLAG_TIMING_PARSER_META` (22) | `MTL_OPT_RX_TIMING_PARSER = 1` + `mtl_rx_get_timing(s, lease, leg, …)` | flag → option + per-unit call (mtl_observe.h) |
| `ST20P_RX_FLAG_USE_MULTI_THREADS` (23) | `MTL_OPT_RX_THREADS = 2` | flag → option |
| `ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS` (24) | `mtl_mem_import_device()` | LATER |

### R4.6 `st20_tx_ops`, `ST20_TX_FLAG_*`

`st20_tx_create()` → `mtl_session_open()`; `sc.essence = MTL_VIDEO`, `v.app_format = 0`. `type` picks `sc.unit`: FRAME_LEVEL → `MTL_UNIT_FRAME`, SLICE_LEVEL → `MTL_UNIT_ROWS`, RTP_LEVEL → `MTL_UNIT_PACKETS` (mtl_packet.h).

| Legacy field | New field or option | Conversion |
|---|---|---|
| `dip_addr`, `port`, `num_port`, `udp_port`, `payload_type`, `ssrc`, `udp_src_port`, `tx_dst_mac` | `sc.flows[]` | R4.1 |
| `pacing` | `v.sender_type` | same |
| `type` | `sc.unit` | R4.1 |
| `packing` | `v.packing` | same |
| `width`, `height`, `fps`, `interlaced` | `v.raster` | as R4.4 |
| `fmt` | `v.format` | +1 |
| `name`, `priv`, `framebuff_cnt`, `socket_id` | R4.1 | R4.1 |
| `flags` | rows below | — |
| `get_next_frame`, `notify_frame_done`, `notify_frame_late`, `notify_event` | `mtl_tx_acquire` + `mtl_tx_submit`, `mtl_tx_reap`, results, events | R4.1 |
| `rtcp.buffer_size` | `MTL_OPT_RTX_BUFFER_PKTS` | same |
| `linesize` | `mtl_attach.stride[0]` | as R4.4 (GAP for library pools) |
| `start_vrx`, `pad_interval` | `MTL_OPT_VIDEO_START_VRX`, `MTL_OPT_VIDEO_PAD_INTERVAL` | same |
| `rtp_timestamp_delta_us` | `sc.media_time_offset_ns` | × 1000 |
| `tx_hang_detect_ms` | `MTL_OPT_TX_HANG_DETECT_NS` | × 1000000 |
| `query_frame_lines_ready` (slice) | `mtl_tx_submit()` of the same lease again with a larger `unit.used` (rows) | callback → call; deadlines: `mtl_tx_row_deadline()`; late rows: `MTL_OPT_ROWS_LATE` |
| `rtp_ring_size` | `sc.pool_count` × `sc.packet.packets_per_chunk` | R4.1 |
| `rtp_frame_total_pkts` | `sc.packet.packets_per_unit` | same; the chunk that ends a frame carries `MTL_SUBMIT_UNIT_END` |
| `rtp_pkt_size` | `sc.packet.slot_bytes` | same (bytes from the RTP header on) |
| `notify_rtp_done` | `mtl_tx_reap` | R4.1 |
| RTP header writes of RTP_LEVEL (implicit) | `sc.packet.set_fields` | the legacy session rewrites the RTP timestamp: `MTL_PKT_SET_TIMESTAMP`; with `USER_TIMESTAMP`: 0 (verbatim) |
| `ST20_TX_FLAG_USER_P_MAC` (0), `ST20_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST20_TX_FLAG_EXT_FRAME` (2), `st20_tx_set_ext_frame()` | `MTL_SESSION_POOL_ATTACHED` + `mtl_session_attach()` + `mtl_tx_acquire_slot()` | as R4.4 |
| `ST20_TX_FLAG_USER_PACING` (3), `ST20_TX_FLAG_USER_TIMESTAMP` (4), `ST20_TX_FLAG_EXACT_USER_PACING` (8) | `sc.media_mode`, `MTL_SUBMIT_*` | R4.1 |
| `ST20_TX_FLAG_ENABLE_VSYNC` (5) | `MTL_OPT_EPOCH_TICK = 1` | R4.1 |
| `ST20_TX_FLAG_ENABLE_STATIC_PAD_P` (6) | `MTL_OPT_VIDEO_STATIC_PAD_P = 1` | flag → option |
| `ST20_TX_FLAG_ENABLE_RTCP` (7) | `MTL_OPT_RTX = 1` | R4.1 |
| `ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH` (9) | — | removed: the default (as R4.4) |
| `ST20_TX_FLAG_DISABLE_BULK` (10) | `MTL_OPT_VIDEO_DISABLE_BULK = 1` | flag → option |
| `ST20_TX_FLAG_FORCE_NUMA` (11) | `MTL_OPT_NUMA` | R4.1 |

### R4.7 `st20_rx_ops`, `ST20_RX_FLAG_*`

| Legacy field | New field or option | Conversion |
|---|---|---|
| `ip_addr`, `sip_addr`, `num_port`, `port`, `udp_port`, `payload_type`, `mcast_sip_addr`, `ssrc` | `sc.flows[]` | R4.1 |
| `type` | `sc.unit` | R4.1 (SLICE_LEVEL → `MTL_UNIT_ROWS`) |
| `width`, `height`, `fps`, `interlaced` | `v.raster` | as R4.4 |
| `fmt` | `v.format` | +1 |
| `pacing`, `packing` | — | removed (M12, "not in use now") |
| `name`, `priv`, `framebuff_cnt`, `socket_id` | R4.1 | R4.1 |
| `flags` | rows below | — |
| `rx_burst_size` | `MTL_OPT_RX_BURST` | same |
| `notify_frame_ready`, `notify_event` | `mtl_rx_dequeue` + `mtl_rx_release`, events | R4.1 |
| `ext_frames` (`struct st20_ext_frame[]`: `buf_addr`, `buf_iova`, `buf_len`, `opaque`) | `mtl_session_attach()` | as R4.5 |
| `rtcp` (5 members) | `MTL_OPT_RTX_*` | as R4.5 |
| `linesize` | `mtl_attach.stride[0]` | as R4.4 (GAP for library pools) |
| `uframe_size`, `uframe_pg_callback` | — | removed (M12: application code per pixel group on the tasklet) |
| `notify_detected` | `MTL_EVENT_RX_FORMAT` | as R4.5 |
| `query_ext_frame` | `mtl_rx_provide()` | LATER |
| `slice_lines` | `MTL_OPT_RX_ROWS_STEP` | same (rows between wake-ups) |
| `notify_slice_ready` | `mtl_rx_dequeue()` (unit with `MTL_UNITF_PARTIAL`), then `mtl_rx_wait_rows()` (mtl_sync.h) | callback → call |
| `rtp_ring_size`, `notify_rtp_ready` | `sc.packet.rx_ring_packets`, `mtl_rx_dequeue` | R4.1 (`sc.unit = MTL_UNIT_PACKETS`) |
| `gpu_direct_framebuffer_in_vram_device_address`, `gpu_context` | `mtl_mem_import_device()` | LATER (S1 U-417) |
| `ST20_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |
| `ST20_RX_FLAG_ENABLE_VSYNC` (1) | `MTL_OPT_EPOCH_TICK = 1` | R4.1 |
| `ST20_RX_FLAG_ENABLE_RTCP` (2) | `MTL_OPT_RTX = 1` | R4.1 |
| `ST20_RX_FLAG_SIMULATE_PKT_LOSS` (3) | `MTL_FAULT_DROP_PKTS` | R4.1 |
| `ST20_RX_FLAG_FORCE_NUMA` (4) | `MTL_OPT_NUMA` | R4.1 |
| `ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (16) | `MTL_OPT_RX_INCOMPLETE` | R4.1 |
| `ST20_RX_FLAG_DMA_OFFLOAD` (17) | `MTL_OPT_DMA = MTL_REQ_PREFER` | flag → option |
| `ST20_RX_FLAG_AUTO_DETECT` (18) | `v.detect = MTL_DETECT_ON` | flag → typed field |
| `ST20_RX_FLAG_HDR_SPLIT` (19) | — | removed (M12) |
| `ST20_RX_FLAG_DISABLE_MIGRATE` (20) | — | GAP (R4.15) |
| `ST20_RX_FLAG_TIMING_PARSER_STAT` (21), `ST20_RX_FLAG_TIMING_PARSER_META` (22) | `MTL_OPT_RX_TIMING_PARSER = 1` (+ `mtl_rx_get_timing`) | as R4.5 |
| `ST20_RX_FLAG_USE_MULTI_THREADS` (23) | `MTL_OPT_RX_THREADS = 2` | flag → option |

### R4.8 `st22p_tx_ops`, `st22p_rx_ops`, `ST22P_*_FLAG_*`

`sc.essence = MTL_CVIDEO`, `sc.unit = MTL_UNIT_FRAME`; `c` = `sc.cvideo`. A codec plugin runs when `c.app_format` is not 0.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `port` (`st_tx_port` / `st_rx_port`, all members) | `sc.flows[]` | R4.1 |
| `width`, `height`, `fps`, `interlaced` | `c.raster` | as R4.4 |
| `input_fmt` (TX), `output_fmt` (RX) | `c.app_format` | +1 for a raw format (the plugin's input or output); a codestream format (56–60) → 0, the app gives or takes the codestream |
| `pack_type` | `MTL_OPT_CVIDEO_PACK` | +1 (`ST22_PACK_CODESTREAM` 0 → `MTL_CVIDEO_PACK_CODESTREAM` 1, `SLICE` → 2, not yet) |
| `codec` | `c.codec`, `c.rate_mode` | R4.14 (`st22_codec`) |
| `device` | `MTL_OPT_CVIDEO_DEVICE` | R4.14 (`st_plugin_device`) |
| `quality` (TX) | `MTL_OPT_CVIDEO_QUALITY` | same values passed to the plugin (`mtl_plugin_session_req.quality`); no named values: GAP |
| `codestream_size` (TX), `max_codestream_size` (RX) | `c.codestream_bytes` | same (per field when interlaced: a unit is a field); RX 0 → required now |
| `framebuff_cnt`, `name`, `priv`, `socket_id` | R4.1 | R4.1 |
| `flags` | rows below | — |
| `codec_thread_cnt` | `MTL_OPT_CVIDEO_THREADS` | same |
| `notify_frame_available`, `notify_frame_done` (TX), `notify_frame_late` (TX), `notify_event` | R4.1 | R4.1 |
| `rtcp` (TX `buffer_size`; RX 5 members) | `MTL_OPT_RTX_*` | as R4.4 / R4.5 |
| `tx_dst_mac` (TX) | `sc.flows[i].dst_mac` | R4.1 |
| `query_ext_frame` (RX) | `mtl_rx_provide()` | LATER |
| `ST22P_TX_FLAG_USER_P_MAC` (0), `ST22P_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST22P_TX_FLAG_DISABLE_BOXES` (2), `ST22P_RX_FLAG_DISABLE_BOXES` (6) | — | removed (M12) |
| `ST22P_TX_FLAG_USER_PACING` (3), `ST22P_TX_FLAG_USER_TIMESTAMP` (4) | `sc.media_mode`, `unit.*` | R4.1 |
| `ST22P_TX_FLAG_ENABLE_VSYNC` (5), `ST22P_RX_FLAG_ENABLE_VSYNC` (1) | `MTL_OPT_EPOCH_TICK = 1` | R4.1 |
| `ST22P_TX_FLAG_ENABLE_RTCP` (6), `ST22P_RX_FLAG_ENABLE_RTCP` (2) | `MTL_OPT_RTX = 1` | R4.1 |
| `ST22P_TX_FLAG_DISABLE_BULK` (7) | `MTL_OPT_CVIDEO_DISABLE_BULK = 1` | flag → option |
| `ST22P_TX_FLAG_EXT_FRAME` (8), `ST22P_RX_FLAG_EXT_FRAME` (4) | attached pool / `mtl_rx_provide()` | as R4.4 / R4.5 |
| `ST22P_TX_FLAG_FORCE_NUMA` (9), `ST22P_RX_FLAG_FORCE_NUMA` (5) | `MTL_OPT_NUMA` | R4.1 |
| `ST22P_TX_FLAG_DROP_WHEN_LATE` (12) | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` | R4.1 |
| `ST22P_TX_FLAG_BLOCK_GET` (15), `ST22P_RX_FLAG_BLOCK_GET` (15) | `timeout_ns` | R4.1 |
| `ST22P_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |
| `ST22P_RX_FLAG_SIMULATE_PKT_LOSS` (3) | `MTL_FAULT_DROP_PKTS` | R4.1 |
| `ST22P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (16) | `MTL_OPT_RX_INCOMPLETE` | R4.1 |

### R4.9 `st22_tx_ops`, `st22_rx_ops`, `ST22_*_FLAG_*`

`sc.essence = MTL_CVIDEO`, `c.codec = MTL_CODEC_JPEGXS` (implied today: RFC 9134), `c.app_format = 0`. The legacy session sends what `get_next_frame` sizes: `c.rate_mode = MTL_CVIDEO_VBR_MAX` keeps that on the wire; the unified default CBR pads each unit (M13).

| Legacy field | New field or option | Conversion |
|---|---|---|
| `dip_addr` / `ip_addr`, `sip_addr`, `port`, `num_port`, `udp_port`, `payload_type`, `ssrc`, `mcast_sip_addr` (RX), `udp_src_port` (TX), `tx_dst_mac` (TX) | `sc.flows[]` | R4.1 |
| `pacing` (TX) | `c.sender_type` | same |
| `pacing` (RX) | — | removed (M12, RX pacing is not read) |
| `type` | `sc.unit` | R4.1 |
| `pack_type` | `MTL_OPT_CVIDEO_PACK` | +1 |
| `width`, `height`, `fps`, `interlaced` | `c.raster` | as R4.4 |
| `fmt` | — | GAP: no sampling/depth in `mtl_cvideo_config` (today's library overwrites it with `ST20_FMT_YUV_422_10BIT`) |
| `name`, `priv`, `flags`, `framebuff_cnt`, `socket_id` | R4.1 / rows below | R4.1 |
| `framebuff_max_size` | `c.codestream_bytes` | same; per unit `unit.used` = the real size |
| `get_next_frame`, `notify_frame_done`, `notify_frame_late`, `notify_frame_ready`, `notify_event` | R4.1 | R4.1 |
| `rtcp` | `MTL_OPT_RTX_*` | as R4.4 / R4.5 |
| `rtp_ring_size`, `rtp_frame_total_pkts`, `rtp_pkt_size`, `notify_rtp_done`, `notify_rtp_ready` | `sc.packet.*`, reap / dequeue | as R4.6 / R4.7 |
| `ST22_TX_FLAG_USER_P_MAC` (0), `ST22_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST22_TX_FLAG_DISABLE_BOXES` (2), `ST22_RX_FLAG_DISABLE_BOXES` (2) | — | removed (M12) |
| `ST22_TX_FLAG_USER_PACING` (3), `ST22_TX_FLAG_USER_TIMESTAMP` (4) | `sc.media_mode`, `unit.*` | R4.1 |
| `ST22_TX_FLAG_ENABLE_VSYNC` (5), `ST22_RX_FLAG_ENABLE_VSYNC` (1) | `MTL_OPT_EPOCH_TICK = 1` | R4.1 |
| `ST22_TX_FLAG_ENABLE_RTCP` (6), `ST22_RX_FLAG_ENABLE_RTCP` (3) | `MTL_OPT_RTX = 1` | R4.1 |
| `ST22_TX_FLAG_DISABLE_BULK` (7) | `MTL_OPT_CVIDEO_DISABLE_BULK = 1` | flag → option |
| `ST22_TX_FLAG_FORCE_NUMA` (8), `ST22_RX_FLAG_FORCE_NUMA` (5) | `MTL_OPT_NUMA` | R4.1 |
| `ST22_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |
| `ST22_RX_FLAG_SIMULATE_PKT_LOSS` (4) | `MTL_FAULT_DROP_PKTS` | R4.1 |
| `ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (16) | `MTL_OPT_RX_INCOMPLETE` | R4.1 |

### R4.10 `st30p_tx_ops`, `st30p_rx_ops`, `st30_tx_ops`, `st30_rx_ops`

`sc.essence = MTL_AUDIO`; `a` = `sc.audio`. Pipeline and session map alike; session `type` RTP_LEVEL → `MTL_UNIT_PACKETS`.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `port` (pipelines) or `dip_addr` / `ip_addr`, `sip_addr`, `port`, `num_port`, `udp_port`, `payload_type`, `ssrc`, `mcast_sip_addr`, `udp_src_port`, `tx_dst_mac` | `sc.flows[]` | R4.1 |
| `fmt` | `a.format` | +1 |
| `channel` | `a.channels` | same |
| `sampling` | `a.sample_rate` | Hz instead of enum (R4.14) |
| `ptime` | `a.ptime` | +1; the three 44.1 kHz values: GAP (R4.14) |
| `type` (session) | `sc.unit` | R4.1 |
| `pacing_way` (TX) | `MTL_OPT_PACING` | `ST30_TX_PACING_WAY_AUTO` = absent, `RL` → `MTL_PACING_HW_RATE`, `TSC` → `MTL_PACING_SW` |
| `framebuff_cnt` | `sc.pool_count` | same |
| `framebuff_size` | `a.unit_samples` | bytes → samples per channel: `framebuff_size / (channels × bytes per sample)`; `mtl_audio_bytes()` converts back |
| `name`, `priv`, `flags`, `socket_id` | R4.1 / rows below | R4.1 |
| `get_next_frame`, `notify_frame_done`, `notify_frame_late`, `notify_frame_available`, `notify_frame_ready`, `rtp_ring_size`, `notify_rtp_done`, `notify_rtp_ready` | R4.1 | R4.1 |
| `fifo_size` (TX) | `MTL_OPT_AUDIO_FIFO_MS` | packets → ms: `fifo_size × ptime` |
| `rtp_timestamp_delta_us` (TX) | `sc.media_time_offset_ns` | × 1000 |
| `rl_accuracy_ns`, `rl_offset_ns` (TX) | `MTL_OPT_AUDIO_RL_ACCURACY_NS`, `MTL_OPT_AUDIO_RL_OFFSET_NS` | same |
| `notify_timing_parser_result` (session RX) | `mtl_rx_get_timing(s, lease, leg, …)` (`dpvr_max_ns`, `ipt_max_ns`, `tsdf_ns`) + stats `tp.*` | callback every 200 ms per port → per unit and leg |
| `sample_size`, `sample_num` (session) | — | removed (M12, deprecated) |
| `ST30_TX_FLAG_USER_P_MAC` (0), `ST30_TX_FLAG_USER_R_MAC` (1), `ST30P_TX_FLAG_USER_P_MAC` (0), `ST30P_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST30_TX_FLAG_USER_PACING` (3), `ST30P_TX_FLAG_USER_PACING` (3), `ST30_TX_FLAG_USER_TIMESTAMP` (4) | `sc.media_mode`, `unit.*` | R4.1; audio media indices count samples |
| `ST30_TX_FLAG_BUILD_PACING` (5) | `MTL_OPT_AUDIO_BUILD_PACING = 1` | flag → option |
| `ST30_TX_FLAG_ENABLE_RTCP` (6), `ST30_RX_FLAG_ENABLE_RTCP` (1) | — | removed (M12) |
| `ST30_TX_FLAG_DEDICATE_QUEUE` (7), `ST30P_TX_FLAG_DEDICATE_QUEUE` (7) | `MTL_OPT_TX_QUEUE = MTL_TXQ_DEDICATED` | R4.1 |
| `ST30_TX_FLAG_FORCE_NUMA` (8), `ST30P_TX_FLAG_FORCE_NUMA` (8), `ST30_RX_FLAG_FORCE_NUMA` (2), `ST30P_RX_FLAG_FORCE_NUMA` (2) | `MTL_OPT_NUMA` | R4.1 |
| `ST30P_TX_FLAG_BLOCK_GET` (15), `ST30P_RX_FLAG_BLOCK_GET` (15) | `timeout_ns` | R4.1 |
| `ST30P_TX_FLAG_DROP_WHEN_LATE` (16) | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` | R4.1 |
| `ST30_RX_FLAG_DATA_PATH_ONLY` (0), `ST30P_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |
| `ST30_RX_FLAG_SIMULATE_PKT_LOSS` (3), `ST30P_RX_FLAG_SIMULATE_PKT_LOSS` (3) | `MTL_FAULT_DROP_PKTS` | R4.1 |
| `ST30_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (4), `ST30P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME` (4) | `MTL_OPT_RX_INCOMPLETE` | R4.1 |
| `ST30_RX_FLAG_TIMING_PARSER_STAT` (16), `ST30_RX_FLAG_TIMING_PARSER_META` (17) | `MTL_OPT_RX_TIMING_PARSER = 1` (+ `mtl_rx_get_timing`) | flag → option |

### R4.11 `st40_tx_ops`, `st40_rx_ops`, `st40p_tx_ops`, `st40p_rx_ops`

`sc.essence = MTL_ANC`; `n` = `sc.anc`. The ANC raster is the video it follows: `n.video.rate` and `n.video.scan`. Legacy RX has no `fps`; on the epoch timeline it is required now.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `port` (pipelines) or `dip_addr` / `ip_addr`, `sip_addr`, `port`, `num_port`, `udp_port`, `payload_type`, `ssrc`, `mcast_sip_addr`, `udp_src_port`, `tx_dst_mac` | `sc.flows[]` | R4.1 |
| `type` (session) | `sc.unit` | R4.1 |
| `fps` (TX) | `n.video.rate` | +1; interlaced: halve (R4.14) |
| `interlaced` | `n.video.scan` | R4.1; RX: the initial value, detection per `MTL_OPT_ANC_RX_DETECT` |
| `name`, `priv`, `flags`, `framebuff_cnt` | R4.1 / rows below | R4.1 |
| `max_udw_buff_size` (pipelines), `framebuff_size` (session RX) | `n.max_udw_bytes` | same (0 = 64 KiB) |
| `test` (`st40_tx_test_config`: `pattern`, `frame_count`, `paced_pkt_count`, `paced_gap_ns`) | `mtl_debug_inject(obj, MTL_FAULT_TX_MUTATE, &p)`: `p.mutation`, `p.unit_count`, `p.paced_pkts`, `p.paced_gap_ns` | debug; `pattern` same values (`ST40_TX_TEST_NONE` = no call) |
| `get_next_frame`, `notify_frame_done`, `notify_frame_late`, `notify_frame_available`, `notify_frame_ready`, `rtp_ring_size`, `notify_rtp_done`, `notify_rtp_ready` | R4.1 | R4.1 |
| `rtp_ring_size` (`st40p_rx_ops`) | — | removed (M12, documented mandatory, unused) |
| `ST40_TX_FLAG_USER_P_MAC` (0), `ST40_TX_FLAG_USER_R_MAC` (1), `ST40P_TX_FLAG_USER_P_MAC` (0), `ST40P_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST40_TX_FLAG_USER_PACING` (3), `ST40P_TX_FLAG_USER_PACING` (3), `ST40_TX_FLAG_USER_TIMESTAMP` (4), `ST40P_TX_FLAG_USER_TIMESTAMP` (4) | `sc.media_mode`, `unit.*` | R4.1 |
| `ST40_TX_FLAG_ENABLE_RTCP` (5), `ST40P_TX_FLAG_ENABLE_RTCP` (5), `ST40_RX_FLAG_ENABLE_RTCP` (1), `ST40P_RX_FLAG_ENABLE_RTCP` (1) | — | removed (M12) |
| `ST40_TX_FLAG_DEDICATE_QUEUE` (6), `ST40P_TX_FLAG_DEDICATE_QUEUE` (6) | `MTL_OPT_TX_QUEUE = MTL_TXQ_DEDICATED` | R4.1 |
| `ST40_TX_FLAG_EXACT_USER_PACING` (7), `ST40P_TX_FLAG_EXACT_USER_PACING` (9) | `MTL_SUBMIT_EXACT` | R4.1 |
| `ST40_TX_FLAG_SPLIT_ANC_BY_PKT` (8), `ST40P_TX_FLAG_SPLIT_ANC_BY_PKT` (10) | `MTL_OPT_ANC_SPLIT_BY_PACKET = 1` | flag → option |
| `ST40P_TX_FLAG_DROP_WHEN_LATE` (7) | `MTL_OPT_LATE_POLICY = MTL_LATE_DROP` | R4.1 |
| `ST40P_TX_FLAG_FORCE_NUMA` (8), `ST40P_RX_FLAG_FORCE_NUMA` (2) | — | removed (M12, "NOT SUPPORTED YET") |
| `ST40P_TX_FLAG_BLOCK_GET` (15), `ST40P_RX_FLAG_BLOCK_GET` (15) | `timeout_ns` | R4.1 |
| `ST40_RX_FLAG_DATA_PATH_ONLY` (0), `ST40P_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |
| `ST40_RX_FLAG_DISABLE_AUTO_DETECT` (2), `ST40P_RX_FLAG_DISABLE_AUTO_DETECT` (3) | `MTL_OPT_ANC_RX_DETECT = MTL_DETECT_OFF` | flag → option (a present 0 is literal) |

### R4.12 `st41_tx_ops`, `st41_rx_ops`

`sc.essence = MTL_FASTMETA`; `f` = `sc.fastmeta`. Legacy RX is RTP level only: `sc.unit = MTL_UNIT_PACKETS` reproduces it; frame units are new.

| Legacy field | New field or option | Conversion |
|---|---|---|
| `dip_addr` / `ip_addr`, `sip_addr`, `port`, `num_port`, `udp_port`, `payload_type`, `ssrc`, `mcast_sip_addr`, `udp_src_port`, `tx_dst_mac` | `sc.flows[]` | R4.1 |
| `type` (TX) | `sc.unit` | R4.1 |
| `fps` (TX) | `f.video.rate` | +1; interlaced: halve (R4.14) |
| `interlaced` | `f.video.scan` | R4.1 |
| `fmd_dit` | `f.data_item_type` | same; RX 0xffffffff (no check) → leave `MTL_FASTMETA_RX_MATCH_DIT` unset, else set it |
| `fmd_k_bit` | `f.k_bit` | same; RX 0xff (no check) → leave `MTL_FASTMETA_RX_MATCH_K` unset, else set it |
| `name`, `priv`, `flags`, `framebuff_cnt` (TX) | R4.1 / rows below | R4.1 |
| `get_next_frame`, `notify_frame_done`, `rtp_ring_size`, `notify_rtp_done`, `notify_rtp_ready` | R4.1 | R4.1 |
| `ST41_TX_FLAG_USER_P_MAC` (0), `ST41_TX_FLAG_USER_R_MAC` (1) | `MTL_FLOWF_USER_MAC` | R4.1 |
| `ST41_TX_FLAG_USER_PACING` (3), `ST41_TX_FLAG_USER_TIMESTAMP` (4) | `sc.media_mode`, `unit.*` | R4.1 |
| `ST41_TX_FLAG_ENABLE_RTCP` (5), `ST41_RX_FLAG_ENABLE_RTCP` (1) | — | removed (M12) |
| `ST41_TX_FLAG_DEDICATE_QUEUE` (6) | `MTL_OPT_TX_QUEUE = MTL_TXQ_DEDICATED` | R4.1 |
| `ST41_RX_FLAG_DATA_PATH_ONLY` (0) | — | removed (M12) |

### R4.13 `st_api.h` address structs and ST 2110-10 helpers

| Legacy | New | Conversion |
|---|---|---|
| `st_tx_dest_info` (`dip_addr[]`, `udp_port[]`) with `*_tx_update_destination()` | `sc.flows[i].ip`, `sc.flows[i].udp_port` + `mtl_session_update(s, &sc, MTL_UPDATE_FLOWS, when, NULL)` | `mtl_session_get_config()`, edit the flows, update |
| `st_rx_source_info` (`ip_addr[]` / `sip_addr[]`, `udp_port[]`, `mcast_sip_addr[]`) with `*_rx_update_source()` | the same flows fields + `source_filter` | as above |
| `enum st10_timestamp_fmt` (`TAI` 0, `MEDIA_CLK` 1) | `unit.media_tai_ns` / `unit.rtp` + `MTL_SUBMIT_RTP_TS` | the format picks the field (R4.1) |
| `st10_tai_to_media_clk()`, `st10_media_clk_to_tai()` | `mtl_media_ticks()`, `mtl_media_tai()` (mtl_sync.h) | exact; `mtl_media_tai` takes a nearby TAI to unwrap |
| `st_frame_rate()`, `st_name_to_fps()` | `mtl_fps_rational()` | double → exact rational; names: none (`mtl_format_parse` covers formats only) |

### R4.14 Enum tables

Each value was checked against both headers.

`enum st_fps` → `enum mtl_fps` (+1; 0 stays "not set"):

| `ST_FPS_*` | value | `MTL_FPS_*` | value |
|---|---|---|---|
| `P59_94` | 0 | `MTL_FPS_59_94` | 1 |
| `P50` | 1 | `MTL_FPS_50` | 2 |
| `P29_97` | 2 | `MTL_FPS_29_97` | 3 |
| `P25` | 3 | `MTL_FPS_25` | 4 |
| `P119_88` | 4 | `MTL_FPS_119_88` | 5 |
| `P120` | 5 | `MTL_FPS_120` | 6 |
| `P100` | 6 | `MTL_FPS_100` | 7 |
| `P60` | 7 | `MTL_FPS_60` | 8 |
| `P30` | 8 | `MTL_FPS_30` | 9 |
| `P24` | 9 | `MTL_FPS_24` | 10 |
| `P23_98` | 10 | `MTL_FPS_23_98` | 11 |
| — | — | `MTL_FPS_47_95`, `MTL_FPS_48` (new) | 12, 13 |

Interlaced sessions: legacy `fps` is the field rate (06 §3.4), `raster.rate` the frame rate. `P59_94` → `MTL_FPS_29_97`, `P50` → `MTL_FPS_25`, `P119_88` → `MTL_FPS_59_94`, `P120` → `MTL_FPS_60`, `P100` → `MTL_FPS_50`, `P60` → `MTL_FPS_30`; the rest have no named half: `rate = 0` and `fps` = {15000, 1001}, {25, 2}, {15, 1}, {12, 1}, {12000, 1001} for `P29_97`, `P25`, `P30`, `P24`, `P23_98`.

`enum st20_fmt` → `enum mtl_video_format` (+1) and `enum mtl_video_format_ext` (mtl_format.h):

| `ST20_FMT_*` | value | New | value |
|---|---|---|---|
| `YUV_422_10BIT`, `YUV_422_8BIT`, `YUV_422_12BIT`, `YUV_422_16BIT` | 0–3 | `MTL_YUV422_10`, `MTL_YUV422_8`, `MTL_YUV422_12`, `MTL_YUV422_16` | 1–4 |
| `YUV_420_8BIT`, `YUV_420_10BIT`, `YUV_420_12BIT`, `YUV_420_16BIT` | 4–7 | `MTL_YUV420_8`, `MTL_YUV420_10`, `MTL_YUV420_12`, `MTL_YUV420_16` | 5–8 |
| `RGB_8BIT`, `RGB_10BIT`, `RGB_12BIT`, `RGB_16BIT` | 8–11 | `MTL_RGB_8`, `MTL_RGB_10`, `MTL_RGB_12`, `MTL_RGB_16` | 9–12 |
| `YUV_444_8BIT`, `YUV_444_10BIT`, `YUV_444_12BIT`, `YUV_444_16BIT` | 12–15 | `MTL_YUV444_8`, `MTL_YUV444_10`, `MTL_YUV444_12`, `MTL_YUV444_16` | 13–16 |
| `YUV_422_PLANAR10LE` (non-RFC 4175) | 16 | `MTL_YUV422P10LE_NONSTD` | 17 |
| `V210` (non-RFC 4175) | 17 | `MTL_V210_NONSTD` | 18 |

`enum st20_packing` → `enum mtl_packing` (same): `BPM` 0 → `MTL_PACKING_BPM` 0, `GPM` 1 → `MTL_PACKING_GPM` 1, `GPM_SL` 2 → `MTL_PACKING_GPM_SL` 2.

`enum st21_pacing` → `enum mtl_sender_type` (same): `NARROW` 0 → `MTL_SENDER_N` 0, `WIDE` 1 → `MTL_SENDER_W` 1, `LINEAR` 2 → `MTL_SENDER_NL` 2.

`enum st30_fmt` → `enum mtl_audio_format` (+1): `ST30_FMT_PCM8` 0 → `MTL_PCM8` 1, `PCM16` 1 → `MTL_PCM16` 2, `PCM24` 2 → `MTL_PCM24` 3, `ST31_FMT_AM824` 3 → `MTL_AM824` 4.

`enum st30_sampling` → `mtl_audio_config.sample_rate` (Hz): `ST30_SAMPLING_48K` 0 → 48000, `ST30_SAMPLING_96K` 1 → 96000, `ST31_SAMPLING_44K` 2 → 44100.

`enum st30_ptime` → `enum mtl_ptime` (+1):

| `ST30_PTIME_*` / `ST31_PTIME_*` | value | `MTL_PTIME_*` | value |
|---|---|---|---|
| `ST30_PTIME_1MS` | 0 | `MTL_PTIME_1MS` | 1 |
| `ST30_PTIME_125US` | 1 | `MTL_PTIME_125US` | 2 |
| `ST30_PTIME_250US` | 2 | `MTL_PTIME_250US` | 3 |
| `ST30_PTIME_333US` | 3 | `MTL_PTIME_333US` | 4 |
| `ST30_PTIME_4MS` | 4 | `MTL_PTIME_4MS` | 5 |
| `ST31_PTIME_80US` | 5 | `MTL_PTIME_80US` | 6 |
| `ST31_PTIME_1_09MS`, `ST31_PTIME_0_14MS`, `ST31_PTIME_0_09MS` (44.1 kHz) | 6, 7, 8 | — | GAP |

`enum st_frame_fmt` → `enum mtl_app_format` (+1; mtl_format.h):

| `ST_FRAME_FMT_*` | value | `MTL_APP_*` | value |
|---|---|---|---|
| `YUV422PLANAR10LE`, `V210`, `Y210`, `YUV422PLANAR8`, `UYVY` | 0–4 | `MTL_APP_YUV422P10LE`, `MTL_APP_V210`, `MTL_APP_Y210`, `MTL_APP_YUV422P8`, `MTL_APP_UYVY` | 1–5 |
| `YUV422RFC4175PG2BE10`, `YUV422PLANAR12LE`, `YUV422RFC4175PG2BE12` | 5–7 | `MTL_APP_YUV422_PG2_BE10`, `MTL_APP_YUV422P12LE`, `MTL_APP_YUV422_PG2_BE12` | 6–8 |
| `YUV444PLANAR10LE`, `YUV444RFC4175PG4BE10`, `YUV444PLANAR12LE`, `YUV444RFC4175PG2BE12` | 8–11 | `MTL_APP_YUV444P10LE`, `MTL_APP_YUV444_PG4_BE10`, `MTL_APP_YUV444P12LE`, `MTL_APP_YUV444_PG2_BE12` | 9–12 |
| `YUV420CUSTOM8`, `YUV422CUSTOM8` | 12, 13 | `MTL_APP_YUV420_CUSTOM8`, `MTL_APP_YUV422_CUSTOM8` | 13, 14 |
| `YUV420PLANAR8`, `YUV422PLANAR16LE` | 14, 15 | `MTL_APP_YUV420P8`, `MTL_APP_YUV422P16LE` | 15, 16 |
| — | — | `MTL_APP_NV12` (new) | 17 |
| `ARGB`, `BGRA`, `RGB8` | 32–34 | `MTL_APP_ARGB`, `MTL_APP_BGRA`, `MTL_APP_RGB8` | 33–35 |
| `GBRPLANAR10LE`, `RGBRFC4175PG4BE10`, `GBRPLANAR12LE`, `RGBRFC4175PG2BE12` | 35–38 | `MTL_APP_GBRP10LE`, `MTL_APP_RGB_PG4_BE10`, `MTL_APP_GBRP12LE`, `MTL_APP_RGB_PG2_BE12` | 36–39 |
| — | — | `MTL_APP_RGBA` (new) | 40 |
| `JPEGXS_CODESTREAM`, `H264_CBR_CODESTREAM`, `H264_CODESTREAM`, `H265_CBR_CODESTREAM`, `H265_CODESTREAM` | 56–60 | no app format: `c.app_format = 0` and `c.codec` as `st22_codec` below | — |

Other enums the field tables use:

| Legacy enum | New | Conversion |
|---|---|---|
| `enum st22_codec`: `JPEGXS` 0, `H264_CBR` 1, `H264` 2, `H265_CBR` 3, `H265` 4 | `c.codec`: `MTL_CODEC_JPEGXS` 1, `MTL_CODEC_H264` 2, `MTL_CODEC_H265` 3; `c.rate_mode` | table: `_CBR` → `MTL_CVIDEO_CBR`, H.264/H.265 without it → `MTL_CVIDEO_VBR_MAX`; JPEG XS → CBR (default) |
| `enum st22_pack_type`: `CODESTREAM` 0, `SLICE` 1 | `MTL_OPT_CVIDEO_PACK`: `MTL_CVIDEO_PACK_CODESTREAM` 1, `MTL_CVIDEO_PACK_SLICE` 2 | +1 |
| `enum st22_quality_mode`: `SPEED` 0, `QUALITY` 1 | `MTL_OPT_CVIDEO_QUALITY` | same value, unnamed (GAP) |
| `enum st_plugin_device`: `AUTO` 0, `CPU` 1, `GPU` 2, `FPGA` 3, `TEST` 4, `TEST_INTERNAL` 5 | `enum mtl_codec_device`: absent, `MTL_CODEC_DEVICE_CPU` 1, `_GPU` 2, `_FPGA` 3, `_TEST` 4; — | same values; `AUTO` = absent; `TEST_INTERNAL` → `MTL_CODEC_DEVICE_TEST` with `mtl_plugin_register()` |
| `enum st21_tx_pacing_way`: `AUTO` 0, `RL` 1, `TSC` 2, `TSN` 3, `PTP` 4, `BE` 5, `TSC_NARROW` 6 | `enum mtl_pacing` (`MTL_OPT_PACING`): absent, `MTL_PACING_HW_RATE` 2, `MTL_PACING_SW` 4, `MTL_PACING_HW_LAUNCH` 3, `MTL_PACING_PTP` 6, `MTL_PACING_BEST_EFFORT` 7, `MTL_PACING_SW_NARROW` 5 | table; `MTL_PACING_HW` 1 = any hardware class; fail instead of fall back: `MTL_OPT_PACING_REQUIRED = 1` |
| `enum st30_tx_pacing_way`: `AUTO` 0, `RL` 1, `TSC` 2 | absent, `MTL_PACING_HW_RATE`, `MTL_PACING_SW` | table |
| `enum mtl_log_level` (mtl_api.h): `DEBUG` 0 … `CRIT` 5 | `enum mtl_log_level` (mtl_options.h): `MTL_LOG_DEBUG` 1 … `MTL_LOG_CRIT` 6 | +1; the two headers declare the same tag (R4.15) |
| `enum mtl_rss_mode`: `NONE` 0, `L3` 1, `L3_L4` 2 | `enum mtl_rss`: `MTL_RSS_NONE` 1, `MTL_RSS_L3` 2, `MTL_RSS_L3_L4` 3 | +1 |
| `enum mtl_iova_mode`: `AUTO` 0, `VA` 1, `PA` 2 | `enum mtl_iova`: absent, `MTL_IOVA_VA` 1, `MTL_IOVA_PA` 2 | same; `AUTO` = absent |
| `enum mtl_pmd_type`: `DPDK_USER` 0, `NATIVE_AF_XDP` 4, `KERNEL_SOCKET` 17, `DPDK_AF_XDP` 19, `DPDK_AF_PACKET` 20 | `mtl_port_spec.name`: BDF, `native_af_xdp:<if>`, `kernel:<if>`; — | enum → name prefix; the two DPDK PMDs: removed (M12) |
| `enum st20_type` / `st22_type` / `st30_type` / `st40_type` / `st41_type` | `enum mtl_unit_kind` | `FRAME_LEVEL` 0 → `MTL_UNIT_FRAME` 0, `SLICE_LEVEL` 2 → `MTL_UNIT_ROWS` 1, `RTP_LEVEL` 1 → `MTL_UNIT_PACKETS` 2 (table) |
| `enum st40_tx_test_pattern`: `NONE` 0, `NO_MARKER` 1, `SEQ_GAP` 2, `BAD_PARITY` 3, `PACED` 4 | `enum mtl_tx_mutation`: —, `MTL_TX_MUTATE_NO_MARKER` 1, `_SEQ_GAP` 2, `_BAD_PARITY` 3, `_PACED` 4 | same; `NONE` = no call |

### R4.15 Gaps found, and how they are closed

The map found these fields with no home in the revision-4 headers and not on the M12 list.
Each is now closed in the headers (check.sh green):

1. **44.1 kHz packet times.** `ST31_PTIME_1_09MS`, `ST31_PTIME_0_14MS`, `ST31_PTIME_0_09MS`: **added** `MTL_PTIME_1_09MS = 7`, `MTL_PTIME_0_14MS = 8`, `MTL_PTIME_0_09MS = 9`.
2. **Per-session migrate opt-out.** `ST20_RX_FLAG_DISABLE_MIGRATE`, `ST20P_RX_FLAG_DISABLE_MIGRATE`: **added** `session.migrate` (`MTL_OPT_MIGRATE`, default from `instance.*_video_migrate`). Q-THR-6 is revised: migration is ported, not dropped, because no current use case is cut without approval.
3. **Padded stride of a library pool.** `linesize`, `transport_linesize`: **added** `mtl_video_config.linesize[MTL_MAX_PLANES]` (0 = packed).
4. **ST 2110-22 sampling and depth.** `st22_tx_ops.fmt`, `st22_rx_ops.fmt`: **not carried**. Today's library ignores the field, so nothing behaves differently; it is listed with the M12 candidates.
5. **Codec quality values.** `st22p_tx_ops.quality`: **added** `enum mtl_cvideo_quality` (`st22_quality_mode + 1`) for `cvideo.quality`.
6. **Detected packing.** `st20_detect_meta.packing`: **added** the key `rx.detected.packing` (08 §R4.3).
7. **Random loss simulation.** `rtcp.sim_loss_rate`, `rtcp.burst_loss_max`: **added** `MTL_FAULT_DROP_RANDOM` with `drop_ppm` and `drop_count` as the burst (`mtl_debug.h`).
8. **Second plugin test device.** `ST_PLUGIN_DEVICE_TEST_INTERNAL`: **added** `MTL_CODEC_DEVICE_TEST_INTERNAL = 5`.
9. **Coexistence.** `enum mtl_log_level` and `enum mtl_simd_level` were declared by both `mtl_api.h` and the new headers: **renamed** the new tags `enum mtl_log` and `enum mtl_simd` (the enumerators did not clash). check.sh now compiles the legacy headers and every new header in one file whenever a configured build tree is present, so a gradual port can mix them.

**Instance-wide settings.** The legacy instance-wide `pacing`, `pkt_udp_suggest_max_size`,
`MTL_FLAG_RANDOM_SRC_PORT`, `MTL_FLAG_MULTI_SRC_PORT` and `MTL_FLAG_TX_NO_CHAIN` are session
keys. Any session key set on the instance (in `mtl_instance_params.options`, or with
`mtl_set_option` on the instance object) is the default for that instance's sessions
(`mtl_options.h`), so a legacy instance-wide setting stays one setting.

**Today's features whose homes are under `MTL_LATER`.** Per-frame TX addresses
(`mtl_tx_acquire_layout`), `query_ext_frame` (`mtl_rx_provide`) and GPU frame buffers
(`mtl_mem_import_device`) exist today, so they belong to the porting phases (Phase 4, memory),
not to Phase 7. `MTL_LATER` marks every declaration that is not in the first implementation,
whichever phase lands it.
