# S4 — Data path and memory: simplification pass

| | |
|---|---|
| Status | Proposal for maintainer review, 2026-10-01. Nothing here is applied to the header yet |
| Scope | Header sections 9 (memory, regions, buffers, pool config, requirements), 15 (CQ records), 16 (TX data path), 17 (RX data path), the lease helpers of section 3 and the session memory calls of section 14 |
| Inputs | `sketch/include/mtl/experimental/mtl_unified.h` (rev 3), examples ex01–ex12, 03 §4, 04 §4.1–§4.2, 05, 06 §4.1/§7.5–§9, 07 §1–§2, 10, 12, R09–R11, C5 + C5-response, DECISIONS |
| Owner's ask | Simpler, leaner include, modular and extensible, every current MTL use case covered; anything cut is listed for approval |
| Late requirement | RTP passthrough / packet chunks become first-class (S8 designs the mode). §2.7 shows the shapes below carry a packet-chunk unit with no second verb set |

Verdicts: **RECOMMEND** (adopt), **OPTION** (worth a maintainer call), **REJECT** (tried, does not stick). Labels: **BREAKS: <assumption>** where a hard constraint or an accepted rule is touched; **CUT – needs approval** for anything removed.

## 0. Summary

| # | Proposal | Verdict | Main saving |
|---|---|---|---|
| P1 | One caller-owned `struct mtl_unit` (lease + slot + planes + meta + per-use values) filled by acquire and dequeue, read by submit | RECOMMEND | acquire 5 → 3 args, dequeue 6 → 3; view, hint, submission and RX-unit outputs collapse into one 208 B struct; per-frame `*_init` calls 1–3 → 0 |
| P2 | The submission folds into the unit; essence sizes become one `used`; ANC table and user meta are written into the slot's meta area, same layout TX and RX | RECOMMEND | `struct mtl_tx_submission` (152 B, 20 fields), `struct mtl_launch`, `enum mtl_launch_mode`, the `valid` mask and 4 meta fields go |
| P3 | Results: a 96 B core record (status, reason, media, margin, RTP, cookie, lease) with the full record selected by `record_size`; RX detail and missing ranges read on demand while the lease is held | RECOMMEND | P1/P2 apps read 96 B and 2 validity bits instead of 208/240 B and 12 + 9; the `RX_MISSING` kind and 4 constants go |
| P4 | One attach call over a uniform layout (`mtl_session_attach`), and **pool slots instead of buffer handles** | RECOMMEND (needs approval: retires `mtl_buffer_h`) | ex04 139 → ≈100 lines, ex09 88 → ≈64; −7 functions and −2 structs net, one handle type fewer |
| P5 | Verb set: 43 exported data-path/memory functions → 28 core + 2 extension + 6 L4 helpers | RECOMMEND | −15 functions in the core header |
| P6 | Completion: one flag (`MTL_SESSION_RESULTS`) and one rule (app memory ⇒ results, always) | RECOMMEND; the EXCEPTIONS cut needs approval | `enum mtl_complete_mode`, `struct mtl_completion_config`, `optional_kinds`, three forced/exception rules go |
| P7 | Progressive and dynamic pools move to extension headers; re-submit is publish; `BY_INDEX` is a pool flag; a packet chunk is "rows of plane 0" | RECOMMEND | `mtl_tx_publish` goes; publish, wait_progress, row_deadline, `RX_PROGRESS`, dynamic acquire and `SOURCE_RELEASED` leave the core |
| P8 | Pool config: 5 enums (13 constants) → 6 flag bits | RECOMMEND | −5 enums |
| P9 | Submit consumes the lease on every failure except `-MTL_EAGAIN` | OPTION | 3 lines per loop and the leaked-lease class of C5 §3.2 |
| P10 | Region calls (`mtl_mem_*`) move to an extension header | OPTION | −7 functions in the core header |

No current MTL use case is cut (§5). Eight design items are cut or changed and listed for approval in §4.

## 1. Diagnosis

### 1.1 Size

| Item | Revision 3 |
|---|---|
| Lines of sections 9 + 15 + 16 + 17 | 568 of 3029 (18.8 %) |
| Exported functions in scope | 43: section 9 (18), lease helpers (3), session memory calls (5), section 16 (12), section 17 (5); plus 6 inline handle helpers for buffer/lease/region |
| Structs in scope | 22 + 1 union: 9 (memory), 9 (CQ records), 3 (TX), `mtl_completion_config` |
| Enums in scope | 16, of which 5 are pool tri-states/booleans with 13 constants (`pool_source`, `path_request`, `rx_overflow`, `rx_slot_select`, `rx_fill`) |
| `struct mtl_tx_submission` | 152 B, 20 fields: 2 time fields, a launch struct, 3 essence sizes (`sample_count`, `valid_bytes`, `ready_rows`), a `valid` mask for one field (`rtp_override`), 4 user-meta fields, 2 ANC fields, cookie, hold, flags |
| CQ record kinds | 5 (`TX_RESULT` 208 B, `RX_UNIT` 240 B, `RX_PROGRESS`, `TX_SOURCE_RELEASED`, `RX_MISSING`) in a 256 B union; `rx_unit` sits exactly at `MTL_CQ_RECORD_MAX` |
| Validity bits | 12 TX time bits + `ESTIMATED`; 9 RX time bits |
| Completion rules | 4 modes, 2 optional kinds, 3 forced/exception rules (attached, dynamic, exported ⇒ ALL; forward pool ⇒ NONE allowed), 1 cookie rule |

### 1.2 Per-call shape

| Call | Arguments | Out bytes when everything is requested | Nullable/size arguments |
|---|---|---|---|
| `mtl_tx_acquire` | 5 | view 144 + hint 32 = 176 | 2 nullable |
| `mtl_tx_acquire_buffer` / `_dynamic` | 6 / 5 | 176 / 144 | 2 / 1 nullable |
| `mtl_tx_submit` | 3 | — (reads 152 B submission) | 1 nullable |
| `mtl_rx_dequeue` | 6 | view 144 + unit 240 = 384 | 2 nullable + `unit_size` |

The header admits the outputs are mostly redundant: "a view is needed once per slot. Cache it by `mtl_buffer_index(mtl_lease_buffer(lease))` and pass NULL" (§9).
So an efficient app makes two mapping calls plus a table lookup to get what one output could carry, and a simple app copies 384 B per RX frame.

### 1.3 What the examples spend

| Example | Lines | Data-path/memory overhead |
|---|---|---|
| ex01 TX minimal | 63 | view init per frame; the submit-failure release (4 lines) |
| ex04 import pool | 139 | ≈42 lines of memory plumbing: requirements query and check (7), region descriptor (9), N × `buffer_create` with 6 plane fields each (17), attach (1), submission init (3), teardown of N buffers and the region (5) |
| ex06 MXL | 58 | `mtl_buffer_index(mtl_lease_buffer(lease))` to learn the grain; buffers pre-created elsewhere |
| ex07 A/V/ANC | 120 | a submission struct reused across three sessions with manual reset of `anc`/`anc_count` |
| ex09 forwarder | 88 | 27 lines to build quadrant layouts (`get_buffers`, `get_desc`, edit, `buffer_create` × 4 × N), a 4 × 16 handle table, a 7-line reap loop the forward pool does not need (05 §5) |
| ex10 processor | 77 | three views, three units, two submissions for three functions |

### 1.4 Root causes

1. **Outputs split by "who designed them"**: view (05), hint (06), submission (06), RX unit (07). Each has its own init, nullability and size rule.
2. **Buffer handles that are only ever slot numbers.** D-55 made a buffer belong to exactly one pool, so a `mtl_buffer_h` is (session, slot) with an object lifecycle around it (create, attach, destroy, `-MTL_EBUSY`) and three mapping helpers.
3. **Essence extras as fields**: `sample_count`/`valid_bytes`/`ready_rows` are one concept ("how much of the unit is valid") in three fields, and ANC/meta travel as pointers on TX but as a meta area on RX.
4. **One record for every reader**: the P1 reader and the timing-analysis reader get the same 208/240 B record with 12/9 validity bits.
5. **Completion as a mode matrix** where the only safety-relevant fact is "is the memory the app's?".

## 2. Proposals

### 2.0 The three loops, before and after

**ex01 loop, before (18 lines in the file)**

