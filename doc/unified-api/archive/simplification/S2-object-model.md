# S2 — Object model, handles, lifecycle and waiting: simplification pass

| | |
|---|---|
| Status | Proposal for owner review. Nothing here changes the normative sketch yet |
| Date | 2026-10-01 |
| Scope | `mtl_unified.h` sections 3 (handles), 5 (errors, wait objects), 8 (instance), 13 (timelines, groups), 14 (sessions), 18 (CQ), 19 (EQ), 20 (init macros), 22 (later), plus the `*_init` functions spread over every section |
| Inputs | sketch at revision 3 (`check.sh`: 217 exported functions, 203 of them in `mtl_unified.h`); 03, 04 §4.3/§5, 06 §3/§5.5/§10/§11, 07 §2–3, 11, 12; DECISIONS; OPEN-QUESTIONS; C5 review and C5 response |
| Owner update applied | RTP passthrough (packet chunks) is a first-class mode, and the legacy session headers (`st20_api.h` and the others) become non-public (§2.10) |

Verdicts: **RECOMMEND** (adopt), **OPTION** (sound, but a matter of taste or overlaps another S-document), **REJECT** (tried, does not hold).
"BREAKS: X" marks a proposal that breaks a hard constraint. No proposal here does. "DROP-NEW" marks a revision-3 addition that is not a current MTL use case and is removed. Each one needs approval because it answered a C5 finding.

## 0. Summary

| # | Proposal | Hypothesis | Functions saved | Verdict |
|---|---|---|---|---|
| P1 | 18 `is_null`/`eq` twins and 9 NULL macros become 3 macros; the type checks are kept | H1 | −18 (−36 counting the inline copies), ≈ −120 lines | RECOMMEND |
| P2 | Generic object verbs on `mtl_obj_h`: wait, trywait, wait object, interrupt, uninterrupt, reap, read_events | H6 | −15 | RECOMMEND |
| P2b | One `mtl_close(obj, flags)` replaces 6 destroy/release/close calls | H6/H7 | −5 | OPTION |
| P3 | One struct initialiser: `mtl_struct_init(p, size, kind)` + `MTL_INIT`; every versioned struct starts with `{struct_size, kind}` | §20 | −35 (header-wide), −24 macros | RECOMMEND |
| P4 | One `mtl_session_create` + one `mtl_session_query`; the essence comes from the config's `kind` | H4 | −8 | RECOMMEND (P4c, the essence as a union inside the session config, is an OPTION) |
| P5 | No group object: `mtl_session_start/stop` take an array of sessions on one timeline | H2 | −7 | RECOMMEND (H2a, a startable timeline, is REJECTED) |
| P6 | Timelines: one create with an optional name, refcounted close; drop `open`, `destroy` and `get_anchor` | H2 | −2 (+1 through P2b) | RECOMMEND |
| P7 | No CQ/EQ handles in core: per-session reads. Shared CQ and EQ merge into one `mtl_queue_h` in `mtl_queue.h` | H3 | −5 | RECOMMEND (a single record union for both is REJECTED) |
| P8 | One `mtl_instance_open(spec, params, &mt)` with an `MTL_INSTANCE_SHARED` flag; legacy bridge moves to `mtl_legacy.h`; `port_open` moves to later; `port_count` goes | H5 | −4 deleted, −2 moved | RECOMMEND |
| P9 | Lifecycle: one `mtl_when`; one `mtl_session_update`; discard without a params struct; destroy returns 1 when deferred; optional merge of states | H7 | −2 functions, −4 structs | RECOMMEND (the state merge is an OPTION; a blocking destroy by default is REJECTED) |
| P10 | Header tiering: a lean `mtl.h` plus `mtl_mem.h`, `mtl_sync.h`, `mtl_queue.h`, `mtl_observe.h`, `mtl_packet.h`, `mtl_legacy.h`, `mtl_simple.h`, `mtl_debug.h` | H8 | core ≈ 50 functions, ≈ 1500 lines | RECOMMEND |

Net for the S2 sections: **137 → ≈ 53 exported functions** (−61 %, the 2 legacy bridges included), **≈ 1025 → ≈ 560 lines**, **9 → 8 handle types**, of which **4 are in core**, **≈ 29 → ≈ 16 concepts**. Header-wide, P1–P9 remove 101 functions: **203 → ≈ 102** before tiering. After tiering, core `mtl.h` has ≈ 50 functions.
No current MTL use case is cut (§4).

## 1. Diagnosis

### 1.1 Numbers today

Counted from the sketch with the `check.sh` rules: lines that start with a call-class macro; an inline helper counts once, through its exported twin.

| § | Lines | Functions | of which `*_init` | Structs/unions | Enums | `#define`s |
|---|---|---|---|---|---|---|
| 3 Handles | 185 | 25 (18 null/eq, 3 lease/buffer, 4 object ctors) | 0 | 10 | 1 | 18 |
| 5 Errors, wait objects | 37 | 4 | 0 | 2 | 1 | 4 |
| 8 Instance, ports | 270 | 30 | 11 | 10 | 5 | 15 |
| 13 Timelines, groups | 92 | 17 (+1 `mtl_timeline_index_at` in §6) | 3 | 3 | 3 | 2 |
| 14 Sessions | 173 | 39 | 3 | 4 | 2 | 2 |
| 18 CQ | 28 | 10 | 1 | 1 | 0 | 0 |
| 19 EQ | 182 | 11 | 1 | 14 | 4 | 6 |
| 20 Init macros | 33 | 0 | — | 0 | 0 | 25 |
| 22 Later | 25 | 5 (not in the 0.1 count) | 0 | 0 | 0 | 1 |
| **S2 total** | **1025 (34 % of 3029)** | **137 of 203 (67 %)** | 19 (36 header-wide) | 44 | 16 | 73 |

Where the weight comes from:

- **Per-type duplication.** Every verb that applies to "an object" is written once per type:
  - wait and interrupt: 15 functions over session, CQ, EQ and instance;
  - destroy, release and close: 7 functions over 6 types;
  - null and equality: 18 functions over 9 types;
  - init: 36 functions, one per struct;
  - create and query: 10 functions, one per essence.
  Together that is **86 of the 137** S2 functions, and none of them adds a behaviour.
- **Two objects for one job, twice.**
  - A group adds nothing but an atomic start over sessions that must already share a timeline.
  - CQ and EQ are both reader-materialised poll sets with the same wait, interrupt and destroy verbs.
