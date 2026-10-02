# PR #1610 unified session API: consolidated design review

> Input document. Written by the maintainer before this planning effort and copied here
> unchanged so that the research notes and design documents can cite it. Section numbers
> below are referenced elsewhere as "review §N".

**Review target:** PR #1610, head
`14a1f80ccc6ec8d2ae021a2baad8e054564dc000`

**MTL implementation baseline:** `bf58f6e9da017114ae4af030c9b3cc111a68a8d9`

**Research date:** 21 September 2026

**Recommendation:** preserve the unified-session direction, but revise the public buffer,
memory, timing, completion, and lifecycle contract before stabilization

## Executive recommendation

The PR has a valuable foundation:

- media-specific session creation;
- one opaque session concept;
- common lifecycle, waiting, statistics, and notification infrastructure;
- media backends hidden behind a small internal interface.

The public API should not stabilize the proposed binary buffer ownership choice:

```c
MTL_BUFFER_LIBRARY_OWNED
MTL_BUFFER_USER_OWNED
```

MTL needs both convenient library allocation and application-imported memory, but they
should not be separate operational modes. “Ownership” currently mixes five independent
questions:

```text
Who allocated the storage?
Who owns the storage lifetime?
Who may access it now?
How was it registered for DMA?
Is the current path direct, copied, or converted?
```

The recommended decision is:

> Keep multiple allocation and registration mechanisms, but expose one buffer handle,
> one access-ownership state machine, and one submit/dequeue contract.

The difference should end at provisioning:

```text
MTL default pool ------------------\
MTL explicit allocation -----------+--> buffer handle --> common runtime API
application memory import ---------+
DMA-BUF/GPU/device memory import ---/
```

For an achievable first implementation, sessions may use a fixed pool established before
start. MTL can populate it automatically, or the application can attach imported buffer
handles. Mixed and dynamic pools can be capability-gated later without changing the
basic API.

The same redesign should also:

1. attach media time to a submission, not reusable storage;
2. separate media time, RTP timestamp, packet launch, and observed timestamps;
3. use one rational timeline for synchronized audio/video;
4. model video lines as progressive availability within one timed frame/field;
5. make reliable completion the authority for buffer reuse;
6. use reference-counted memory regions with explicit layouts and data-path policy;
7. define behavior as observable guarantees that map directly to contract tests;
8. initially adapt existing ST20P/ST22P/ST30P/ST40P paths instead of replacing their
   packet engines.

## 1. Scope and evidence

This review covers:

- the 16 commits and design documents in PR #1610;
- `include/mtl_session_api.h` and all `lib/src/new_api/` code in the PR;
- current ST20/ST22/ST30 pipeline and low-level memory, pacing, and timestamp paths;
- current MTL DMA mapping and external-frame handling;
- NVIDIA's public Rivermax product and FAQ pages;
- the public Rivermax low-level examples;
- the open Rivermax Dev Kit memory, media-unit, chunk, and stream wrappers.

The complete Rivermax SDK header/manual is distributed with the licensed SDK. Statements
about low-level behavior below are limited to public NVIDIA examples and open Dev Kit
source. Where the public material does not establish a lifetime guarantee, this report
does not infer one.

## 2. The central design decision: do we need two buffer modes?

### Short answer

No binary public ownership mode is needed.

The API needs at least two **provisioning paths**:

1. MTL prepares suitable storage automatically.
2. The application imports storage and describes its layout.

After provisioning, both should become the same opaque `mtl_buffer_handle` and obey the
same runtime calls, timing, progress, completion, and validation rules.

| Public design | Benefit | Drawback | Decision |
|---|---|---|---|
| separate library/user session modes | superficially simple implementation branches | duplicates lifecycle, conflates allocation with directness, and scales poorly to GPU/DMA-BUF/mixed memory | reject |
| one handle, fixed pool only | simple hot path, bounded resources, validates/registers before start | cannot accept arbitrary new allocations while running | required v1 baseline |
| one handle, fixed pool plus dynamic provide/submit | accommodates codecs, cameras, and framework-owned pools | more lifetime, admission, and capability complexity | optional later capability |

### Why “library-owned” is misleading

In a library-allocated TX pool, the application temporarily receives exclusive writable
access after `acquire()`. In an RX pool, it temporarily receives exclusive readable
access after `dequeue()`. Allocation ownership remains with MTL, but access ownership
moves repeatedly.

```text
MTL owns allocation lifetime
          |
          +--> app owns writable access
          +--> MTL owns transport access
          +--> app owns readable access
```