```c
while (g_running) {
  mtl_lease_h lease;  struct mtl_buffer_view v;  mtl_buffer_view_init(&v);
  ret = mtl_tx_acquire(s, &lease, &v, NULL, MTL_MS(100));
  if (ret == -MTL_ETIMEDOUT) continue;
  if (ret < 0) { ex_fail("acquire", ret); break; }
  render_yuv422_10(v.plane[0].addr, v.plane[0].stride);
  ret = mtl_tx_submit(s, lease, NULL);
  if (ret < 0) { ex_fail("submit", ret); mtl_tx_release(s, lease); break; }
}
```

**ex01 loop, after (P1; P9 would drop the release)**

```c
struct mtl_unit u;
mtl_unit_init(&u);                            /* once: struct_size; acquire rewrites the rest */
while (g_running) {
  ret = mtl_tx_acquire(s, &u, MTL_MS(100));
  if (ret == -MTL_ETIMEDOUT) continue;
  if (ret < 0) { ex_fail("acquire", ret); break; }
  render_yuv422_10(u.plane[0].addr, u.plane[0].stride);
  ret = mtl_tx_submit(s, &u);
  if (ret < 0) { ex_fail("submit", ret); mtl_tx_release(s, u.lease); break; }
}
```

ex01 was already lean; the gain is the init hoisted out of the loop safely and one shape shared with RX.

**ex04 setup and loop, before (≈60 lines, abridged)**

```c
sc.pool.source = MTL_POOL_ATTACHED;  sc.pool.count = N;  sc.pool.data_path = MTL_PATH_REQUIRE_DIRECT;
/* requirements: init + query + check (7 lines); EQ, create, subscribe (unchanged) */
struct mtl_mem_desc md;  mtl_mem_desc_init(&md);
md.va = pool_base;  md.length = pool_len;  md.access = MTL_MEM_READ;  md.flags = MTL_MEM_MAP_REQUIRED;
mtl_region_h r;  ret = mtl_mem_import(mt, &md, &r);
for (; created < N; created++) {   /* 13 lines: desc init, plane_count, cookie, region, offset, span,
                                      row_bytes, stride, rows from req.plane[0], buffer_create(&base[created]) */
}
if (ret >= 0) ret = mtl_session_attach_buffers(s, base, N);
if (ret >= 0) ret = mtl_session_start(s, NULL);
while (ret >= 0 && g_running) {
  uint32_t i;  uint64_t frame_id;  mtl_lease_h lease;
  if (reap_all(s, 0) < 0) break;
  if (next_framework_frame(&i, &frame_id) < 0) continue;
  ret = mtl_tx_acquire_buffer(s, base[i], &lease, NULL, NULL, MTL_MS(20));
  if (ret == -MTL_EBUSY || ret == -MTL_ETIMEDOUT) { ret = 0; continue; }
  if (ret < 0) break;
  struct mtl_tx_submission sub;  mtl_tx_submission_init(&sub);  sub.user_cookie = frame_id;
  ret = mtl_tx_submit(s, lease, &sub);
  if (ret < 0) mtl_tx_release(s, lease);
}
/* teardown: stop, reap, destroy_and_wait, then N x mtl_buffer_destroy, then mtl_mem_destroy */
```

**ex04 setup and loop, after (P1 + P2 + P4 + L4 `mtl_tx_send_slot`)**

```c
sc.pool.count = N;
sc.pool.flags = MTL_POOL_ATTACHED | MTL_POOL_REQUIRE_DIRECT;   /* imported app memory: results always on */
/* ... EQ, create, subscribe: unchanged ... */
struct mtl_attach a;  mtl_attach_init(&a);   /* the framework arena: N surfaces, natural layout */
a.va = pool_base;  a.length = pool_len;      /* page aligned; imported for this session, read-only for TX */
a.count = N;                                 /* surface i = slot i at i * unit_bytes (pitch 0) */
ret = mtl_session_attach(s, &a);     /* import + layout check (SPAN, POOL_TOO_SMALL) + map */
if (ret >= 0) ret = mtl_session_start(s, NULL);
struct mtl_unit how;  mtl_unit_init(&how);  /* a template: only the per-use fields are read */
while (ret >= 0 && g_running) {
  uint32_t i;
  if (reap_all(s, 0) < 0) break;
  if (next_framework_frame(&i, &how.cookie) < 0) continue;
  ret = mtl_tx_send_slot(s, i, &how, MTL_MS(20));  /* acquire slot i + submit, or nothing at all */
  if (ret == -MTL_EBUSY || ret == -MTL_ETIMEDOUT) ret = 0;  /* still in flight */
}
/* teardown: stop(FLUSH), reap, destroy_and_wait; SESSION_RETIRED means the import is gone */
```

Requirements are still available for framework negotiation, but the example no longer needs them: the attach validates and fails with a reason. Teardown loses the buffer loop and `mtl_mem_destroy`.

**ex09 layouts and loop, before (≈50 lines, abridged)**

```c
static mtl_buffer_h txb[QUADS][MAX_SLOTS];
/* build_tx_layouts: get_buffers(rx) → for q, j: get_desc(rxb[j]), halve row_bytes and rows,
   shift offset, recompute span, buffer_create(&txb[q][j]) → attach_buffers(tx[q], txb[q], n)  (27 lines) */
while (g_running) {
  mtl_lease_h in;  struct mtl_rx_unit u;
  reap_nonblocking(tx);                                    /* 7-line helper: "ALL is forced" */
  ret = mtl_rx_dequeue(rx, &in, NULL, &u, sizeof(u), MTL_MS(50));
  if (ret == -MTL_ETIMEDOUT) continue;
  if (ret < 0) break;
  uint32_t j = mtl_buffer_index(mtl_lease_buffer(in));
  for (uint32_t q = 0; (u.hdr.time_valid & MTL_RT_MEDIA) && q < QUADS; q++) {
    mtl_lease_h out;  struct mtl_tx_submission sub;  mtl_tx_submission_init(&sub);
    if (mtl_tx_acquire_buffer(tx[q], txb[q][j], &out, NULL, NULL, 0) < 0) continue;
    sub.media_tai_ns = u.timing.media_tai_ns;  sub.hold = in;
    if (mtl_tx_submit(tx[q], out, &sub) < 0) mtl_tx_release(tx[q], out);
  }
  if ((ret = mtl_rx_release(rx, in)) < 0) break;
}
```

**ex09 layouts and loop, after**

```c
static int attach_quadrants(mtl_session_h rx, mtl_session_h* tx) {
  struct mtl_session_info ri;  struct mtl_buffer_requirements rr, tr;  mtl_region_h rg;
  mtl_session_info_init(&ri);  mtl_buffer_requirements_init(&rr);  mtl_buffer_requirements_init(&tr);
  int ret = mtl_session_get_pool_region(rx, &rg);
  if (ret >= 0) ret = mtl_session_get_info(rx, &ri);
  if (ret >= 0) ret = mtl_session_get_buffer_requirements(rx, &rr);
  if (ret >= 0) ret = mtl_session_get_buffer_requirements(tx[0], &tr);   /* 1080p quadrant layout */
  for (uint32_t q = 0; ret >= 0 && q < QUADS; q++) {
    struct mtl_attach a;  mtl_attach_init(&a);
    a.region = rg;                      /* the RX library pool: library memory, so no results needed */
    a.count = ri.pool_count;            /* TX slot j lies over RX slot j */
    a.pitch = ri.pool_slot_pitch;
    a.offset = (uint64_t)(q / 2) * tr.plane[0].rows * rr.plane[0].stride + (q % 2) * tr.plane[0].row_bytes;
    a.stride[0] = rr.plane[0].stride;   /* > row_bytes: still DIRECT (05 §4.2) */
    ret = mtl_session_attach(tx[q], &a);
  }
  return ret;
}
/* loop */
struct mtl_unit in, how;
mtl_unit_init(&in);  mtl_unit_init(&how);
while (g_running) {
  ret = mtl_rx_dequeue(rx, &in, MTL_MS(50));
  if (ret == -MTL_ETIMEDOUT) continue;
  if (ret < 0) break;
  how.media_tai_ns = in.media_tai_ns;   /* derived RTP = input RTP if it was compliant */
  how.hold = in.lease;                  /* RX slot stays HELD until each TX unit is done */
  for (uint32_t q = 0; (in.flags & MTL_UNIT_TAI_VALID) && q < QUADS; q++)
    (void)mtl_tx_send_slot(tx[q], in.slot, &how, 0);   /* -MTL_EBUSY: still in flight; this quad drops */
  if ((ret = mtl_rx_release(rx, in.lease)) < 0) break;
}
```