- **Four ways to get an instance**: `open`, `acquire_default`, `open_simple`, `from_legacy`.
- **Four ways to change a session** (`reconfigure`, `update_flows`, `set_leg_enabled`, `discard_queued`), and two "when" structs with the same three fields (`mtl_start_params`, `mtl_activation`).

### 1.2 Concepts a user meets in this area

| Today (29) | After (16) |
|---|---|
| private instance; default instance + merge table; spec-string open; legacy bridge | instance (open, flag SHARED, optional spec string); legacy bridge in its own header |
| session; 5 typed creates; 5 typed queries | session; one create; one query |
| buffer; lease; region | buffer; lease; region (`mtl_mem.h`) |
| private CQ; shared CQ + bind; private EQ (refcounted); instance EQ; created EQ + subscriptions | per-session reads; one shared queue (`mtl_queue.h`) with bind and subscribe |
| timeline (4 anchors); named timeline (open/close vs create/destroy); epoch handle; group + 6 group states | timeline (4 anchors, optional name, refcounted); multi-session start |
| 10 session states; DESTROYING, RETIRED, FORCE, SESSION_RETIRED | 10 states (or 8 with P9e); destroy returns 0 or 1 (deferred) |
| wait object + trywait; wait-target mask; interrupt × 3 scopes; `mtl_object` | one generic wait family on any object; `MTL_OBJ(h)` |
| null/eq helpers; `struct_size` + 36 `_init` + 25 macros | `MTL_NULL`, `MTL_IS_NULL`, `MTL_SAME`; `MTL_INIT(p, kind)` |
| reconfigure, update_flows, set_leg_enabled, discard; start params vs activation | update (one struct, a mask); discard; one `mtl_when` |

## 2. Proposals

### P1 — Handle helpers as three macros (H1)

**Change.**

- Keep distinct C handle types. C5 §2.5 (Blocker) is right that a lease and a buffer must not convert into each other.
- Replace the 9 `MTL_*_NULL` pairs and the 18 `is_null`/`eq` functions (inline plus exported twins) with three macros that work for every handle type.
- `MTL_SAME` keeps the type check. Comparing two distinct handle types is an error in C++ and a constraint diagnostic in C ("comparison of distinct pointer types", an error with `-Werror`). Both cases were checked with gcc, g++ and clang.
- Bindings need no twins: a binding wraps a u64 newtype, and `==` and `== 0` are native in Rust and Python.

```c
/* before: 9 typedefs + 18 NULL macro lines + 18 exported twins + 18 inline bodies (≈ 150 lines) */
if (!mtl_group_is_null(g)) ...;  if (mtl_session_eq(a, b)) ...;  x = MTL_SESSION_NULL;
/* after (≈ 12 lines for the macros) */
#if !defined(__cplusplus)
#define MTL_NULL(T) ((T){0})
#else
#define MTL_NULL(T) (T{})
#endif
#define MTL_IS_NULL(h) ((h).id == 0)
#define MTL_SAME(a, b) ((void)sizeof(&(a) == &(b)), (a).id == (b).id) /* lvalues of one handle type */
if (!MTL_IS_NULL(anc)) ...;  if (MTL_SAME(a, b)) ...;  x = MTL_NULL(mtl_session_h);
```

**Fewer types.**

- P5 removes `mtl_group_h`. P7 replaces `mtl_cq_h` and `mtl_eq_h` with `mtl_queue_h`. P2 adds `mtl_obj_h`.
- The result is 8 types. Four are in core (instance, session, lease, buffer, plus the generic `mtl_obj_h`); `region`, `timeline` and `queue` live in their extension headers.
- Merging buffer and lease is REJECTED (C5 §2.5). Merging region and buffer is out of scope (S-memory); they are different things (bytes and mappings vs a layout).

**Savings.** −18 exported functions (−36 bodies), −18 NULL lines, ≈ −120 lines.
**Assumptions touched.** C99/C++: two macro variants, both compile clean. `MTL_SAME` needs lvalues; in C++, compare with a null handle through `MTL_IS_NULL`.
**Risk.** Low. A C caller without `-Werror` gets a warning, not an error, for a cross-type `MTL_SAME`.
**Verdict.** RECOMMEND.

### P2 — Generic object verbs (H6, part of H1)

**Change.**

- Object IDs already carry their type: `| type:8 | reserved:8 | index:16 | generation:32 |` (C7).
- One generic handle `mtl_obj_h` and an explicit conversion `MTL_OBJ(h)` therefore let one function serve every type. libfabric does the same: `fi_close`, `fi_control` and `fi_trywait` all take a `struct fid*`.
- The library checks the type byte on every call. An object type the verb does not apply to returns `-MTL_EINVAL` with a new reason, `WRONG_OBJECT`; a stale or forged ID returns `-MTL_EBADF`.
- `mtl_obj_h` (8 B) replaces `struct mtl_object` (16 B, `enum mtl_object_kind` and 4 constructors, each with an exported twin).
- Per-port targets (EQ subscription, fault injection) take the port index as an argument or a params field.

```c
typedef struct mtl_obj_h { uint64_t id; } mtl_obj_h;         /* instance, session, timeline, queue, region, buffer */
#define MTL_OBJ(h) ((mtl_obj_h){(h).id})                     /* C++: (mtl_obj_h{(h).id}) */
#define MTL_WAIT_ACQUIRE 0x1u  /* session TX */                 /* targets; 0 = every target the object has */
#define MTL_WAIT_DEQUEUE 0x2u  /* session RX */
#define MTL_WAIT_RESULTS 0x4u  /* session, queue */
#define MTL_WAIT_EVENTS 0x8u   /* session, queue, instance */
#define MTL_WAIT_RETIRED 0x10u /* session: a deferred destroy finished (P9d) */
MTL_API_CP int mtl_get_wait_object(mtl_obj_h o, uint64_t mask, struct mtl_wait_object* out);
MTL_API_DP int mtl_trywait(mtl_obj_h o, uint64_t mask);                  /* 1 ready / 0 armed / < 0 */
MTL_API_WT int mtl_wait(mtl_obj_h o, uint64_t mask, int64_t timeout_ns); /* > 0: the ready subset */
MTL_API_AS int mtl_interrupt(mtl_obj_h o);   /* session or queue; instance = every waiter (was interrupt_all) */
MTL_API_CP int mtl_uninterrupt(mtl_obj_h o);
MTL_API_WT int mtl_reap(mtl_obj_h o, void* rec, size_t rec_size, uint32_t max, int64_t timeout_ns);   /* session or queue: CQ records */
MTL_API_WT int mtl_read_events(mtl_obj_h o, struct mtl_event* ev, size_t ev_size, uint32_t max, int64_t timeout_ns); /* session, queue, instance */
```