Calling the buffer “library-owned” hides the access lease, which is the part correctness
depends upon.

### Why “user-owned” is misleading

Application allocation does not imply:

- direct NIC DMA;
- zero-copy;
- application access while the buffer is in flight;
- application-managed registration;
- one-plane CPU memory;
- a different submit operation.

An imported buffer may be copied because its format, alignment, memory domain, topology,
or backend is incompatible with direct transport. It may instead be converter input
while another buffer is transmitted. Its allocation lifetime remains the application's,
but its access must still transfer exclusively to MTL during an accepted operation.

### The independent axes

| Axis | Possible values |
|---|---|
| allocation origin | MTL pool, MTL explicit allocation, host import, DMA-BUF, GPU/device import, pre-mapped expert import |
| allocation lifetime owner | session, explicit MTL object, application/native object |
| current access owner | pool, application, queued, converter, encoder, NIC/device, application again |
| registration | MTL automatic, imported native registration, application pre-registration |
| layout | packed, planar, separate allocations, header/payload split, progressive extent |
| execution path | direct, conversion-direct, copy, copy+convert |
| provisioning policy | fixed pool, mixed pool, dynamic submit/provide |

A two-value ownership enum cannot represent this matrix. Independent typed policies can.

## 3. Where the two provisioning paths are useful

### MTL-prepared storage

Library preparation should remain the default because it is valuable for:

- minimal examples and applications that only want complete media units;
- correct NUMA placement near the selected NIC;
- hugepage, alignment, IOVA, and backend-specific allocation requirements;
- automatic pool sizing and bounded backpressure;
- CPU frame generators with no existing allocator;
- audio and ancillary applications where imported memory offers little benefit;
- internal staging and format conversion;
- a reliable fallback when direct access to imported storage is impossible;
- tests and benchmarks that need a known memory configuration;
- portability across DPDK, AF_XDP, sockets, and future backends.

The application should not need to understand registration keys or IOVA to send a normal
frame.

### Application-imported storage

Imported memory is essential for:

- camera and capture-card pools;
- decoder and encoder surfaces;
- FFmpeg/GStreamer or another framework's existing buffer pool;
- CUDA, Level Zero, FPGA, DPU, or other accelerator memory;
- DMA-BUF and operating-system native handles;
- shared-memory IPC and process-to-process media pipelines;
- deterministic application-wide memory budgets;
- RX placement directly into the next processing stage's storage;
- RX-to-TX forwarding without an intermediate application copy;
- line-based producers whose renderer writes progressively;
- one large registration partitioned across several streams;
- applications that require a direct-only path and can satisfy its constraints.

Imported memory costs more validation, synchronization, and lifetime management. The API
should make those costs explicit rather than calling every imported path zero-copy.

### Mixed storage

Mixed pools and multi-region buffers are useful for:

- host-visible packet headers with GPU-resident payload;
- separate allocations for video planes;
- imported source plus internal conversion/transport destination;
- gradual migration from internal to imported buffers;
- fallback capacity when an external producer temporarily withholds buffers;
- sessions shared by producers with different memory domains.

Mixed operation is not required in the first implementation. The public object model
should permit it, and a backend should advertise whether it supports:

```text
imported buffers
mixed origins
dynamic attach/detach
direct access
header/payload separation
specific memory domains
```

## 4. What PR #1610 currently does

The PR makes ownership a session-wide choice:

```text
LIBRARY_OWNED:
    buffer_get() -> buffer_put()

USER_OWNED:
    mem_register() -> buffer_post(data, size, context) -> event
```

This creates different APIs and result paths for storage that should differ only in
provisioning.

### 4.1 “User-owned” is not consistently zero-copy

On TX, a posted user buffer is direct only when application and transport formats match.
When conversion is required, it is read into an internal transport buffer.

On default RX `buffer_post()`, MTL receives into an internal frame and then copies or
converts into the posted application address. Direct RX requires a separate
`query_ext_frame` callback path.

The same public mode therefore means:

```text
direct TX
or converter-source TX
or copied RX
or callback-selected direct RX
```

Those paths have different performance and lifetime points. They should be selected and
reported as data paths, not hidden behind ownership.

### 4.2 `buffer_post(data, size, context)` is not a media-buffer description

It cannot describe:

- multiple planes or allocations;
- plane offset, span, row width, stride, and row count;
- memory domain and device identity;
- valid samples or bytes;
- producer/consumer fences;
- progressive line readiness;
- media time, duration, exact launch, or RTP override;
- direct-only versus copy-allowed intent.