The reap loop disappears: a TX pool over another session's *library* pool is library memory with mandatory holds (P6).
05 §5 already allows NONE for it; the example's "ALL is forced" comment contradicts that exception today.

### 2.1 P1 — One unit struct (H1: confirmed)

**Change.** `mtl_tx_acquire`, `mtl_tx_acquire_slot` and `mtl_rx_dequeue` fill one caller-owned `struct mtl_unit`; `mtl_tx_submit` reads it.
The unit carries the lease, the slot, the planes, the meta area and the per-use values (TX in, zeroed by acquire; RX out).
It is the `struct v4l2_buffer` model (QBUF/DQBUF, `bytesused`, timestamp and flags in both directions), which media developers know.

```c
/* before */
int mtl_tx_acquire(mtl_session_h s, mtl_lease_h* lease, struct mtl_buffer_view* view, struct mtl_slot_hint* hint, int64_t timeout_ns);
int mtl_rx_dequeue(mtl_session_h s, mtl_lease_h* lease, struct mtl_buffer_view* view, struct mtl_rx_unit* unit, size_t unit_size, int64_t timeout_ns);
/* after */
int mtl_tx_acquire(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns);
int mtl_tx_acquire_slot(mtl_session_h s, uint32_t slot, struct mtl_unit* u, int64_t timeout_ns);
int mtl_rx_dequeue(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns);
int mtl_tx_submit(mtl_session_h s, const struct mtl_unit* u);
```

Rules:
- `struct_size` is set once by `mtl_unit_init()` and never rewritten (C3). Acquire and dequeue rewrite every other field, so hoisting the init out of a loop is safe; on TX they zero the per-use fields, so a value from the previous frame never leaks into the next submit.
- Submit reads only the per-use TX fields and the lease; it never trusts the plane/meta outputs (it uses its own copy of the layout).
- The slot hint leaves the acquire path and becomes an opt-in DP getter, `mtl_tx_next_slot(s, &hint)`: it is a session property ("where the next submission lands"), and computing it costs a time-base read and 128-bit math that most acquires do not need.
- `mtl_lease_buffer`, `mtl_buffer_index` and the "cache views by index" advice go: `u.slot` is the index. `mtl_lease_slot()` becomes a static inline decode of the documented lease encoding (C7), for result headers.

**Savings.** Arguments: acquire 5 → 3, dequeue 6 → 3, submit 3 → 2. Structs on the per-frame path: 4 (view, hint, submission, RX unit) → 1. Per-frame init calls: 1 (TX P1), 2 (TX with submission), 2 (RX) → 0. Mapping calls per RX frame for slot-indexed apps (ex06, ex09): 2 → 0.

**Data-path cost.** Acquire writes 208 B into caller memory instead of 8–176 B; dequeue writes 208 B instead of 8–384 B.
The static part (planes, meta, slot, ≈130 B) is a per-slot template in the app half of the lease table (04 §4.1), copied with the per-use fields zeroed.
Estimate: 10–20 ns per call on a warm line, below 1 % of a core at 0.5 M units/s (512 audio sessions at 1 kHz units).
The tasklet side is unchanged (store + fence). Spike S3 should measure it against the pointer form below.

**Assumptions touched.** C5 §2.19 asked for nullable outputs to avoid copies; this replaces "nullable + cache" with "one small fixed copy". C10 inline-safe subset gains `mtl_tx_acquire_slot` and `mtl_tx_next_slot`.

**Risk.** Medium-low. In/out structs are a known hazard (C5 §2.6). It is contained here because the library rewrites the whole struct on every acquire/dequeue and never reads an output field back.

**Alternatives tried.**

| Alternative | Verdict | Why |
|---|---|---|
| Library-owned unit pointer, `mtl_tx_acquire(s, struct mtl_unit** u, t)`, as `st20p_tx_get_frame()` returns `struct st_frame*` | REJECT (S3 comparison point) | Zero copy and familiar, but a write after release lands in the next acquirer's unit (another thread under MP-safe acquire); submit must still snapshot the per-use fields, so only ≈130 B are saved |
| Lease-only + getters (`mtl_lease_addr(lease, p)`, `mtl_lease_stride(...)`) | REJECT | Fine for TX planes, but RX needs status, used, media, RTP, flags: a getter per field |
| Separate out-unit and in-args struct (`submit(s, lease, &args)`) | REJECT | Two structs again, and TX/RX asymmetric; V4L2 shows the in/out unit works |
| Slim unit without `plane[]` (planes cached by slot via `mtl_session_get_slot`) | REJECT for core | Saves 96 B per call but puts the cache back on the P1 path |

**Verdict: RECOMMEND.**

### 2.2 P2 — The submission folds into the unit; one `used`; one meta layout (H2: confirmed, refined)

**Change.** `struct mtl_tx_submission` is removed. Its fields map as follows:

| Revision 3 submission field | Proposed home |
|---|---|
| `media_index`, `media_tai_ns` | `u.media_index`, `u.media_tai_ns` (two fields kept: a value in the field the session mode does not use stays detectable; a single `media` field interpreted by mode was REJECTED because an index in a TAI session would read as a 1970 time and be dropped instead of failing) |
| `flags` (`DISCONTINUITY`), `valid` + `rtp_override` | `u.flags` with `MTL_SUBMIT_DISCONTINUITY`, `MTL_SUBMIT_RTP`; `u.rtp` (the `valid` mask goes) |
| `launch {mode, tai_ns}` | `MTL_SUBMIT_LAUNCH_NOT_BEFORE` / `MTL_SUBMIT_LAUNCH_EXACT` in `u.flags` + `u.launch_tai_ns`; `struct mtl_launch` and `enum mtl_launch_mode` go (to be confirmed with the timing pass) |
| `sample_count`, `valid_bytes`, `ready_rows` | `u.used`: bytes when plane 0 has one row (audio, ANC UDW, fastmeta, cvideo codestream), else rows of plane 0 (progressive rows, packet slots); 0 on TX = the whole unit. Audio `sample_count` = `used / (channels × sample bytes)`, and a remainder is `-MTL_EINVAL` |
| `user_cookie` | `u.cookie` |
| `hold` | `u.hold` |
| `user_meta`, `user_meta_size`, `user_meta_type`, `user_meta_version`, `anc`, `anc_count` | written by the app into the slot's meta area at `u.meta`, which starts with `struct mtl_meta_hdr { kind, tag_version, count, bytes, tag }`. RX dequeue fills the same layout, so TX and RX see one format |

The meta area exists for every slot of every pool. The library allocates it in the lease table (≤ 4 KiB for ANC: 255 × 16 B + header; 1332 B for video user meta; 0 for audio), including for app-memory pools, so attached buffers still need no meta region.

**Before / after (ex07 ANC + video, abridged).**

```c
/* before */
struct mtl_tx_submission sub;  mtl_tx_submission_init(&sub);
sub.media_index = pts;
sub.anc = pkts;  sub.anc_count = n;
ret = mtl_tx_write(anc, udw, udw_len, &sub, MTL_MS(20));
sub.anc = NULL;  sub.anc_count = 0;                         /* manual reset before reuse for video */
ret = mtl_tx_acquire(video, &lease, &v, NULL, MTL_MS(40));
decode_into(&v, data, size);
ret = mtl_tx_submit(video, lease, &sub);
/* after */
ret = mtl_tx_acquire(anc, &u, MTL_MS(20));                  /* u: plane[0] = UDW run, meta = table */
struct mtl_meta_hdr* mh = (struct mtl_meta_hdr*)u.meta;
mh->kind = MTL_META_ANC;  mh->count = n;
memcpy(mh + 1, pkts, n * sizeof(*pkts));
memcpy(u.plane[0].addr, udw, udw_len);
u.used = (uint32_t)udw_len;  u.media_index = pts;
ret = mtl_tx_submit(anc, &u);
ret = mtl_tx_acquire(video, &u, MTL_MS(40));
decode_into(&u, data, size);
u.media_index = pts;
ret = mtl_tx_submit(video, &u);
```

The ANC path is two lines longer for an app that builds a table elsewhere and four lines shorter for one that builds it in place; the RX → TX passthrough becomes `mtl_tx_write(tx, in.plane[0].addr, in.used, &in, t)` (L4 helper, §3), because a received unit is a valid send template.