Before and after (ex03 and ex11):

```c
/* before */ mtl_session_get_wait_object(s, mask, &wo); r = mtl_session_trywait(s, mask); n = mtl_tx_reap(s, r, sizeof r[0], 16, 0);
             mtl_instance_interrupt_all(g_mt); ... mtl_instance_uninterrupt_all(mt);
/* after  */ mtl_get_wait_object(MTL_OBJ(s), mask, &wo); r = mtl_trywait(MTL_OBJ(s), mask); n = mtl_reap(MTL_OBJ(s), r, sizeof r[0], 16, 0);
             mtl_interrupt(MTL_OBJ(g_mt)); ... mtl_uninterrupt(MTL_OBJ(mt));
```

**Semantics kept.**

- The armed-waiter protocol (04 §5.1) and its one armed word per target.
- Sticky interrupt with "destroy wins" (04 §5.4); the effective state stays session flag OR queue flag OR instance flag.
- `trywait` returns 1, 0 or < 0, never `-MTL_EAGAIN`.
- `mtl_tx_reap` becomes `mtl_reap`. On a session it reads every non-`RX_UNIT` record kind, which also gives RX sessions a way to read `RX_PROGRESS` and `RX_MISSING` follow-ons now that the private CQ handle is gone (P7). `RX_UNIT` stays with `mtl_rx_dequeue`, because dequeue hands out a lease and a view.
- `read_events` on the instance object is the instance's own event stream, which needs no handle (P7).

**Lease safety.**

- A lease ID has no type byte. As written, `MTL_OBJ(lease)` would decode a lease's session index as a type.
- Fix: set bit 63 in every lease ID (`| 1 | session index:15 | slot:16 | generation:32 |`) and keep object types below 128.
- A lease passed as an object then fails deterministically with `-MTL_EBADF`.
- 15 bits allow 32 767 sessions per instance; the engine's maximum is 29 808 (03 §2.2).

**Savings.** −10 wait/interrupt functions (15 → 5), −1 read (`tx_reap`, `cq_read` and `eq_read` → `reap` and `read_events`), −4 object constructors (+4 inline copies), −1 struct, −1 enum. Total **−15**, ≈ −60 lines.
**Assumptions touched.**

- Tasklets: none; every verb is app-side and dispatches on the type byte (one switch).
- C5 §2.9 objected to *implicit* type erasure (`uint64_t object`). Here the conversion is explicit, limited to verbs that are meaningful for several types, and checked on every call.

**Risk.** Medium-low:

- calls read slightly longer (`MTL_OBJ(s)`);
- a wrong-type misuse is found at runtime on the first call, not at compile time.

**Verdict.** RECOMMEND.

#### P2b — One `mtl_close(obj, flags)`

**Change.** `mtl_close(o, flags)` replaces `mtl_instance_release`, `mtl_session_destroy`, `mtl_timeline_close`, `mtl_timeline_destroy`, `mtl_cq_destroy` and `mtl_eq_destroy`. `mtl_mem_destroy` and `mtl_buffer_destroy` (S-memory) can follow.

**One rule.** "Close drops the caller's reference; the object goes when its last reference does." That rule already holds for the instance (03 §7.1), named timelines and EQs. Sessions keep their deferred retire, and `MTL_CLOSE_FORCE` is today's `MTL_DESTROY_FORCE`. Regions keep `-MTL_EBUSY` while referenced, which is their real contract.

**Savings.** −5 functions.
**Risk.** Readability: `mtl_close(MTL_OBJ(s), 0)` against `mtl_session_destroy(s, 0)`. The per-type return rules have to be stated in one table.
**Verdict.** OPTION. The fallback, typed destroys, costs +5 functions and no concept.

### P3 — One struct initialiser (section 20 and the 36 `*_init`)

**Change.**

- Every `struct_size` struct starts with an 8-byte header `{uint32_t struct_size; uint32_t kind;}`. `kind` is the existing `enum mtl_struct_kind`.
- One exported initialiser fills in both header fields. It replaces 36 exported `*_init` functions and 24 of the 25 value macros.
- The library checks `kind` on every input and output struct: `-MTL_EINVAL`, reason `WRONG_KIND`. A struct passed in the wrong place (an output struct as an input, or an audio config to a video call) therefore fails deterministically on the first call.
- `kind` is what makes P4 type-safe at runtime.

```c
/* before */ struct mtl_session_config sc; mtl_session_config_init(&sc);   /* + 35 more init functions, + 25 C-only macros */
/* after  */
MTL_API_DP void mtl_struct_init(void* p, size_t size, uint32_t kind);  /* zero-fill; struct_size = size; kind = kind */
#define MTL_INIT(p, kind) mtl_struct_init((p), sizeof(*(p)), (kind))
#if !defined(__cplusplus)
#define MTL_VALUE(type, k, ...) ((struct type){.struct_size = sizeof(struct type), .kind = (k), __VA_ARGS__})
#endif
struct mtl_session_config sc; MTL_INIT(&sc, MTL_STRUCT_SESSION_CONFIG);
```

**Savings.** −35 functions header-wide (−19 in S2 sections), −24 macros, ≈ −70 lines. `check.sh` lint 2 becomes "every `struct_size` struct is followed by a `kind` field".
**Assumptions touched.**

- `struct_size` versioning stays exactly as in C3, and the zero-default rule is unchanged. `kind`, like `struct_size`, is a header field, not a configuration field.
- Every layout gains 4 B, or takes over an existing `reserved0` where one follows `struct_size`.
- C99 and C++: `MTL_INIT` works in both. C++ and bindings call `mtl_struct_init`.

**Risk.**

- Passing the wrong `kind` constant is caught at the first call.
- C++ loses the typed `mtl_x_init(&x)`. An optional C++ `template` overload is possible but costs lines.

**Verdict.** RECOMMEND (shared with S-config if it covers section 20).

### P4 — One create and one query (H4)

**Change.** The essence is the `kind` of the essence config (P3), so one pair of functions serves all five essences:

```c
/* before: 5 creates + 5 queries */
ret = mtl_video_session_create(mt, &sc, &vc, &s);
ret = mtl_video_session_query(mt, &sc, &vc, MTL_QUERY_CHECK_CAPACITY, &info, &req);
/* after */
MTL_API_CP int mtl_session_create(mtl_instance_h mt, const struct mtl_session_config* sc, const void* essence, mtl_session_h* out);
MTL_API_CP int mtl_session_query(mtl_instance_h mt, const struct mtl_session_config* sc, const void* essence, uint64_t flags,
                                 struct mtl_session_info* MTL_NULLABLE info, struct mtl_buffer_requirements* MTL_NULLABLE req);
struct mtl_video_config vc; MTL_INIT(&vc, MTL_STRUCT_VIDEO_CONFIG); vc.width = 1920; ...
ret = mtl_session_create(mt, &sc, &vc, &s);
```

- `essence->kind` selects VIDEO, CVIDEO, AUDIO, ANC or FASTMETA. Any other kind is `-MTL_EINVAL`, reason `WRONG_KIND`.
- A packet-chunk session (owner update) is the same create with `sc.unit = MTL_UNIT_PACKET_CHUNK` and the essence config of the stream it carries (S8/S9).
- `mtl_reconfigure_params.media_config` is already a `const void*` naming "the session's own essence config". P4 applies the same rule at create.
- A dry-run flag on create was considered and rejected: create has no `info` or `req` outputs, so the query keeps its own signature.

**Savings.** −8 functions, ≈ −40 lines; one concept ("create per essence") gone.
**Assumptions touched.** Compile-time type safety of the essence argument becomes a runtime `kind` check. That check is deterministic and runs on the first call, CP only.
**Risk.** Low: `&sc` or a status struct passed as the essence fails with `WRONG_KIND`. **Verdict.** RECOMMEND.

#### P4c — The essence config as a tagged union inside the session config

**Change.** `sc.essence` plus `union { struct mtl_video_config video; …; uint8_t reserved[192]; } media;`, then `mtl_session_create(mt, &sc, &s)`.

- One struct, one init, one argument, and `update` (P9b) can take the whole config with a mask.
- The essence sub-structs become fixed size, versioning with the session config, and grow beyond 192 B only through `next`.

**Costs.** The session config grows to ≈ 990 B. Unions are awkward for bindings (C5 §3.5). It overlaps the configuration area.
**Verdict.** OPTION. Decide together with S-config.

### P5 — No group object: multi-session start (H2)

**Change.** `mtl_group_h`, `mtl_group_config`, `enum mtl_group_state` and 8 functions are removed. Atomic start and stop take an array:

```c
struct mtl_when { uint32_t mode; uint32_t reserved0; int64_t tai_ns; int64_t media_index; int64_t preroll_ns; uint64_t reserved[2]; }; /* fixed, 48 B; P9a */
MTL_API_CP int mtl_session_start(const mtl_session_h* s, uint32_t n, const struct mtl_when* MTL_NULLABLE when, struct mtl_time* MTL_NULLABLE chosen);
MTL_API_CP int mtl_session_stop(const mtl_session_h* s, uint32_t n, uint32_t mode, int64_t timeout_ns);
/* before (ex07): 7 calls + a handle */
mtl_group_create(mt, tl, NULL, &g); mtl_group_add(g, video); mtl_group_add(g, audio); mtl_group_add(g, anc);
mtl_group_start(g, &sp, &t0); ... mtl_group_stop(g, MTL_STOP_DRAIN, MTL_SEC(2)); mtl_group_destroy(g);
/* after: 2 calls */
mtl_session_h av[3] = {video, audio, anc};
mtl_session_start(av, 3, &when, &t0); ... mtl_session_stop(av, 3, MTL_STOP_DRAIN, MTL_SEC(2));
/* single session */
mtl_session_start(&s, 1, NULL, NULL);
```

`mtl_session_start` with n > 1 runs the four steps of 06 §10.3 unchanged: validate every member, resolve T0, check the horizon, arm all or none. `stop` with n > 1 issues every stop command first, then waits once with the single timeout, which is today's group stop.

| Group feature (r3) | Where it goes |
|---|---|
| all-or-nothing arm and start, `chosen` instant | the start array (n > 1) |
| one timeline per group; TX members INDEX/TAI; one direction | validated per call: every element on one timeline and in one direction, no TX in AUTO; else `-MTL_EINVAL` |
| k's stream = first video member, else first in add order | first video in the array, else `s[0]`: explicit instead of add order |
| join a RUNNING group (C5 §5.10) | `mtl_session_start(&s, 1, NOW)` on the resolved timeline (06 §10.3 already) |
| restart after PARTIAL_FAILURE | the same call; the "members start only through the group" exception disappears with the rule |
| offset inheritance | already a timeline property (`frame_offset`, 06 §10.6) |
| ANC/fastmeta take the video's L_v, raster, rate (06 §5.5, §4.8) | recorded at start from the first video in the array; for a later single start, from the timeline's video owner (the existing offset owner) on created or named timelines. This also gives framework sinks on a named timeline the AUTO window rule, which today needs a group |
| RX: one `link_offset_ns` for every member (06 §11.4) | each session's `timing.link_offset_ns`; a start array whose RX members differ returns `-MTL_EINVAL` |
| group state, `GROUP_MEMBER_FAILED` | per-session `SESSION_STATE` events and `get_state`; the aggregate is DROP-NEW |

**Savings.** −7 group functions (the eighth, `mtl_group_config_init`, is counted in P3); the array start/stop replace `session_start/stop` one for one. Also −1 handle type, −1 struct, −1 enum, and the constants `MTL_OBJECT_GROUP`, `MTL_ORIGIN_GROUP`, `MTL_STRUCT_GROUP_CONFIG`, `MTL_EVENT_GROUP_MEMBER_FAILED` and `MTL_REASON_GROUP_MEMBER`. ≈ −45 lines; one concept gone.
**Assumptions touched.** None of the hard ones. D-12 ("TX and RX groups") changes shape, but none of its behaviour.
**Risk.**

- Nothing stops one app thread from starting a group member alone. It joins at the next feasible index: correct, but not atomic.
- n = 1 calls carry a pointer and a count.

**Verdict.** RECOMMEND. If the owner prefers it, keep `mtl_session_start(s, when)` and add `mtl_session_start_all`: +2 functions, same semantics.

#### H2a — A startable timeline (the timeline is the group) — REJECTED

- Implicit membership (every session naming the timeline) makes `mtl_timeline_start` on a **named** timeline start other framework elements' sessions.
- On the **epoch** timeline (the default for every session) it would start the whole instance.
- An explicit join list on the timeline is just the group again under another name.
- One direction per start, PARTIAL_FAILURE and late join need the same rules as P5, with an object on top.