Adding more flags around this function would create combinations without fixing its
fundamental lack of identity and layout.

### 4.3 Registration is not a retained memory object

The PR's `mem_register()` mainly stores address/size and attempts to discover an IOVA. It
does not generally call the existing DMA-map machinery. The registration record has no
in-flight reference count. `mem_unregister()` frees the record without proving that no
queued frame, converter, mbuf, or NIC descriptor can still access the region.

A registered region must be an opaque retained object. Destroy/unregister must return
`-EBUSY` while any buffer or operation references it.

### 4.4 Accepted ownership can be lost

The user-buffer comments promise one `BUFFER_DONE` for every successful post, but the
common event ring may drop events when it is full. A lost informational event is
recoverable; a lost ownership completion permanently loses the application buffer.

Reliable completions must use retained queue entries or backpressure. They cannot share
drop-on-full semantics with informational events.

### 4.5 Progress and order depend on implementation accidents

Accepted user TX buffers may be held in a backlog that advances only during later
`buffer_post()` calls. The last accepted buffer can remain stranded if the application
has nothing more to submit.

Binding is FIFO, but transport scans for the lowest free frame-slot index. Pool indices
can therefore change transmission order. Ordering must be a declared policy, never an
artifact of slot selection.

### 4.6 Metadata does not reliably reach the transport callback

The PR writes timing metadata into `frame->tv_meta`, while its
`video_tx_get_next_frame()` ignores the metadata output parameter. The low-level path
then assigns its callback metadata over `frame->tv_meta`. A callback-boundary contract
test is required; application pacing and metadata cannot be assumed to survive.

### 4.7 Public ABI and lifecycle remain under-specified

Large public buffer/config/event/stat structures have no general `struct_size` evolution
contract. Destroy does not yet have a complete call/callback guard. Start/stop behavior,
blocked waits, drain versus abort, outstanding completion handling, and calls racing
destroy require normative definitions before ABI stabilization.

## 5. Current MTL external-memory behavior

Current MTL already contains useful pieces, but they do not form one contract.

### Low-level ST20

- MTL normally allocates NUMA-local transport frames.
- External TX skips allocation and uses application address/IOVA after per-slot
  validation.
- Low-level direct completion is tied to the final mbuf external reference, which is a
  meaningful safe-to-reuse point.
- RX supports MTL frames, a static external array, or dynamic `query_ext_frame`.

### ST20 pipeline

- Matching-format external TX may directly transmit imported storage.
- Conversion uses the external frame only as input and transmits an internal frame.
- Conversion may stop reading the source before network transmission finishes.
- Manual external release manages a pipeline slot and has path-dependent behavior.
- RX external memory may be selected statically or through a nonblocking datapath
  callback.

### Other essences and general DMA mapping

ST30 and ST40 pipelines do not expose the same general imported-memory facility.
`mtl_dma_map()` has stricter alignment/IOVA constraints, currently maps only the primary
port in its implementation, and does not itself protect mapping lifetime with in-flight
references.

The new API should normalize these differences above the backends rather than expose
them as more public ownership modes.

## 6. How Rivermax models the same problem

Rivermax's useful design lesson is the separation of:

```text
allocation
    x registration
        x reusable chunk lifecycle
```

### 6.1 Three public memory configurations

NVIDIA's public Output Media and Input examples demonstrate:

| Configuration | Address supplied | Memory key | Responsibility |
|---|---:|---:|---|
| Rivermax-managed | no | invalid | Rivermax allocates and registers stream storage |
| application-allocated | yes | `RMX_MKEY_INVALID` | application allocates; Rivermax registers |
| application-allocated and registered | yes | valid `mkey` | application allocates and registers |

All configurations use the same stream/chunk operations. There is no separate
“external-buffer commit” API.

The registration examples also document a concrete reason for application registration:
one large registered allocation can be partitioned across multiple streams with one
memory key, improving hardware key-cache utilization compared with separate per-stream
registrations.

### 6.2 Persistent ring and common TX lifecycle

Output memory is configured as stream memory blocks with one or more sub-blocks. A
sub-block can hold a full packet or separate application header/payload memory.

```text
configured memory ring
        |
get_next_chunk()
        v
application-writable chunk
        |
commit_chunk(send_time)
        v
queued/transmitting
        |
ring position becomes free
```

Relevant low-level calls include:

- `rmx_output_media_init_mem_blocks()`;
- `rmx_output_media_get_sub_block()`;
- `rmx_output_media_assign_mem_blocks()`;
- `rmx_output_media_get_next_chunk()`;
- `rmx_output_media_commit_chunk()`;
- `rmx_output_media_cancel_unsent_chunks()`.

Backpressure is explicit through statuses such as `RMX_NO_FREE_CHUNK` and
`RMX_HW_SEND_QUEUE_IS_FULL`.

Tracked TX completion is optional and token-based. MTL's high-level unified API should be
stricter: reliable completion must be mandatory whenever it is required to return an
application buffer safely.

Rivermax also documents a surprising stream-level FIFO rule: after acquiring several
chunks, a commit sends the oldest acquired chunk, not necessarily the wrapper object on
which commit was called. MTL should retain explicit submission identity and avoid this
object/queue ambiguity.

### 6.3 RX provisioning

Rivermax RX memory is configured at stream creation. The application determines the
layout, may supply address and memory key, creates the stream, and retrieves received
chunks through `rmx_input_get_next_chunk()` and completion accessors.

The public examples do not show attaching an arbitrary new address per received chunk.
That supports a fixed-pool first implementation for MTL:

```text
provision/register once
attach a reusable pool
run one common dequeue/release lifecycle
```

The public examples process RX completion pointers synchronously and do not expose a
per-chunk release call. The licensed API documentation may define the exact reuse point.
MTL should state its own lease lifetime explicitly rather than inherit an undocumented
assumption.

### 6.4 Memory domains and higher-level media buffers

The Rivermax Dev Kit has allocators for regular memory, hugepages, GPU memory, and
GPU-host-pinned memory. It models host/GPU location and header/payload layouts. Its
`MediaUnitBuffer` abstraction supports internally allocated and external memory.

However, a Dev Kit `MediaUnit` is a higher media-layer object; external media-unit memory
may still be copied into a low-level Rivermax chunk. This reinforces an important rule:

> External allocation and direct NIC access are separate claims.

NVIDIA's product documentation separately advertises GPUDirect and header/payload split
for direct NIC/GPU data movement. MTL should likewise make the selected direct/copy path
observable rather than infer it from allocation origin.

### 6.5 What MTL should adopt and what it should improve

Adopt:

- allocation and registration independent from chunk operation;
- one runtime lifecycle over differently sourced memory;
- fixed reusable pools as the efficient baseline;
- explicit layout and memory keys/handles;
- header/payload or multi-region capability;
- explicit backpressure statuses;
- host, pinned, and device memory as real domains.

Improve:

- mandatory reliable completion for ownership-bearing submissions;
- object identity that matches the submitted unit;
- explicit safe-to-reuse semantics;
- normalized media time and A/V synchronization above the packet ring;
- explicit RX lease/release lifetime;
- provider-neutral memory handles rather than one-vendor memory keys.

## 7. Proposed object model

Four objects should remain separate:

```text
memory region
    where bytes live; registration and allocation lifetime
        |
        v
buffer
    immutable planes/spans/layout over one or more regions
        |
        v
access lease
    who may read/write the buffer now
        |
        v
submission/result
    media meaning, timing, progress, status, and user identity for one use
```

### Memory region

An opaque `mtl_memory_handle` represents:

- host pointer, DMA-BUF, device/provider handle, or expert pre-mapped memory;
- total length and access permissions;
- memory/device identity;
- mappings required by each active port/device;
- references from buffers and in-flight operations;
- optional synchronization/fence provider.

Raw IOVA should be an expert import, not the ordinary application contract.

### Buffer

An opaque `mtl_buffer_handle` has immutable layout:

```c
struct mtl_plane_desc {
  mtl_memory_handle memory;
  size_t offset;
  size_t span;
  uint32_t row_bytes;
  uint32_t stride;
  uint32_t rows;
};

struct mtl_buffer_desc {
  uint32_t struct_size;
  uint32_t plane_count;
  struct mtl_plane_desc planes[MTL_MAX_PLANES];
  size_t logical_capacity;
  void* user_opaque;
};
```

Explicit span and row count allow safe validation of packed, planar, and subsampled
formats. Planes may reference one shared region or different regions.

### Submission

Timing and progress are per-use values:

```c
struct mtl_tx_submission {
  uint32_t struct_size;
  uint64_t valid_fields;
  struct mtl_media_span media;
  const struct mtl_tx_overrides* overrides;
  const struct mtl_progress* initial_progress;
  void* user_opaque;
};
```

Reusing the same storage for another frame creates another submission; it does not mutate
the buffer's permanent identity or layout.