**Savings.** −1 struct (152 B, 20 fields), −`mtl_launch`, −1 enum, −`mtl_tx_submission_init`, −`MTL_TX_SUBMISSION_INIT`, −`MTL_SUB_RTP`, −`MTL_META_UNTAGGED` (tag 0 in the header). One meta format instead of "pointer on TX, area on RX".

**Assumptions touched.**
- 05 §4.4 "TX user meta travels in the submission and is copied at submit" changes to "is read from the slot's meta area at pick-up".
- **BREAKS: copy-at-submit as the TOCTOU guard for ANC and user meta.** Without the copy, the builder bounds-checks each `mtl_anc_packet` (`udw_offset + udw_count ≤ used`, `count ≤ 255`) at pick-up.
  That adds ≤ 255 compares per ANC unit to the builder's packetisation on the tasklet (not to completion, which stays a store + fence) and keeps memory safety if the app scribbles after submit.
  The alternative is a submit-time snapshot of the meta area (DPC, ≤ 4 KiB memcpy on the app thread): today's semantics at today's cost. Needs a maintainer pick (Q4).

**Risk.** Low. `used` has two units (bytes vs rows), but the rule is mechanical ("rows of plane 0 unless plane 0 has one row") and is the same rule RX already needs. **Verdict: RECOMMEND.**

### 2.3 P3 — Core and full result records (H3: confirmed)

**Change.** Reorder each record so the fields most apps read come first, in a frozen 96 B core, followed by the detail. The existing C4 rule ("write `min(record_size, native)` bytes per record") then selects the depth with no new mode:

```c
struct mtl_tx_result r[16];       int n = mtl_tx_reap(s, r, sizeof(r[0]), 16, 0);  /* 96 B core */
struct mtl_tx_result_full f[16];  int m = mtl_tx_reap(s, f, sizeof(f[0]), 16, 0);  /* 216 B: + timing, legs, path */
```

| Record | Core (96 B) | Full adds |
|---|---|---|
| TX | hdr (48 B), `reason`, `error`, `media_index`, `media_tai_ns`, `margin_ns`, `rtp`, 12 B reserved | submitted/deadline/scheduled/enqueued times, observed first per leg and last, pickup slack, snap error, packet lateness, timing flags, slots skipped, audio sample counters, per-leg packets and reasons, path, DMA/partial counters (216 B) |
| RX (shared-CQ readers) | hdr (lease = the lease now held), `media_index`, `media_tai_ns`, `rtp`, `used`, `missed_before`, 20 B reserved | arrival first/last per leg, presentation, delivered, timing flags, media phase, stream gap, units before start, packets expected/received/recovered, format-changed mask, missing count, path, DMA/partial counters (208 B) |

The core uses two validity bits (`MTL_TT_MEDIA`, `MTL_TT_MARGIN`; `MTL_RT_MEDIA`, `MTL_RT_INDEX`); the other bits only matter to full-record readers.

**RX detail on demand.** Dequeue fills the unit, not a record. For detail, a dequeue-path app calls `mtl_rx_get_result(s, lease, &full, sizeof(full))` (DP) while it holds the lease.
The tasklet half of a slot in APP_READING is stable, so this is a plain copy with no lock. Missing ranges come from `mtl_rx_get_missing(s, lease, ranges, max)`, computed on the app thread from the slot's packet bitmap.
That removes the `MTL_CQE_RX_MISSING` kind, `MTL_CQE_ENABLE_RX_MISSING`, `MTL_RX_MISSING_RANGES`, `MTL_RX_MISSING_EXT` and the inline `missing[4]`.

**Savings.** P1/P2 read 96 B per result instead of 208/240 B, and learn 2 validity bits instead of 21. −1 record kind, −4 constants. `rx_unit` no longer sits at the 240 B limit; the core has 20 B of growth and the full record 32 B.

**Assumptions touched.**
- Tasklet cost is unchanged: the completing context already writes the result into the tasklet half (04 §4.1, step 1).
  The on-demand missing table is *less* tasklet work than computing ranges at completion, if the packet bitmap belongs to the lease slot rather than the reassembly slot (an L2 choice).
  v1 over `st20p` has no ranges at all, so `get_missing` returns `-MTL_ENOTSUP` there until L2.
- Results are still published in submission order, from a ring of pool size: nothing changes in G-09 or in "results cannot be lost".
- Shared-CQ readers that need planes take them from `mtl_session_get_slot(s, slot, &u)` once per slot (static part); the slot comes from `mtl_lease_slot(hdr.lease)`.

**Rejected variants.** A detail follow-on record per unit (`MORE` flag, like `SOURCE_RELEASED`) doubles records for every detail reader. A per-session "detail" flag is redundant with the record size. **Verdict: RECOMMEND.**

### 2.4 P4 — One attach call, and slots instead of buffer handles (H4: confirmed and extended)

**Change, part 1: a uniform-layout attach.** One CP call imports (or reuses) a region and appends `count` slots laid out at a pitch in the session's natural layout:

```c
struct mtl_attach {                 /* 144 B */
  uint32_t struct_size;
  uint32_t count;                   /* slots appended: slot i at offset + i * pitch, or slot_offset[i] */
  mtl_region_h region;              /* null: import [va, va + length) for this session; else an existing region */
  MTL_ADDR(void) va;                /* page aligned (hugepage aligned for hugetlbfs); never rounded outward */
  uint64_t length;
  uint64_t offset;                  /* slot 0, plane 0, from the region/va start */
  uint64_t pitch;                   /* 0 = unit_bytes */
  const uint64_t* slot_offset;      /* NULL = uniform pitch; else count entries (woven interlaced fields, scattered surfaces) */
  uint64_t plane_offset[MTL_MAX_PLANES]; /* from the slot start; 0 = right after the previous plane */
  uint32_t stride[MTL_MAX_PLANES];  /* 0 = natural; any stride >= row_bytes, one per pool */
  uint64_t flags;                   /* MTL_MEM_MAP_REQUIRED | MTL_MEM_NUMA_REQUIRED | MTL_ATTACH_META_IN_SLOT */
  uint64_t meta_offset;             /* MTL_ATTACH_META_IN_SLOT: RX meta written into the slot (MXL grain header) */
  uint32_t meta_capacity;
  uint32_t reserved0;
  uint64_t reserved[2];
};
int mtl_session_attach(mtl_session_h s, const struct mtl_attach* a);  /* CP; CREATED or STOPPED; may be called again to append */
```

- Access is derived from the direction (TX reads, RX writes), so `ACCESS_MISMATCH` cannot happen through the helper.
- An import made by the attach belongs to the session. `SESSION_RETIRED` is posted only after it is unmapped from every device, so the app frees its memory after RETIRED and calls no destroy.
- Validation (span, stride, alignment, region budget, `POOL_TOO_SMALL`, `POOL_COUNT_MAX`) runs here and fails with the same reasons as today.
- Calling it again appends slots, which covers surfaces in several allocations and is the natural path to mixed pools later.

**Change, part 2: slots replace `mtl_buffer_h`.** Since D-55 a buffer belongs to exactly one pool, so the handle is (session, slot) with an object lifecycle around it. With part 1, every place a buffer handle appears has a slot equivalent:

| Revision 3 | Proposed |
|---|---|
| `mtl_buffer_create` / `destroy` / `get_desc`, `struct mtl_buffer_desc` (248 B), `struct mtl_plane_desc` | `struct mtl_attach` (slots described in bulk, validated once) |
| `mtl_session_attach_buffers(s, b[], n)` | `mtl_session_attach(s, &a)` |
| `mtl_session_get_buffers`, `mtl_buffer_get_view`, `struct mtl_buffer_view` | `mtl_session_get_slot(s, slot, &u)` (DP: the static part of a unit) |
| `mtl_tx_acquire_buffer(s, b, …)` | `mtl_tx_acquire_slot(s, slot, &u, t)` |
| `mtl_tx_withdraw(s, b)` | `mtl_tx_withdraw(s, slot)` |
| `mtl_buffer_hold(b)` / `unhold(b)` | `mtl_tx_hold(s, slot, on)` |
| `mtl_lease_buffer`, `mtl_buffer_index`, `mtl_buffer_is_null/eq`, `MTL_BUFFER_NULL` | `u.slot`, inline `mtl_lease_slot()` |
| `mtl_tx_acquire_dynamic(s, &buffer_desc, …)` | `mtl_tx_acquire_layout(s, &attach /* count 1, region set */, &u, t)` in the Phase 4 extension |
| per-buffer `user_cookie` | the slot index is the stable identity; the per-use `u.cookie` is unchanged |