### P6 — Timeline lifecycle (H2)

**Change.**

```c
struct mtl_timeline_config { uint32_t struct_size; uint32_t kind; char name[MTL_NAME_MAX]; /* "" = private; else open-or-create, refcounted */
  uint32_t anchor_mode; uint32_t step_policy; int64_t tai_ns; struct mtl_rational grid; uint64_t flags; uint64_t reserved[4]; };
MTL_API_CP int mtl_timeline_create(mtl_instance_h mt, const struct mtl_timeline_config* c, mtl_timeline_h* out); /* replaces create + open */
MTL_API_DP int mtl_timeline_get_info(mtl_timeline_h tl, struct mtl_timeline_info* info);  /* replaces get_anchor: t0, RESOLVED */
MTL_API_DP int mtl_timeline_index_at(mtl_timeline_h tl, mtl_session_h s, int64_t tai_ns, int64_t* k);
/* close: mtl_close(MTL_OBJ(tl), 0) (P2b) or one mtl_timeline_close; each session using tl holds a reference */
```

- A non-empty `name` gives today's `open` semantics, including `-MTL_EEXIST` with `TIMELINE_CONFIG_MISMATCH`.
- Sessions hold a reference, so close never fails. This is the instance's rule (03 §7.1) and replaces "`destroy` returns `-MTL_EBUSY` while used".
- `mtl_timeline_epoch(mt)` stays. The option is to drop it and let a null timeline mean the epoch in `index_at` and `get_info` (−1 function).
- Cross-process (06 §10.7: EPOCH + INDEX, or AT_TAI with an explicit grid) is unchanged.

**Savings.** −2 functions (`open`, `get_anchor`), −1 more with P2b (`close` and `destroy` → `mtl_close`). One concept gone: "created vs opened timelines".
**Risk.** Low. **Verdict.** RECOMMEND.

### P7 — Queues: none in core, one shared type in an extension (H3)

**H3 as stated, one queue object with one record union for both kinds, is REJECTED.**

- Results are lossless, ordered per session and bounded by the pool.
- Events are coalesced, can overflow and are fanned out by subscription.
- One record union would force every result reader to skip event records and every event reader to size buffers for 256-B CQ entries.
- Ordering across the two kinds cannot be defined.

**What holds instead:**

**P7a — no queue handles in core (RECOMMEND).**