## 8. Proposed provisioning and runtime API

### 8.1 Default pool: simplest path

```c
struct mtl_session_buffer_config buffers = {
    .struct_size = sizeof(buffers),
    .default_buffer_count = 4,
    .path_policy = MTL_DATA_PATH_ALLOW_COPY,
};

mtl_video_session_create(mtl, &config, &session);
```

MTL allocates, maps, lays out, and destroys these buffers. Runtime remains:

```c
mtl_tx_acquire(session, timeout_ms, &buffer);
fill(buffer);
mtl_tx_submit(session, buffer, &submission);
mtl_tx_dequeue_completion(session, timeout_ms, &completion);
```

### 8.2 Imported fixed pool

```c
mtl_memory_import(mtl, &memory_desc, &memory);
mtl_buffer_create(&buffer_desc, &buffer);
mtl_session_attach_buffer(session, buffer);
mtl_session_start(session);
```

After attachment, the same runtime calls apply:

```c
mtl_tx_acquire(session, timeout_ms, &buffer);
fill(buffer);
mtl_tx_submit(session, buffer, &submission);
```

RX uses:

```c
mtl_rx_dequeue(session, timeout_ms, &buffer, &result);
consume(buffer, result);
mtl_rx_release(session, buffer);
```

`rx_release()` returns the attached buffer to the receive pool. It does not transfer
allocation lifetime to MTL. Detach is allowed only after the buffer is idle, normally
after session stop/drain.

Session creation should produce a stopped/configuring session. Pool validation,
registration, and attachment complete before explicit start. This removes the race in
which an RX backend could begin receiving before imported buffers exist.

### 8.3 Optional dynamic operation

Some camera, codec, or framework integrations may need to submit/provide a buffer without
permanently attaching it to a session pool. This can be added as a capability:

```c
mtl_tx_submit(session, app_idle_imported_buffer, &submission);
mtl_rx_provide(session, app_idle_imported_buffer);
```

It still uses the same buffer handle and completion/result types. It is not a new
“user-owned mode.” A successful operation transfers the access lease; a rejection leaves
it with the caller.

### 8.4 Why a fixed-pool first implementation is a good compromise

Benefits:

- maps directly to current MTL frame pools;
- maps to Rivermax's persistent stream ring;
- performs registration and validation outside the hot path;
- gives bounded queue and memory behavior;
- makes direct DMA and progressive access easier to validate;
- supports imported camera/codec pools by attaching all surfaces before start.

Limitations:

- cannot immediately accept an unbounded sequence of unrelated allocations;
- pool changes require stop/drain unless dynamic attach is implemented;
- mixed origins may initially be unsupported by some backends.

Those limitations should be capabilities, not permanent ABI divisions.

## 9. Access ownership and completion

Allocation ownership never changes. Access ownership follows one state machine for both
MTL and imported storage:

```text
TX:

POOL_FREE --acquire--> APP_WRITABLE --submit--> MTL_QUEUED
    ^                                           |
    |                                           v
    +---- reliable terminal result <------ MTL_IN_FLIGHT

RX:

POOL_FREE --> RECEIVING --> READY --dequeue--> APP_READING
    ^                                              |
    +---------------- release ---------------------+
```

Required rules:

1. successful acquire returns one unique lease;
2. successful submit/provide transfers access to MTL;
3. rejection leaves access with the caller and creates no later completion;
4. every accepted TX submission receives exactly one terminal result;
5. a terminal reusable result means no converter, encoder, packet reference, DMA
   descriptor, NIC, or backend can still access that submission's storage;
6. the terminal result must be retained before the buffer becomes acquirable again;
7. wrong-session, stale, duplicate, and double-return operations fail without corrupting
   another lease;
8. memory/buffer destroy or detach returns `-EBUSY` while referenced;
9. direct, copy, and conversion paths use the same ownership result;
10. informational events may coalesce; ownership results may not be dropped.

For v1, one terminal result may wait until both source-memory access and transport outcome
are finished. A later `SOURCE_REUSABLE` event can release conversion input earlier, but
it must be separate from terminal transport completion.

## 10. Data-path policy

Allocation origin must not promise zero-copy. The application states acceptable behavior:

```c
enum mtl_data_path_policy {
  MTL_DATA_PATH_REQUIRE_DIRECT,
  MTL_DATA_PATH_PREFER_DIRECT,
  MTL_DATA_PATH_ALLOW_COPY,
};
```

MTL reports the selected path:

```text
DIRECT
CONVERT_DIRECT
COPY
COPY_AND_CONVERT
UNSUPPORTED
```

`REQUIRE_DIRECT` fails rather than silently copying. `PREFER_DIRECT` permits an explicit
fallback. Capability queries must report:

- supported memory types and devices;
- direct TX/RX support;
- per-plane, stride, size, and address alignment;
- registration granularity and device/port scope;
- fixed/mixed/dynamic pool support;
- progressive publication granularity;
- audio sample/packet granularity;
- fence requirements;
- selected path and reason when direct operation is unavailable.

If a layout satisfies all advertised requirements and capacity remains, the backend must
not reject it later for an undisclosed alignment requirement.

## 11. Timing and A/V synchronization remain independent of storage

A reusable buffer has no permanent timestamp. One submission gives it media meaning.

The API must separate:

| Time | Meaning |
|---|---|
| media time | first media element represented by the submission |
| media reference TAI | common timeline instant represented by that media position |
| scheduled packet time | transport launch plan |
| RTP timestamp | wire essence-clock identity |
| observed packet time | hardware/software TX/RX measurement |

For synchronized operation, a shared rational timeline maps video frames and audio samples
to one TAI origin. Video and audio buffer boundaries do not need to match.

```text
shared media timeline
      /         \
59.94 video    48 kHz audio
90 kHz RTP     48 kHz RTP
      \         /
common TAI reference
```

An audio submission timestamp identifies its first sample. Every packet derives its RTP
and launch position from the exact sample offset. A video timestamp identifies the frame
or field sampling instant.

Exact first-packet launch and explicit RTP timestamp are independent expert overrides.
Invalid exact requests fail explicitly; they never fall back silently to another pacing
mode.

Storage origin must not change any timing rule:

```text
same media submission + equivalent content
    internal storage == imported storage
for media time, RTP, schedule, completion status, and A/V phase
```

## 12. Progressive line operation

Line operation is not another buffer ownership mode. It is progressive readiness inside
one buffer lease:

```text
submit frame F at media time P, ready=0
publish ready=64
publish ready=128
publish ready=height, FINAL
terminal completion
```

All lines in the frame/field share one media and RTP identity. `ready=N` publishes the
immutable contiguous prefix `[0,N)`. Publication establishes release/acquire visibility.
MTL reads only published data.

For planar video, a logical line is ready only when every required plane row is valid.
For interlaced video, each field is one timed unit and progress counts field lines.

The packet schedule is established once from the frame media time. Waiting for a late
line must not move the remainder to another frame epoch. The selected late policy produces
one explicit result.

Internal and imported buffers use the same `mtl_tx_publish()` operation and progress
contract.

## 13. API evolution and common verbs

The public runtime verbs should expose genuine ownership transitions:

```c
/* TX */
mtl_tx_acquire();
mtl_tx_submit();
mtl_tx_abort();
mtl_tx_publish();             /* progressive only */
mtl_tx_dequeue_completion();

/* RX */
mtl_rx_dequeue();
mtl_rx_release();

/* Storage provisioning */
mtl_memory_alloc();
mtl_memory_import();
mtl_memory_destroy();
mtl_buffer_create();
mtl_buffer_destroy();
mtl_session_attach_buffer();
mtl_session_detach_buffer();
```

These remain media-polymorphic. Direction-specific names prevent `get()` and `put()` from
meaning opposite ownership transitions on TX and RX.

Opaque handles plus small typed views preserve type checking without freezing one large
public union. Every extensible structure begins with `struct_size`; newer libraries fill
only the caller's known size, and older libraries ignore unknown trailing bytes while
rejecting unknown required semantics.

## 14. Minimum observable guarantees

The API should be reviewed and tested against these statements:

### Ownership and progress

1. Every accepted ownership transfer terminates exactly once.
2. Every rejected operation leaves ownership with the caller.
3. An accepted item progresses without requiring an unrelated future API call.
4. Reliable completions cannot be dropped by event-queue saturation.
5. A buffer is not reacquirable before its terminal result is retained.
6. Completion means the documented set of readers/writers has ended.
7. Foreign, stale, duplicate, and wrong-session handles cannot corrupt valid state.
8. Ordering comes from a declared FIFO/media-time policy, never slot index.

### Memory

9. A memory region outlives every buffer, conversion, packet, and DMA reference.
10. Destroy/unregister returns `-EBUSY` while referenced.
11. No access occurs outside declared plane spans or published progress.
12. Direct-only never silently copies or converts.
13. Selected data path is queryable and reported.
14. Internal and imported buffers obey the same timing and terminal accounting.