**Savings.** Functions: −10 (`buffer_create`, `_destroy`, `_get_view`, `_get_desc`, `_desc_init`, `_view_init`, `attach_buffers`, `get_buffers`, `lease_buffer`, `buffer_index`), +3 (`mtl_attach_init`, `mtl_session_attach`, `mtl_session_get_slot`): −7 net.
Structs: −3 (`buffer_desc`, `plane_desc`, `buffer_view`), +1 (`mtl_attach`); −1 handle type and its 2 inline helpers. ex04 139 → ≈100 lines, ex09 88 → ≈64, ex06 drops its pre-created buffer array.

**Assumptions touched.**
- **BREAKS (wording, not safety): C5 §2.5 / D-07 "distinct C types for a buffer and a lease".** The blocker was that a base handle and a lease shared one type, so misuse compiled clean.
  With no buffer handle, the only access token is `mtl_lease_h` and a slot is a `uint32_t`: swapping them is still a compile error. The owner listed the buffer type explicitly, so this needs approval.
- D-01 ("one buffer handle …") becomes "one slot model"; D-55's "a buffer belongs to one pool" becomes true by construction.
- 05 §3.7 teardown order is simpler for helper imports; app-owned regions keep `-MTL_EBUSY` on destroy while referenced.

**Risk.** Medium. Re-validation on `mtl_session_reconfigure` now works on slot layouts (same checks). Future "one buffer object in two sessions" is excluded, but D-55 already excluded it.

**Rejected variants.**

| Variant | Verdict | Why |
|---|---|---|
| Attach from an array of plane pointers, layout derived from the session | REJECT | Framework pointers are 64 B aligned, not page aligned; covering them needs outward page rounding, which maps neighbouring memory into the NIC (C5 §6.5, D-16) |
| Infer `pool.source` from "buffers attached at start" | REJECT | Library pools are allocated and the completion/zero-fill defaults resolved at create; inference would move allocation to start (or allocate twice) and make create's capacity check incomplete. The source stays one flag (`MTL_POOL_ATTACHED`, P8) |
| Keep `mtl_buffer_h` and add the helper only | OPTION (fallback) | Keeps D-07 as written, but the header carries both the helper and the full buffer API (≈+9 functions, +3 structs over the recommendation) |

**Verdict: RECOMMEND** (part 1 unconditionally; part 2 needs approval, see §4).

### 2.5 P5 — The verb set (H5)

**Findings per hypothesis.**