- Per-session reads cover the single-session case: `mtl_rx_dequeue`, `mtl_reap(MTL_OBJ(s))` and `mtl_read_events(MTL_OBJ(s))`.
- Instance-scope events (PORT, TIME, INSTANCE, SESSION_RETIRED) are read with `mtl_read_events(MTL_OBJ(mt))` (single reader, as today's instance EQ).
- Removed from core: `mtl_session_get_cq`, `mtl_session_get_eq` (and its refcount trick), `mtl_session_bind_eq`, `mtl_instance_get_eq`.
- Retirement stays readable without the private EQ: destroy returns 1 when deferred, `MTL_WAIT_RETIRED`, and `SESSION_RETIRED` on the instance stream and subscribed queues (P9d). This is C5 §7.1.

**P7b — one shared `mtl_queue_h` in `mtl_queue.h` (RECOMMEND).**

- The shared CQ and the created EQ are both poll sets (04 §4.3, 07 §3.3).
- One object holds two internal sources:
  - the **results** of bound sessions: exclusive, lossless, bounded by the members' pools;
  - **event copies** from subscriptions: coalesced, with OVERFLOW.
- Reads are separate: `mtl_reap` for results, `mtl_read_events` for events. Both record formats stay as they are.
- One armed word covers both sources, and the wait mask selects RESULTS and/or EVENTS. A framework element then needs one fd, not two.
- A results-only queue binds sessions and has mask 0; an events-only queue (twelve elements, each watching PORT | TIME, C5 §7.5) binds nothing.

```c
typedef struct mtl_queue_h { uint64_t id; } mtl_queue_h;
struct mtl_queue_config { uint32_t struct_size; uint32_t kind; uint32_t event_capacity; /* 0 = 64 per ring */ uint32_t reserved0;
  uint64_t event_mask; /* MTL_SUB_*: instance-wide subscriptions */ uint64_t reserved[4]; };
MTL_API_CP int mtl_queue_create(mtl_instance_h mt, const struct mtl_queue_config* c, mtl_queue_h* out);
MTL_API_CP int mtl_queue_bind(mtl_queue_h q, mtl_session_h s);                    /* CREATED/STOPPED; s's results go to q instead */
MTL_API_CP int mtl_queue_subscribe(mtl_queue_h q, mtl_obj_h o, uint64_t mask);    /* event copies; mask 0 = unsubscribe */
MTL_API_DP int mtl_queue_post(mtl_queue_h q, const struct mtl_event* ev);         /* USER ring */
MTL_API_CP int mtl_queue_dispatch_start(mtl_queue_h q, mtl_queue_dispatch_fn fn, void* user, void** handle);
MTL_API_CP int mtl_queue_dispatch_stop(void* handle);
/* read: mtl_reap / mtl_read_events; wait: mtl_trywait / mtl_get_wait_object; interrupt; close: all P2 */
```

**Savings.** CQ (10) + EQ (11) + the 4 core accessors = 25 functions, replaced by 6 queue functions plus P2's generic verbs. That is −5 beyond P2. Also −1 struct, −1 handle type, and ≈ −30 lines in core, where the queue moves out.
**Assumptions touched.**

- **Results cannot be lost.** Kept: results are never in a structure an event can fill, and bind stays exclusive.
- Tasklets: unchanged. Both sources are reader-materialised, and completers only set summary bits plus the one armed word.

**Risk.**

- The libfabric map (doc 12) changes: `fi_cq` and `fi_eq` become one `mtl_queue`.
- Per-port subscription is DROP-NEW (subscribe to the instance with `MTL_SUB_PORT` and filter on the payload's `port`).
- `mtl_queue_post` (USER events) is DROP-NEW-eligible: it is not a current MTL use case, and is kept only if the owner wants it.

**Verdict.** RECOMMEND both. Fallback: keep CQ and EQ as two types in `mtl_queue.h` (+6 functions).

### P8 — One instance open; the legacy bridge in its own header (H5)

**Change.**

```c
#define MTL_INSTANCE_SHARED 0x400u /* the refcounted process-wide instance; later opens merge (03 §7.2) */
/* port_spec: "0000:af:01.0=192.168.1.10[,...]", "kernel:eth0", "null:1"; NULL = use p->ports.  p: NULL = all defaults. */
MTL_API_CP int mtl_instance_open(const char* MTL_NULLABLE port_spec, const struct mtl_instance_params* MTL_NULLABLE p, mtl_instance_h* out);
/* before */ mtl_instance_open_simple("0000:af:01.0=192.168.1.10", &mt);  mtl_instance_acquire_default(&p, &mt);
/* after  */ mtl_instance_open("0000:af:01.0=192.168.1.10", NULL, &mt);   p.flags |= MTL_INSTANCE_SHARED; mtl_instance_open(NULL, &p, &mt);
```

- The default stays **private**. A SHARED default would merge two apps that open different NICs and fail the second open with `PORT_NOT_OPEN` (A11).
- Passing both a spec and `p->port_count` is `-MTL_EINVAL`.
- Release is `mtl_close(MTL_OBJ(mt), 0)` (P2b), or `mtl_instance_release` is kept.
- `mtl_port_open` returns `-MTL_ENOTSUP` in 0.1, so it moves to the LATER block with `struct mtl_port_params` and its init.
- `mtl_port_count` duplicates `mtl_instance_info.port_count` and is removed.
- `mtl_instance_from_legacy` / `to_legacy` move to `mtl_legacy.h` (P10).

**Savings.** −4 deleted (`acquire_default`, `open_simple`, `port_open`, `port_count`), −2 moved out of core, −1 struct; three ways of opening become one.
**Risk.** Low; simple apps keep a one-line open. **Verdict.** RECOMMEND.

### P9 — Lifecycle: the minimum users see (H7)

**P9a — one "when" (RECOMMEND).**

- `struct mtl_start_params` (`mode`, `media_index`, `tai_ns`, `preroll_ns`) and `struct mtl_activation` (`kind`, `tai_ns`, `media_index`) are the same thing.
- Both become the fixed-size value type `struct mtl_when` (P5), passed by pointer, with NULL = NOW.
- It is used by `start` and by `update` (P9b). AT_MEDIA_INDEX stays TX-only for activations, with the same rule as today.
- Savings: −1 struct, −1 enum (`mtl_activation_kind`), −2 inits, −2 macros, −2 kinds.

**P9b — one mutator (RECOMMEND).**

- `mtl_session_reconfigure`, `mtl_session_update_flows` and `mtl_session_set_leg_enabled` become one call.
- NMOS IS-05 itself patches `transport_params` and `master_enable` together, with one activation, so one atomic call fits it better than two.

```c
#define MTL_UPDATE_FLOWS 0x1u  /* legs[]: all-or-nothing across legs at `when` (IS-05); boundary command while started */
#define MTL_UPDATE_ENABLE 0x2u /* legs_enabled bit mask: admin state per leg; boundary command */
#define MTL_UPDATE_MEDIA 0x4u  /* essence config (same kind); CREATED/STOPPED only */
#define MTL_UPDATE_POOL 0x8u   /* pool_count; CREATED/STOPPED only */
struct mtl_session_update { uint32_t struct_size; uint32_t kind; uint64_t what; const struct mtl_flow* legs; uint32_t leg_count;
  uint32_t legs_enabled; const void* media; uint32_t pool_count; uint32_t reserved0; struct mtl_when when; uint64_t reserved[4]; };
MTL_API_CP int mtl_session_update(mtl_session_h s, const struct mtl_session_update* u);
```

- Legality is the strictest selected item: MEDIA or POOL require CREATED/STOPPED, and a `flow.port` change requires STOPPED (as reconfigure did).
- The whole update is all-or-nothing, a strict superset of today's guarantees.
- Savings: −2 functions, −1 struct (`mtl_reconfigure_params`; `mtl_activation` went in P9a), one concept ("which of three mutators").

**P9c — discard without a params struct (RECOMMEND).**

- `mtl_session_discard(mtl_session_h s, uint64_t flags, int64_t first_index)`, with `MTL_DISCARD_REBASE`.
- The name loses "_queued"; on RX it still force-completes.
- Savings: −1 struct, −1 init, −1 macro, −1 kind.
- Merging discard into stop was considered and rejected: discard keeps the session RUNNING, and a "stop" that does not stop is a trap.

**P9d — destroy says whether it finished (RECOMMEND).**

- `mtl_session_destroy` (or `mtl_close`) returns **0** when the session is retired and **1** when it is deferred because app leases or holds are still out.
- `mtl_wait(MTL_OBJ(s), MTL_WAIT_RETIRED, timeout)` waits for the deferred retire. A tombstoned or reused slot reads as retired.
- This is what a framework needs before `mtl_mem_destroy` (03 §6.4), without an EQ.
- DESTROYING, RETIRED, `SESSION_RETIRED` (on the instance stream and subscribed queues) and FORCE stay.
- Most users only ever see "destroy returned 0".

**H7 blocking destroy by default — REJECTED.**

- Destroy already blocks, bounded, for device references (03 §6.2). It defers only for leases the **app** holds, so the library cannot bound that wait.
- `GstBaseSrc::stop` and FFmpeg `read_close` destroy while downstream holds buffers that are released later on another thread. Blocking there deadlocks or times out (C5 §7.1).
- A simple app holds no leases at destroy, so deferral never triggers for it, and a blocking default buys it nothing.

**P9e — fewer public states (OPTION).**

- CREATED and STOPPED become IDLE. The 03 §3.2 table treats them the same in every row; "after ERROR" is `get_status().last_error_reason`.
- DRAINING and FLUSHING become STOPPING. The internal sub-state keeps the escalation rules.
- That gives 10 → 8 values, and `SESSION_STATE` events get simpler.
- Most callers react to return codes (`-MTL_ESHUTDOWN`, `-MTL_EIO`) and never call `get_state`. The minimum a user must know: RUNNING or not; ERROR means stop, then start or update; destroy always succeeds.
- Savings are small (2 enumerators, fewer table rows). Risk: observers lose the drain-vs-flush distinction.

**Assumptions touched (P9).** None. No P9 call runs on or waits for a tasklet beyond the existing command and ack.

### P10 — Header tiering (H8), with the owner update

| Header | Content | Functions (≈) | Lines (≈) |
|---|---|---|---|
| `mtl.h` (core) | conventions, errors, reasons; core handles, P1/P2 macros, generic verbs; init, versions; instance open/info; port find/caps; time now, fps; session, flow, essence configs; `MTL_UNIT_*` incl. `PACKET_CHUNK`; create … get_status; TX and RX verbs; CQ records; event record | 48–52 | 1450–1550 |
| `mtl_mem.h` | regions, buffer create/destroy/desc/view, attach/detach, requirements, pool region, get_buffers, hold, `acquire_buffer`, `acquire_dynamic`, lease copy, `rx_transfer` | ≈ 22 | ≈ 320 |
| `mtl_sync.h` | timelines (P6), `rational_index_at`, time convert, cross-timestamp, user time source, `rx_align`, `row_deadline` | ≈ 11 | ≈ 170 |
| `mtl_queue.h` | `mtl_queue_h` (P7b), subscription masks, post, dispatcher | 6 | ≈ 60 |
| `mtl_observe.h` | stats structs and getters, stat keys, options, port status/capacity, sched status, mem status, instance status, `list_sessions` | ≈ 18 | ≈ 560 |
| `mtl_packet.h` | the packet-chunk (RTP-level) mode, first class per the owner update: chunk views, chunk records, any chunk-specific verbs (S8/S9 own the details) | S8/S9 | S8/S9 |
| `mtl_legacy.h` | `mtl_instance_from_legacy` / `to_legacy` only, over the opaque `struct mtl_main_impl` | 2 | ≈ 25 |
| `mtl_simple.h`, `mtl_debug.h` | as today; `mtl_debug_inject` takes `mtl_obj_h` + `params.port` | 9, 5 | 76, 88 |

**Rules.**

- Each extension includes `mtl.h`; `mtl.h` includes nothing from the extensions.
- Size checks move next to their structs, in the header that defines them.
- The `MTL_UNIFIED_LATER` block (25 lines) leaves the public header for `sketch/later.h`, which `check.sh` keeps compiling.
- A first program (ex01, ex02) includes `mtl.h` only. A framework plugin adds `mtl_mem.h` and `mtl_queue.h`; a sync playout app adds `mtl_sync.h`.

**Packet-chunk mode (owner update).**

- In the object model it is a session `unit` kind, not a new object. It uses the same handle, states, start/stop/update, wait targets, and results via `mtl_reap`. A chunk is one pool slot, so one `mtl_lease_h`.
- `MTL_UNIT_PACKET_CHUNK` therefore lives in core: create, lifecycle and waiting are shared.
- Only the chunk data verbs and records go to `mtl_packet.h`, so media-only apps never read them.
- If S8/S9 find the chunk path needs no new verbs (acquire/submit with a chunk view), `mtl_packet.h` shrinks to the chunk view and record structs.

**Legacy headers non-public (owner update).**

- No public unified header names an `st20_*`/`st30_*`/`st40_*`/`st41_*` type today. The one reference is a comment in `mtl_debug.h`, which should drop the `st40_api.h` line citation.
- `mtl_legacy.h` exists only while `mtl_api.h` (`mtl_init`, `mtl_handle`) stays public, and exposes nothing else.
- If `mtl_api.h` also becomes non-public, `mtl_legacy.h` becomes an internal header used by in-tree adapters during migration, and the public surface loses those 2 functions.
- The merge table rows for legacy `mtl_init_params` fields (03 §7.2) stay only as long as `mtl_init` is public.

**Verdict.** RECOMMEND.

### P11 — Other ideas tried and rejected

| Idea | Why it does not stick |
|---|---|
| An implicit process-default instance (null `mt` = default) | hidden global state (C5 §2.12); ports still need configuring; P8 gives a one-line open |
| Merge lease into buffer | C5 §2.5 Blocker: every access-moving misuse would compile |
| Drop `trywait`, keep only wait objects | without it there is no race-free "arm then block" for external event loops (04 §5.3) |
| Instance-only interrupt | GStreamer `unlock` is per element; interrupting one element must not wake another (Q-LIFE-6) |
| Drop deferred destroy | GstBufferPool / AVBufferPool semantics (Q-LIFE-4); C5 §7.1 |
| Generic functions taking a raw `uint64_t` | any integer compiles; `mtl_obj_h` + `MTL_OBJ` keeps the conversion explicit and greppable |

## 3. Proposed minimal object and verb list (S2 area)

**Objects.**

- Core: instance, session, lease, buffer (identity of a pool slot), and the generic `mtl_obj_h`.
- Extensions: region (`mtl_mem.h`), timeline (`mtl_sync.h`), queue (`mtl_queue.h`).
- Gone: group, CQ, EQ (merged into queue), `struct mtl_object`.
- Today's 9 handle types become 8; the 4 + generic in core are what a first program sees.

| Family | Verbs | n | Today |
|---|---|---|---|
| generic (P2) | `mtl_close`* · `mtl_get_wait_object` · `mtl_trywait` · `mtl_wait` · `mtl_interrupt` · `mtl_uninterrupt` · `mtl_reap` · `mtl_read_events` | 8 | 28 (15 wait, 7 destroy, 3 read, 3 accessors) |
| handle helpers | `mtl_lease_buffer` · `mtl_buffer_index` · `mtl_lease_slot` + macros `MTL_NULL` `MTL_IS_NULL` `MTL_SAME` `MTL_OBJ` | 3 | 25 |
| init, versions | `mtl_struct_init` (+ `MTL_INIT`) · `mtl_struct_known_size` · `mtl_version_num` | 3 | 38 |
| errors | `mtl_last_error` · `mtl_call_seq` · `mtl_error_name` · `mtl_reason_name` | 4 | 4 |
| instance | `mtl_instance_open` · `mtl_instance_get_info` (+ observe getters: status, mem status, list sessions) | 2 + 3 | 16 (excl. init) |
| ports | `mtl_port_find` · `mtl_port_get_caps` (+ observe: status, capacity, sched status) | 2 + 3 | 7 |
| session | `create` · `query` · `start`[] · `stop`[] · `discard` · `update` · `get_state` · `get_info` · `get_status` (+ observe: `get_stats`) | 9 + 1 | 39 (incl. 9 moved to mem/queue) |
| session × memory (S-memory, `mtl_mem.h`) | `get_buffer_requirements` · `attach_buffers` · `detach_buffers` · `get_pool_region` · `get_buffers` | 5 | 5 |
| timeline (`mtl_sync.h`) | `mtl_timeline_create` · `mtl_timeline_get_info` · `mtl_timeline_index_at` · (`mtl_timeline_epoch`) | 3–4 | 17 + 1 (with groups) |
| queue (`mtl_queue.h`) | `mtl_queue_create` · `bind` · `subscribe` · `post` · `dispatch_start` · `dispatch_stop` | 6 | 21 |
| legacy (`mtl_legacy.h`) | `mtl_instance_from_legacy` · `mtl_instance_to_legacy` | 2 | 2 (in core) |
| **Total** | | **≈ 53 + 2** | **157** (S2's 137 + the 2 version calls, the 17 `*_init` outside S2 and `mtl_tx_reap`) |

\* With P2b rejected: `mtl_instance_release`, `mtl_session_destroy`, `mtl_timeline_close`, `mtl_queue_destroy` (+3 net).

### First program after P1–P9 (ex01, abridged)

```c
mtl_instance_h mt;  int ret = mtl_instance_open("0000:af:01.0=192.168.1.10", NULL, &mt);
struct mtl_session_config sc;  MTL_INIT(&sc, MTL_STRUCT_SESSION_CONFIG);  sc.direction = MTL_DIR_TX;
struct mtl_video_config vc;    MTL_INIT(&vc, MTL_STRUCT_VIDEO_CONFIG);    /* width, height, fps, format */
mtl_flow_parse("239.168.85.20:20000,pt=112", &sc.flows[0]);
mtl_session_h s;  ret = mtl_session_create(mt, &sc, &vc, &s);
ret = mtl_session_start(&s, 1, NULL, NULL);
while (g_running) { /* acquire, fill, submit: unchanged */ }
mtl_session_stop(&s, 1, MTL_STOP_DRAIN, MTL_SEC(1));
mtl_close(MTL_OBJ(s), 0);  mtl_close(MTL_OBJ(mt), 0);
```

The S2 functions this uses are open, create, start, stop and close (×2), down from open_simple, video_session_create, session_start, session_stop, session_destroy and instance_release. The concepts are the same, with fewer names.

## 4. Cuts and design changes needing approval

**Current MTL use cases cut: none.** Every capability of today's MTL that falls in this area stays covered:

- multiple instances; shared instances for plugins; legacy coexistence;
- per-session events (today's `notify_event`, VSYNC as `EPOCH_TICK`);
- PTP notifications (TIME events on the instance stream or a queue);
- destroy while the app holds frames;
- RTP-level sessions (the packet-chunk unit, owner update).

Revision-3 additions that change shape or go away. They are not current MTL use cases, but each answered a review finding, so each needs a yes:

| Item | Proposal | Status | Finding it answered, and how it is still met |
|---|---|---|---|
| `is_null`/`eq` functions and exported twins | P1 | replaced by macros | C5 §2.8: null and equality exist, typed |
| per-type wait, interrupt and destroy functions | P2, P2b | replaced by generic verbs | C5 §2.10, §8.12: same protocol and the same three interrupt scopes |
| 36 `*_init`, 25 macros | P3 | replaced by `mtl_struct_init`; layouts gain `kind` | C5 §2.3: one exported initialiser serves C++ and bindings |
| group object, group state, `GROUP_MEMBER_FAILED`, group link-offset override | P5 | DROP-NEW (aggregate, override); the rest moves to the start array | C5 §5.7, §5.10: atomic RX/TX start, late join, restart, per-session link offset with consistency checked at start |
| private CQ/EQ handles, `get_eq` refcount, instance EQ handle, `bind_eq` | P7a | removed | C5 §7.1, §7.5: retire is readable (P9d); the instance stream is `read_events(instance)`; per-element queues |
| per-port subscription; USER posts | P7b | DROP-NEW; USER optional | C5 §7.5 asked for per-element copies, which a queue gives |
| `acquire_default`, `open_simple`, `port_count`; `port_open` in 0.1 | P8 | merged or moved | A11 unchanged; `port_open` was already `-MTL_ENOTSUP` |
| `reconfigure`, `update_flows`, `set_leg_enabled` | P9b | merged into `mtl_session_update` | C5 §7.4, §8.1, §8.2: same rules, stronger atomicity |
| `get_anchor`, `timeline_open/destroy` | P6 | merged | C5 §5.6: named and refcounted, mismatch is `-MTL_EEXIST` |

Decisions affected: D-07, D-12, D-22, D-30, D-36 (signalling only), D-44, D-58, D-59, D-61, plus D-26 (shape only). Q-CMP-5 (shared CQs, still in v1) and Q-LIFE-4 (deferred destroy, still the default) keep their answers.
Hard constraints:

- no tasklet impact in any proposal;
- results stay lossless (P7b keeps them exclusive and pool-bounded);
- `struct_size` and the zero-default rule are kept (P3 adds `kind` next to `struct_size`);
- C99 and C++ were checked for P1/P2's macros.

## 5. Open questions

1. **Generic verbs (P2).** Is explicit erasure through `MTL_OBJ(h)`, checked by the type byte, acceptable for wait, interrupt, reap, read and close? Or should the typed per-object functions stay (+15 functions)?
2. **`mtl_close` (P2b).** One close for every object, or typed destroy and release (+5)?
3. **Start and stop arrays (P5).** One array form, or a single-session form plus `_all` variants (+2)?
4. **ANC/fastmeta ↔ video association without groups (P5).** Is "first video in the start array, else the timeline's video owner (not on the epoch timeline)" enough? Or should ANC and fastmeta configs carry an explicit `video` session handle?
5. **Instance event stream (P7a).** Keep `read_events(MTL_OBJ(mt))` as a single-reader stream, or require a queue for any non-session event?
6. **P4 or P4c.** A separate essence config pointer, or a union inside the session config? Decide with S-config.
7. **`kind` in every versioned struct (P3).** Accept the 4-B layout change everywhere? It also enables `WRONG_KIND` checks on output structs.
8. **State merge (P9e).** IDLE and STOPPING, or keep the 10 values?
9. **Lease encoding.** Bit 63 set, with a 15-bit session index (≤ 32 767 sessions per instance, today's maximum 29 808). Acceptable?
10. **USER posts (`mtl_queue_post`).** Keep (libfabric `fi_eq_write` parity) or drop (no current MTL use)?
11. **`mtl_api.h` after the owner update.** Does it stay public? That decides whether `mtl_legacy.h` is public, internal, or deleted.
12. **`mtl_timeline_epoch`.** Keep the handle, or let a null timeline mean the epoch wherever a timeline argument is accepted (−1)?