### Time and synchronization

15. Every timestamp field has one clock, unit, epoch, and measurement point.
16. Numeric zero is valid when its validity bit is set.
17. Requested, resolved, scheduled, and observed times are separate.
18. RTP derives from absolute rational media time without cumulative drift.
19. Audio timing identifies the first sample; line timing identifies the containing
    frame/field.
20. Multi-session arm is atomic and no required stream starts alone.
21. Dropping a unit does not shift later media or another synchronized stream.
22. Arrival time is never substituted for media reference time.

### Lifecycle and ABI

23. Destroy cannot race an active public call or callback into freed memory.
24. Stop/drain/abort wake blocked calls with documented outcomes.
25. Drain/abort returns terminal ownership for all accepted work.
26. Creation failure leaves no live session, mapping, callback, or output handle.
27. Structure-size compatibility prevents reads/writes outside the caller's ABI.
28. Every advertised capability names and passes its conformance tests.
29. A session cannot start until its required pool is complete and validated.
30. Pool attachment, detachment, and memory registration never occur implicitly in the
    real-time packet path.

At all times:

```text
accepted submissions
    = terminal results
    + submissions observably still in flight
```

At quiescence:

```text
accepted submissions = terminal results
rejected submissions intersect terminal results = empty
```

## 15. Implementation strategy

### Phase 1: freeze semantics, keep existing datapaths

- define opaque session/buffer/submission handles;
- define direction-specific verbs and reliable completion;
- define structure-size/version rules and error classes;
- wrap current library-owned pipeline pools;
- add state-machine and fault-injection tests.

### Phase 2: one real memory object

- implement retained host-memory import using existing DMA machinery;
- add immutable plane layouts;
- attach imported buffers to fixed pools;
- initially allow copy/conversion where direct operation is unavailable;
- expose selected data path and capability requirements.

### Phase 3: direct external paths

- adapt low-level ST20 external TX/RX;
- map every active port/device and protect mappings by reference count;
- replace callback-selected RX memory with pre-attached/provided handles;
- add direct-only and safe-reuse tests.

### Phase 4: timing and synchronization

- separate media, launch, RTP, and observed timing;
- add absolute rational mapping and extended RTP;
- add a shared timeline with atomic A/V arm;
- implement exact launch as an independent override.

### Phase 5: progressive and additional essences

- adapt current ST20 slice readiness to cumulative publication;
- add explicit memory ordering and missed-line results;
- adapt ST22/ST30/ST40 to the same ownership API;
- add multi-packet audio and long-duration A/V synchronization tests.

### Phase 6: device memory and advanced pools

- add DMA-BUF/device-provider imports and fences;
- add header/payload split where backends support it;
- permit mixed and dynamic pools only after their conformance sets pass;
- add optional early source-release without weakening terminal completion.

## 16. Required changes to the PR proposal

Before stabilization:

1. remove `mtl_buffer_ownership_t` as a session-wide semantic mode;
2. replace raw `buffer_post(data, size, ctx)` with buffer handles and submissions;
3. make library allocation the default pool configuration, not another runtime API;
4. make imported memory a retained region plus immutable buffer layout;
5. support imported fixed pools through attach/detach;
6. separate data-path policy from allocation origin;
7. replace lossy ownership events with reliable completions;
8. separate input metadata from completion output;
9. introduce media span/timeline semantics before adding more timestamp flags;
10. model slice mode as progress on the same lease;
11. wire lifecycle guards into every public entry/callback;
12. publish capability-to-contract-test mappings;
13. implement the public layer over existing pipelines first.

## 17. Suggested PR comment

The unified-session direction is worth continuing: media-specific creation followed by
one opaque session and common lifecycle/statistics/waiting infrastructure can simplify
applications substantially.

I do not think the proposed `LIBRARY_OWNED` versus `USER_OWNED` session mode should become
part of the stable ABI. MTL needs both convenient internal allocation and imported
application memory, but those are provisioning choices, not different buffer semantics.
The current user-owned path may be direct TX, converter input, copied RX, or
callback-selected direct RX, so “user-owned/zero-copy” does not describe observable
behavior.

I suggest one opaque buffer handle with four separate concepts:

```text
memory region -> immutable layout -> access lease -> per-use submission/result
```