| Question | Answer |
|---|---|
| Can `acquire_buffer` and `acquire_dynamic` become args of one acquire? | Partly. `acquire_slot` stays a verb: a "want" argument adds a null token to every P1 loop, and a unit in-field reintroduces the C5 §2.6 reuse hazard (last frame's slot requested again). Dynamic needs a layout and is Phase 4: `mtl_tx_acquire_layout` in an extension |
| Is `rx_transfer` just submit with `.hold`? | Yes: it is `acquire_slot(tx, matching slot)` + `hold` + release on submit. The app already knows the slot mapping (`u.slot`). Removed (no use case lost) |
| Is `tx_write` a helper over acquire + copy + submit? | Yes, including the split at buffer capacity (`used` bytes per unit; the next unit's media time advances by the samples sent). Moves to L4 with the signature `mtl_tx_write(s, data, bytes, const struct mtl_unit* how, t)` |
| `release` / `withdraw` / `hold` / `unhold` | Release (APP_WRITABLE) and withdraw (QUEUED) stay, withdraw by slot; hold/unhold become `mtl_tx_hold(s, slot, on)` |
| `copy_in` / `copy_out`, `wait_progress`, `row_deadline`, `align` | L4 (bounded memcpy over `u.plane[]` for bindings; timing helpers); `wait_progress` → rows extension |
| `lease_buffer`, `buffer_index`, `lease_slot` | Gone, except `mtl_lease_slot` as a static inline |
| `reap` | Stays: it is `cq_read` on the private CQ, but it is the P2 verb |

**Before → after.**

| Area | Revision 3 (exported) | Proposed core | Moved out |
|---|---|---|---|
| TX | acquire, acquire_buffer, acquire_dynamic, submit, publish, release, withdraw, reap, write, row_deadline, submission_init, slot_hint_init (12) | unit_init, acquire, acquire_slot, submit, release, withdraw, hold, reap, next_slot, slot_hint_init (10) | ext: acquire_layout; L4: write, send_slot, row_deadline |
| RX | dequeue, release, wait_progress, transfer, align (5) | dequeue, release, get_result, get_missing (4) | ext: wait_rows; L4: align |
| Memory | 18 (section 9) + 5 (session) + 3 (lease) = 26 | mem_desc_init, mem_info_init, mem_alloc, mem_import, mem_destroy, mem_get_info, mem_map_device, attach_init, session_attach, session_detach, session_get_slot, session_get_pool_region, session_get_buffer_requirements, buffer_requirements_init (14) | L4: copy_in, copy_out |
| Total | 43 | 28 | 2 ext + 6 L4 |

**Direction-generic verbs (REJECTED).** `get/put` for both directions, as `st20p` does today, merges submit (ownership transfer with an outcome, DPC) with RX release (return, DP). Merging only `mtl_tx_release` and `mtl_rx_release` into `mtl_lease_release(lease)` (the lease encodes the session) saves one function; it is an OPTION (Q6) with no safety impact. **Verdict: RECOMMEND** the set in §3.

### 2.6 P6 — Completion: one flag, one rule (H6)

**Change.** `enum mtl_complete_mode`, `struct mtl_completion_config` and `optional_kinds` are replaced by:

1. **App memory ⇒ results, always.** A session whose slots reference app memory (an attach that imported `va`, an attach over an app-imported region, or `MTL_POOL_DYNAMIC`) produces a terminal result for every accepted unit, and no flag can turn that off. This is the only rule the safety guarantee needs: imported memory is never reused before the app reads its result.
2. **Library memory ⇒ no results unless `MTL_SESSION_RESULTS`.** Library memory covers the session's own library pool, and slots attached over *another session's library pool region* (the forward pool). For the latter, every submit must carry `u.hold` (`-MTL_EINVAL`, reason `HOLD_REQUIRED`), which is what makes running without results safe (05 §5.4).
3. Unchanged: results are published in submission order from a ring of pool size; a non-zero `u.cookie` without results is `-MTL_EINVAL` (`COOKIE_WITHOUT_RESULTS`); `MTL_SESSION_EXPORT_POOL` implies `RESULTS | MT_SUBMIT`.
4. Early source release (`TX_SOURCE_RELEASED`, Phase 4) becomes the session flag `MTL_SESSION_EARLY_RELEASE` in the dynamic/early-release extension; `RX_MISSING` is gone (P3).

```c
sc.completion.mode = MTL_COMPLETE_ALL;    /* before, plus the DEFAULT/forced/exception table and optional_kinds */
sc.flags |= MTL_SESSION_RESULTS;          /* after; only needed on library memory */
```

**Savings.** −1 enum (4 values), −1 struct, −2 optional-kind bits, and the defaults table drops from four provisioning cases to one rule. `mtl_session_info.completion_mode` becomes a `results` bit.

**CUT – needs approval: `MTL_COMPLETE_EXCEPTIONS`.** It only applies to library pools, and an app that wants per-unit late identity must keep reaping in EXCEPTIONS too (exceptions fill the same ring).
It saves reap bandwidth, not code: 60 × 96 B/s at 60 Hz video, 96 kB/s at 1 kHz audio. With results off, lateness stays visible in counters and coalesced EQ events.
It is not a current MTL mode (today's `notify_frame_late` maps to results or events), but D-35 and the `FI_SELECTIVE_COMPLETION` row of 12 §2 name it.

**Risk.** Low. The forced-ALL behaviour is unchanged for imported memory; the forward pool loses nothing (holds were already required for its NONE exception). **Verdict: RECOMMEND.**

### 2.7 P7 — Progressive, MXL by index, dynamic pools and packet chunks (H7)

**Progressive rows → extension header `mtl_unified_rows.h`.**

- TX needs no verb of its own: in an `MTL_UNIT_ROWS` session the first `mtl_tx_submit` fixes the slot and media time, and every later submit of the same lease with a larger `u.used` publishes rows `[0, used)` with release semantics; `used == rows` is FINAL. The other per-use fields must equal the first submit's (`-MTL_EINVAL` otherwise). `mtl_tx_publish` goes.
- RX: dequeue returns the unit at the first slice with `u.used` = ready rows and `MTL_UNIT_PARTIAL`. The extension adds `mtl_rx_wait_rows(s, lease, min_rows, &rows, t)` and the coalesced `RX_PROGRESS` record for shared-CQ readers. `mtl_session_row_deadline` goes to L4. `progressive_late` stays in the video config.
- The core keeps only `MTL_UNIT_ROWS` and the `used` rule.

```c
/* ex08 after: no publish verb, no submission struct */
ret = mtl_tx_acquire(s, &u, MTL_MS(20));
if (ret < 0) return ex_fail("acquire", ret);
mtl_tx_next_slot(s, &hint);
u.media_index = hint.next_media_index;
for (uint32_t done = 0; done < u.plane[0].rows;) {
  uint32_t next = done + STEP > u.plane[0].rows ? u.plane[0].rows : done + STEP;
  render_rows(&u, done, next);
  u.used = next;                                  /* first submit fixes the slot; later ones publish */
  ret = mtl_tx_submit(s, &u);
  if (ret < 0) { if (done == 0) mtl_tx_release(s, u.lease); return ex_fail("submit", ret); }
  done = next;
}
```

**MXL by index → stays in core as one pool flag.** `MTL_POOL_RX_BY_INDEX` (slot = media index mod count) costs one bit; with `u.slot` and the attach pitch/offset the whole MXL bridge is a dequeue loop:

```c
ret = mtl_rx_dequeue(s, &u, MTL_MS(100));          /* ex06 after */
if (ret == 0 && (u.flags & MTL_UNIT_INDEX_VALID))
  mxl_commit_grain(u.slot, u.media_index, u.status == MTL_RX_COMPLETE);
if (ret == 0) ret = mtl_rx_release(s, u.lease);
```

**Dynamic pools and early release → extension header (Phase 4).** `mtl_tx_acquire_layout` and `MTL_SESSION_EARLY_RELEASE` with the `TX_SOURCE_RELEASED` record. They are Phase 4 features and do not belong in the 0.1 core.

**Packet-chunk units (the late requirement; S8 owns the mode).** The same unit, verbs and records carry a chunk of N packets, because a packet slot is a row:

| Unit field | Packet-chunk meaning (session `unit = MTL_UNIT_PACKET_CHUNK`) |
|---|---|
| `plane[0]` | packet slots: `addr` = first slot, `stride` = slot pitch, `row_bytes` = max packet bytes, `rows` = N (`chunk_packets`); optional `plane[1]` = header slots for header/payload split |
| `used` | packets in the chunk (TX in, RX out): the same "rows of plane 0" rule as progressive |
| `meta` | `struct mtl_meta_hdr { kind = MTL_META_PACKETS, count = used }` followed by N packet descriptors (S8 defines them: TX `len`, leg mask, flags; RX `len`, `leg`, flags, extended `seq`, `arrival_tai_ns`); at 16 B each, 64 packets = 1 KiB of meta |
| `media_index`, `media_tai_ns`, `launch_tai_ns`, `flags` | chunk-level timing: the media time of the chunk's first packet, `MTL_SUBMIT_LAUNCH_*` for exact or not-before launch; S8 decides how app-built RTP timestamps relate |
| `rtp` | RX: the first packet's RTP timestamp |
| `status`, `missed_before` | RX: COMPLETE (N packets) or INCOMPLETE (chunk closed at its due time with `used < rows`); chunks the pool could not take |
| `hold` | RTP-level forwarding: TX chunk slots over an RX chunk pool region, zero-copy, as for frames |
| Results | one TX core/full result per chunk; one RX record per chunk; legs and path in the full record |

The tasklet still only places packets and publishes with a store + fence; a chunk is just a unit that closes on N packets or its due time. **Verdict: RECOMMEND.**

### 2.8 P8 — Pool configuration as flags

**Change.** Five 2- and 3-valued enums become flag bits; `count` stays:

```c
struct mtl_pool_config {           /* 48 B, same size */
  uint32_t count;                  /* 0 = the essence default (09 §9) */
  uint32_t reserved0;
  uint64_t flags;                  /* MTL_POOL_* */
  uint64_t reserved[4];
};
/* MTL_POOL_ATTACHED 0x1 (slots only from mtl_session_attach, none allocated at create), _DYNAMIC 0x2 (Phase 4), _REQUIRE_DIRECT 0x4,
   _RX_BY_INDEX 0x8 (slot = media_index mod count), _RX_LATEST 0x10 (was RECLAIM_OLDEST_READY), _RX_NO_FILL 0x20 */
```

- `ALLOW_COPY` and `PREFER_DIRECT` merge into the default: "direct whenever the pool and format allow it, otherwise copy, always reported in `path`". 05 §6.1 gives them no observable difference beyond "the fallback is counted", and both are counted.
- **CHANGE – needs approval: zero-fill becomes the default for app-memory RX pools too** (today NONE for attached). It writes only inside the slot the app gave for RX and only on incomplete units, and it matches the repository rule that gaps must read as zeros. `MTL_POOL_RX_NO_FILL` opts out (MXL bridges that check `status`).

**Savings.** −5 enums, 13 constants → 6 bits; the 09 defaults table loses three "0 means …" rows. **Verdict: RECOMMEND.**

### 2.9 P9 — Submit always consumes the lease (except `-MTL_EAGAIN`)

**Change.** On success, access moves to MTL (unchanged). On any failure other than `-MTL_EAGAIN` (inline-safe trylock contention, 04 §3.3) the lease returns to FREE with no result, and `-MTL_ESTALE` stays a no-op. Every loop loses `if (ret < 0) mtl_tx_release(...)`, and the leaked-lease stall of C5 §3.2 (`BLOCKED_APP_LEASES`) can no longer come from a failed submit.
**Cost.** "Fix and resubmit" without re-rendering (an oversize ST22 codestream, a bad media index) needs a new acquire and a re-fill. No current MTL flow relies on it. The two-tier rule (EAGAIN keeps, everything else releases) is one more thing to learn. **Verdict: OPTION** (Q2).

### 2.10 P10 — Region calls in an extension header

The 7 `mtl_mem_*` calls, `struct mtl_mem_desc`, `struct mtl_mem_info`, `enum mtl_backing` and the access/mapping flags serve three cases: one registration shared by several sessions, `mtl_mem_alloc`, and explicit mapping before DYNAMIC.
With P4 a framework pool, an MXL ring and a forwarder need none of them. Moving them to `mtl_unified_mem.h` (`mtl_region_h` stays in core, `struct mtl_attach` names it) takes ≈70 lines out of the core.
**Verdict: OPTION**: a leaner core for one more include in the advanced cases.

## 3. Proposed data-path verbs and struct sketches

### 3.1 Verbs

| Verb | Class | Header |
|---|---|---|
| `void mtl_unit_init(struct mtl_unit* u)` | DP | core |
| `int mtl_tx_acquire(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns)` | WT | core |
| `int mtl_tx_acquire_slot(mtl_session_h s, uint32_t slot, struct mtl_unit* u, int64_t timeout_ns)` (`-MTL_EBUSY` if not FREE with timeout 0) | WT | core |
| `int mtl_tx_submit(mtl_session_h s, const struct mtl_unit* u)` (ROWS: re-submit publishes) | DPC | core |
| `int mtl_tx_release(mtl_session_h s, mtl_lease_h lease)`, `int mtl_tx_withdraw(mtl_session_h s, uint32_t slot)`, `int mtl_tx_hold(mtl_session_h s, uint32_t slot, uint32_t on)` | DP | core |
| `int mtl_tx_reap(mtl_session_h s, void* res, size_t res_size, uint32_t max, int64_t timeout_ns)` (96 B core or 216 B full) | WT | core |
| `void mtl_slot_hint_init(struct mtl_slot_hint* h)`, `int mtl_tx_next_slot(mtl_session_h s, struct mtl_slot_hint* h)` | DP | core |
| `int mtl_rx_dequeue(mtl_session_h s, struct mtl_unit* u, int64_t timeout_ns)` | WT | core |
| `int mtl_rx_release(mtl_session_h s, mtl_lease_h lease)` (any thread, any order) | DP | core |
| `int mtl_rx_get_result(mtl_session_h s, mtl_lease_h lease, void* rec, size_t rec_size)` (DP); `int mtl_rx_get_missing(mtl_session_h s, mtl_lease_h lease, struct mtl_rx_missing_range* r, uint32_t max)` (DPC, count ≥ 0) | DP / DPC | core |
| `void mtl_attach_init(struct mtl_attach* a)`, `int mtl_session_attach(mtl_session_h s, const struct mtl_attach* a)`, `int mtl_session_detach(mtl_session_h s)` | CP | core |
| `int mtl_session_get_slot(mtl_session_h s, uint32_t slot, struct mtl_unit* u)` (static part; lease null) | DP | core |
| `int mtl_session_get_pool_region(mtl_session_h s, mtl_region_h* out)`, `mtl_session_get_buffer_requirements`, `mtl_buffer_requirements_init` (unchanged) | CP | core |
| `mtl_mem_desc_init`, `mtl_mem_info_init`, `mtl_mem_alloc`, `mtl_mem_import`, `mtl_mem_destroy`, `mtl_mem_get_info`, `mtl_mem_map_device` | CP | core (P10: extension) |
| `static inline uint32_t mtl_lease_slot(mtl_lease_h l)` | AS | core |
| `int mtl_rx_wait_rows(mtl_session_h s, mtl_lease_h lease, uint32_t min_rows, uint32_t* rows, int64_t timeout_ns)` + `struct mtl_rx_progress` | WT | `mtl_unified_rows.h` |
| `int mtl_tx_acquire_layout(mtl_session_h s, const struct mtl_attach* one, struct mtl_unit* u, int64_t timeout_ns)` + `MTL_SESSION_EARLY_RELEASE`, `struct mtl_tx_source_released` | WT | `mtl_unified_dynamic.h` (Phase 4) |
| `mtl_tx_write(s, data, bytes, const struct mtl_unit* how, t)`, `mtl_tx_send_slot(s, slot, const struct mtl_unit* how, t)`, `mtl_unit_copy_in/out(u, plane, offset, ptr, len)`, `mtl_session_row_deadline`, `mtl_rx_align` | WT / DPC / DP | L4 (`mtl_simple.h` or a sibling `mtl_util.h`), implemented only over core verbs |

Inline-safe subset (C10) becomes: `mtl_tx_acquire`, `mtl_tx_acquire_slot`, `mtl_rx_dequeue` and `mtl_tx_reap` with timeout 0, `mtl_tx_submit`, `mtl_tx_release`, `mtl_rx_release`, `mtl_tx_next_slot`, `mtl_rx_get_result` and the DP getters.

### 3.2 Structs

```c
/* The unit you hold: output of acquire/dequeue, input of submit. 208 B, no implicit padding. */
struct mtl_unit {
  uint32_t struct_size;                        /* in: mtl_unit_init(); never rewritten */
  uint32_t plane_count;                        /* out */
  mtl_lease_h lease;                           /* out: the access token */
  uint32_t slot;                               /* out: pool slot, stable for the life of the attach */
  uint32_t status;                             /* RX out: enum mtl_rx_status; TX: 0 */
  struct mtl_plane_view plane[MTL_MAX_PLANES]; /* out: addr, stride, row_bytes, rows */
  MTL_ADDR(void) meta;                         /* out: the slot's meta area (struct mtl_meta_hdr first) */
  uint32_t meta_capacity;                      /* out */
  /* per use: TX in (zeroed by acquire), RX out */
  uint32_t used;                               /* bytes if plane[0].rows == 1, else rows of plane 0; TX 0 = all */
  uint32_t flags;                              /* TX: MTL_SUBMIT_* (low 16 bits); RX: MTL_UNIT_* (high 16 bits) */
  uint32_t rtp;                                /* TX: with MTL_SUBMIT_RTP (PASSTHROUGH); RX: received */
  int64_t media_index;                         /* TX: INDEX sessions; RX: with MTL_UNIT_INDEX_VALID */
  int64_t media_tai_ns;                        /* TX: TAI sessions; RX: with MTL_UNIT_TAI_VALID */
  uint64_t cookie;                             /* TX: returned in the result; RX: 0 */
  mtl_lease_h hold;                            /* TX: an RX lease this unit reads (1 -> N); RX: null */
  int64_t launch_tai_ns;                       /* TX: with MTL_SUBMIT_LAUNCH_NOT_BEFORE / _EXACT */
  uint32_t missed_before;                      /* RX: units the pool could not receive since the last delivery */
  uint32_t reserved0;
  uint64_t reserved[2];                        /* growth: device handles (later), ... */
};
/* TX submit flags (low 16 bits): MTL_SUBMIT_DISCONTINUITY 0x1, _RTP 0x2, _LAUNCH_NOT_BEFORE 0x4, _LAUNCH_EXACT 0x8 (non-compliant).
   RX unit flags (high 16 bits, masked off when a received unit is used as a send template): MTL_UNIT_INDEX_VALID 0x10000,
   _TAI_VALID 0x20000, _USED_REDUNDANCY 0x40000, _DISCONTINUITY 0x80000, _FORMAT_CHANGED 0x100000, _PARTIAL 0x200000 (extensions). */

/* At the start of every meta area, TX and RX alike. 16 B. */
struct mtl_meta_hdr {
  uint16_t kind;                               /* MTL_META_NONE, _ANC, _USER, _PACKETS */
  uint16_t tag_version;                        /* USER: the sender's version */
  uint32_t count;                              /* ANC: packets (<= 255); PACKETS: descriptors */
  uint32_t bytes;                              /* payload bytes after the header */
  uint32_t tag;                                /* USER: 0 = untagged (wire-compatible), else the app tag */
};
/* ANC: struct mtl_anc_packet[count] follows (unchanged, 16 B each); USER: bytes of user meta follow. */
/* struct mtl_slot_hint: unchanged (32 B), now filled by mtl_tx_next_slot(). */

/* CQ records: header unchanged (48 B); union unchanged (256 B: hdr, tx full, rx full, raw). */
struct mtl_tx_result {                         /* 96 B core: what P1/P2 read */
  struct mtl_cq_entry_hdr hdr;                 /* status, flags, time_valid, session, lease, user_cookie, seq */
  uint32_t reason;                             /* enum mtl_tx_reason */
  int32_t error;                               /* MTL_E* for FAILED */
  int64_t media_index;                         /* MTL_TT_MEDIA */
  int64_t media_tai_ns;                        /* MTL_TT_MEDIA */
  int64_t margin_ns;                           /* MTL_TT_MARGIN: deadline - submitted */
  uint32_t rtp;
  uint32_t reserved[3];
};
struct mtl_tx_result_full {                    /* 216 B */
  struct mtl_tx_result r;
  int64_t submitted_tai_ns, deadline_tai_ns, scheduled_first_tai_ns, enqueued_first_tai_ns;
  int64_t observed_first_tai_ns[MTL_MAX_LEGS], observed_last_tai_ns, pickup_slack_ns;
  int32_t snap_error_ns, max_packet_lateness_ns;
  uint32_t timing_flags, slots_skipped_before, samples_padded, samples_dropped, samples_inserted;
  uint32_t leg_count, pkts_sent[MTL_MAX_LEGS];
  uint8_t leg_reason[MTL_MAX_LEGS];
  uint16_t reserved0;
  uint32_t path, pkts_dma, pkts_copied_partial;
};
struct mtl_rx_result {                         /* 96 B core, CQ record for shared-CQ readers */
  struct mtl_cq_entry_hdr hdr;                 /* status, MTL_RX_F_USED_REDUNDANCY, lease = the lease now held */
  int64_t media_index;                         /* MTL_RT_INDEX */
  int64_t media_tai_ns;                        /* MTL_RT_MEDIA */
  uint32_t rtp, used, missed_before;
  uint32_t reserved[5];
};
struct mtl_rx_result_full {                    /* 208 B */
  struct mtl_rx_result r;
  int64_t arrival_first_tai_ns[MTL_MAX_LEGS], arrival_last_tai_ns[MTL_MAX_LEGS];
  int64_t presentation_tai_ns, delivered_tai_ns;
  uint32_t timing_flags;
  int32_t media_phase_ticks;
  uint32_t units_missing_before, units_before_start;
  uint32_t leg_count, pkts_expected, pkts_received[MTL_MAX_LEGS], pkts_recovered;
  uint32_t format_changed, missing_count, path, pkts_dma, pkts_copied_partial, reserved0[2];
};
```

`struct mtl_attach` and `struct mtl_pool_config` are in §2.4 and §2.8; `struct mtl_plane_view`, `struct mtl_anc_packet`, `struct mtl_rx_missing_range`, `struct mtl_buffer_requirements` and the CQ header stay as they are. Multi-field declarations above are shorthand; the header would keep one field per line for `-Wpadded` review.

### 3.3 Net effect on the four sections

| | Revision 3 | Proposed core | Proposed extensions + L4 |
|---|---|---|---|
| Exported functions (incl. lease helpers, session memory calls, requirements) | 43 | 28 | 8 |
| Structs + unions | 22 + 1 | 16 + 1 (`unit`, `meta_hdr`, `attach`, `pool_config`, `plane_view`, `plane_requirements`, `buffer_requirements`, `mem_desc`, `mem_info`, `slot_hint`, `cq_entry_hdr`, `tx_result`, `tx_result_full`, `rx_result`, `rx_result_full`, `rx_missing_range`) | 2 (`rx_progress`, `tx_source_released`) |
| Enums | 16 | 9 (`mem_domain`, `backing`, `path_kind`, `convert_context`, `cqe_kind`, `tx_status`, `tx_reason`, `rx_status`, `rx_reject`) | — |
| CQ record kinds | 5 | 2 | 2 |
| Lines (estimate) | 568 | ≈400 | ≈70 |

## 4. Cuts and changes needing approval

| Item | Kind | What is lost | Mitigation |
|---|---|---|---|
| `mtl_buffer_h` and the buffer object API (P4 part 2) | CHANGE – needs approval; touches C5 §2.5 / D-01 / D-07 / D-55 wording | a free-standing buffer object; the explicit second type the owner listed | slots are `uint32_t`, the lease stays `mtl_lease_h`; swapping them is still a compile error. Fallback: keep the handle and add only `mtl_session_attach` (P4 "keep" variant) |
| `MTL_COMPLETE_EXCEPTIONS` (P6) | CUT – needs approval | results only for non-ON_TIME units on library pools | `MTL_SESSION_RESULTS` and ignore ON_TIME; counters and coalesced EQ events with results off. Not a current MTL mode |
| `PREFER_DIRECT` as a third data-path value (P8) | CUT – needs approval | a policy name with no observable difference from the default | the default is "direct when possible, reported"; `REQUIRE_DIRECT` unchanged |
| Zero-fill default for app-memory RX pools (P8) | CHANGE – needs approval (Q-MEM-6) | the "NONE for attached" default | `MTL_POOL_RX_NO_FILL` |
| `mtl_rx_transfer` (P5) | CUT (sugar) – needs approval | a one-call 1 → 1 forward | `mtl_tx_send_slot(tx, in.slot, &how /* hold = in.lease */, 0)`; no use case lost |
| `MTL_CQE_RX_MISSING` follow-on record (P3) | CUT – needs approval | the missing table in the CQ stream for shared-CQ readers | `mtl_rx_get_missing` while the lease is held (shared-CQ readers hold it after reading the record) |
| ANC copy-at-submit (P2) | CHANGE – needs approval | submit-time snapshot of the packet table | pick-up bounds check, or keep a submit-time snapshot of the meta area (Q4) |
| Submit failure releases the lease (P9) | OPTION – needs approval if adopted | "fix and resubmit" without re-render | `-MTL_EAGAIN` keeps the lease |

Hard constraints: no app call runs on or blocks a tasklet, and completion stays a store + fence (`get_missing` scans on the app thread; the only tasklet-side addition is P2's optional ANC bounds check inside packetisation).
Results are never lost and stay in submission order; zero-copy stays a policy (`MTL_POOL_REQUIRE_DIRECT`); imported memory always produces results (P6 rule 1).

## 5. Coverage check: every current MTL use case

| Use case (today) | Revision 3 | Proposed |
|---|---|---|
| Frame TX/RX (`st20p`, `st22p`, `st30p`, `st40p`) | acquire/submit, dequeue/release | same verbs with the unit |
| Slice / line TX (`ST20_TYPE_SLICE_LEVEL`, `query_frame_lines_ready`), incl. with ext frames | ROWS + `publish`, `acquire_buffer` | ROWS extension: re-submit publishes; `acquire_slot` for a named slot |
| RTP passthrough (`tsmode = PRES`, `rtp_timestamp` override) | `rtp_override` + `MTL_SUB_RTP` | `u.rtp` + `MTL_SUBMIT_RTP` |
| RTP-level / packet-chunk sessions (now first-class, S8) | reserved `MTL_UNIT_PACKET_CHUNK` | the same unit: packet slots as rows of plane 0, descriptors in the meta area (§2.7) |
| Ext frames (`st20p_tx_put_ext_frame`, `st20_tx_set_ext_frame`; RX `ext_frames[]`, `query_ext_frame`) | import + buffers + attach + `acquire_buffer`; RX attached pool (+ `BY_INDEX`) | `mtl_session_attach` + `acquire_slot` / `send_slot`; RX attach (+ `MTL_POOL_RX_BY_INDEX`) |
| Moving-cursor ext frames | `acquire_dynamic` (Phase 4) | `acquire_layout` (Phase 4 extension) |
| Interlaced, woven frames | two field buffers over one region | `slot_offset[]` in one attach |
| ST 2022-7; RX DMA offload and path reporting | legs, `path`, `pkts_dma` in records | unchanged in the full records and `mtl_session_get_info`; `MTL_UNIT_USED_REDUNDANCY` in the unit |
| MXL rings | attach + `BY_INDEX` + `buffer_index(lease_buffer())` | attach with pitch/offset (+ meta in slot) + `u.slot` |
| 1 → N forwarding (split-forward, `rx_st20p_tx_st20p_fwd`) | pool region + buffers + `.hold` | pool region + attach + `u.hold`; no reap loop needed |
| User meta (ST20 `user_meta`), ANC packet tables | submission pointer on TX, meta area on RX | meta area both ways, typed header |
| Audio arbitrary byte runs (FFmpeg/GStreamer re-framing) | `mtl_tx_write` | L4 `mtl_tx_write` over acquire/submit |
| Display-while-sending (new in r3) | `buffer_hold` | `mtl_tx_hold(s, slot, on)` |
| Exported pools (GStreamer `propose_allocation`) | `EXPORT_POOL` | unchanged (implies `RESULTS` and `MT_SUBMIT`) |

## 6. Open questions

1. **Unit copy vs pointer (P1).** Spike S3 should time acquire/dequeue writing the 208 B unit against the pointer form at 0.5 M units/s and 512 sessions per scheduler. If the copy shows up, a per-session `MTL_SESSION_UNIT_PTR` could return a library-owned pointer for single-threaded apps.
2. **Submit consumes the lease on failure (P9)?** Adopt or not.
3. **Retire `mtl_buffer_h` (P4 part 2)?** The recommendation depends on it; the fallback keeps the handle and still adds `mtl_session_attach`.
4. **ANC/meta TOCTOU (P2):** builder bounds check at pick-up (no copy) or submit-time snapshot (today's cost)?
5. **Zero-fill default for app-memory RX pools (P8):** acceptable for MXL bridges, or keep NONE there?
6. **One release verb?** `mtl_lease_release(lease)` for both directions saves one function; keep the symmetric `tx_`/`rx_` names (D-34)?
7. **Where do L4 helpers live:** `mtl_simple.h` (today's L4) or a sibling `mtl_util.h`, so `mtl_simple.h` stays the seven-call first-program header?
8. **Missing ranges in v1 over `st20p` (P3):** `mtl_rx_get_missing` returns `-MTL_ENOTSUP` until L2 owns the per-lease bitmap; acceptable, given revision 3 promises `missing[]` that `st20p` cannot fill either?
9. **Coordination.** S8: the packet-descriptor layout and how app-built RTP timestamps relate to `media_*` and `launch_tai_ns`. Timing pass: `launch` as flags, the slot hint as a getter, `row_deadline`/`rx_align` in L4. CQ/EQ pass: the union keeps `hdr`, `tx`, `rx`, `raw`; extension kinds are cast from `raw`.
10. **`withdraw` and `hold` in core or in an extension?** Neither is a current MTL use case (playlist edit, display-while-sending); each is one DP function.