MTL may populate a default fixed pool, or applications may import memory, create buffer
layouts, and attach those handles to the same pool. After that, both use the same
TX acquire/submit/completion and RX dequeue/release operations. Direct, copied, and
converted execution should be an independent policy (`REQUIRE_DIRECT`,
`PREFER_DIRECT`, `ALLOW_COPY`) whose selected result is queryable.

This also gives external memory a testable lifetime: successful submit transfers access;
rejection does not; every accepted TX submission produces exactly one reliable terminal
result; completion means no converter, packet reference, DMA descriptor, or NIC can
access the storage; region destruction returns `-EBUSY` while referenced.

The public Rivermax examples support the same architectural conclusion. Rivermax can
allocate/register stream memory, register application allocation, or consume an
application-provided memory key, while all configurations use the same chunk
acquire/commit lifecycle. Its fixed stream ring is also a practical first target for MTL
imported pools. MTL can improve on that model with mandatory ownership completion,
explicit RX lease lifetime, provider-neutral memory handles, and media-aware A/V timing.

Timing must remain independent from storage. A submission—not a reusable buffer—should
carry media time. A shared rational timeline should derive video/audio RTP and standard
pacing; exact first-packet launch and explicit RTP should remain independent overrides.
Line mode should publish a cumulative ready prefix within the same timed frame/field.

I recommend freezing these ownership/time guarantees and their contract tests first,
then implementing the new API as a facade over existing pipelines. That preserves the
good unified-session idea without locking path-dependent memory semantics into the ABI.

## References

### PR and MTL

- [PR #1610](https://github.com/OpenVisualCloud/Media-Transport-Library/pull/1610)
- [PR public API header](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/14a1f80ccc6ec8d2ae021a2baad8e054564dc000/include/mtl_session_api.h)
- [PR TX implementation](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/14a1f80ccc6ec8d2ae021a2baad8e054564dc000/lib/src/new_api/mt_session_video_tx.c)
- [PR RX implementation](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/14a1f80ccc6ec8d2ae021a2baad8e054564dc000/lib/src/new_api/mt_session_video_rx.c)
- [PR buffer implementation](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/14a1f80ccc6ec8d2ae021a2baad8e054564dc000/lib/src/new_api/mt_session_buffer.c)
- [Current ST20 low-level API](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/bf58f6e9da017114ae4af030c9b3cc111a68a8d9/include/st20_api.h)
- [Current pipeline API](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/bf58f6e9da017114ae4af030c9b3cc111a68a8d9/include/st_pipeline_api.h)
- [Current ST20 pipeline TX](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/bf58f6e9da017114ae4af030c9b3cc111a68a8d9/lib/src/st2110/pipeline/st20_pipeline_tx.c)
- [Current MTL DMA mapping](https://github.com/OpenVisualCloud/Media-Transport-Library/blob/bf58f6e9da017114ae4af030c9b3cc111a68a8d9/lib/src/mt_main.c)

### Rivermax

- [NVIDIA Rivermax product page](https://developer.nvidia.com/networking/rivermax)
- [NVIDIA Rivermax FAQ](https://developer.nvidia.com/networking/rivermax/faq)
- [Rivermax Dev Kit](https://github.com/NVIDIA/rivermax-dev-kit)
- [Rivermax TX allocation example](https://github.com/NVIDIA/rivermax-examples/tree/main/api_demo/output_media/memory_allocation_media_send)
- [Rivermax TX registration example](https://github.com/NVIDIA/rivermax-examples/tree/main/api_demo/output_media/memory_registration_media_send)
- [Rivermax RX allocation example](https://github.com/NVIDIA/rivermax-examples/tree/main/api_demo/input/memory_allocation_receive)
- [Rivermax RX registration example](https://github.com/NVIDIA/rivermax-examples/tree/main/api_demo/input/memory_registration_receive)
- [Rivermax media chunk wrapper](https://github.com/NVIDIA/rivermax-dev-kit/blob/main/source/core/chunk/include/rdk/core/chunk/media_chunk.h)
- [Rivermax media-unit buffer abstraction](https://github.com/NVIDIA/rivermax-dev-kit/blob/main/source/services/media/include/rdk/services/media/media_essence_source.h)
- [Rivermax Dev Kit memory allocators](https://github.com/NVIDIA/rivermax-dev-kit/blob/main/source/services/memory_allocation/include/rdk/services/memory_allocation/memory_allocator_interface.h)
- [Rivermax header/payload layout](https://github.com/NVIDIA/rivermax-dev-kit/blob/main/source/core/memory_layout/include/rdk/core/memory_layout/header_payload_memory_layout.h)
