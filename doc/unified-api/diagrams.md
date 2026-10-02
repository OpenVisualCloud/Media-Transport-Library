# Unified MTL API: diagrams

| | |
|---|---|
| Status | Maintained. The picture book of the unified API: one picture per example, then the pictures of each topic, then an index of every diagram in the maintained set. Nothing here is implemented yet |
| Date | 2026-10-02 |
| Sources | [archive/samples/diagrams.md](archive/samples/diagrams.md), [archive/samples/README.md](archive/samples/README.md), [archive/02](archive/02-architecture.md), [archive/03](archive/03-object-model-and-lifecycle.md), [archive/04](archive/04-threading-and-execution.md), [archive/05](archive/05-memory-and-buffers.md), [archive/06](archive/06-timing-pacing-and-sync.md), [archive/07](archive/07-completions-events-and-errors.md), [archive/11](archive/11-abi-compatibility-and-migration.md), [archive/12](archive/12-familiarity-libfabric-and-rivermax.md), [archive/14](archive/14-implementation-roadmap.md), [archive/16](archive/16-kubernetes-and-crash-safety.md), [archive/LEARN.md](archive/LEARN.md), [archive/LIST-OF-CHANGES.md](archive/LIST-OF-CHANGES.md), [archive/REVISION-4.md](archive/REVISION-4.md), [archive/simplification/S6](archive/simplification/S6-minimal-alternative.md), [S7](archive/simplification/S7-samples-friction.md), [S9](archive/simplification/S9-hiding-session-headers.md), [archive/research/](archive/research/) (notes 00, 03, 04, 05, 12, 13) |
| Headers | [sketch/include/mtl/experimental/](sketch/include/mtl/experimental/): the names in every picture are checked against them, and the headers win |

The pictures are drawn from the headers and the compiling examples in
[sketch/examples/](sketch/examples/). Each example picture links to its section in
[examples.md](examples.md) (the examples with their full text) and to its file.

| Notation | Meaning |
|---|---|
| blue box | your application's own code |
| green box | an MTL call, or MTL's work |
| grey circle | the network, or another system (MXL readers, an NMOS controller, the kubelet) |
| arrow labels | verb names only; the arguments are in the example |
| plain boxes | topic pictures (sections 3 to 13), which show the inside of MTL as well |
| (Phase N), (Phase 7, later), (`MTL_LATER`) | not in the first implementation; phases are those of [implementation-plan.md](implementation-plan.md) |

The example pictures follow four rules, so they stay readable: one idea each, at most about eight
boxes, only what an application sees, and the happy path. Error handling is in
[§2.20](#220-what-a-return-code-tells-you); the inside of MTL appears once, in
[§2.19](#219-under-the-hood-nothing-waits-on-the-pinned-cores), and then in the topic sections.

## 1. The samples at a glance

**Every example compiles.** The files in [sketch/examples/](sketch/examples/) are built
warning-free as C99 and C++17, with gcc and clang, against the headers, by
[sketch/check.sh](sketch/check.sh). They are never linked: application hooks such as `render()`
are declared, not defined. Every function in the pictures exists in the headers. Every
configuration is typed (enums and defines, no strings).

Personas ([archive/01 §2](archive/01-goals-and-requirements.md)): P1 simple generator or player,
P2 framework integrator, P3 broadcast playout, P4 live capture or gateway, P5 zero-copy forwarder
or processor, P7 libfabric- or Rivermax-familiar developer, P9 operator or NMOS integrator.

| Example | Persona | What it shows | Headers | Phase | Picture | Text |
|---|---|---|---|---|---|---|
| [ex01_tx_video.c](sketch/examples/ex01_tx_video.c) | P1 | the smallest sender: ports from `MTL_PORTS`, a typed config, `mtl_session_open`, acquire → draw → submit, close | `mtl.h` | 1 | [2.1](#21-send-video-ex01), [2.3](#23-one-frames-life) | [examples §3](examples.md#3-send-video-smallest-program) |
| [ex02_rx_video.c](sketch/examples/ex02_rx_video.c) | P1 | a receiver on two ST 2022-7 legs; lost packets read as zero | `mtl.h` | 1 | [2.2](#22-receive-video-on-two-networks-ex02) | [examples §4](examples.md#4-receive-video-on-two-st-2022-7-legs) |
| [ex03_event_loop.c](sketch/examples/ex03_event_loop.c) | P2, P7 | a sender in the application's own epoll loop, a result per frame | `mtl.h` | 1 | [2.4](#24-in-your-own-event-loop-ex03) | [examples §5](examples.md#5-a-sender-in-the-applications-epoll-loop) |
| [ex04_zero_copy_tx.c](sketch/examples/ex04_zero_copy_tx.c) | P2, P5 | a framework's pool attached in one call; surface i = slot i; close returns 0 when the memory is free | `mtl_mem.h` | 4 | [2.6](#26-zero-copy-from-a-frameworks-pool-ex04) | [examples §6](examples.md#6-zero-copy-from-a-frameworks-pool) |
| [ex05_rx_to_framework.c](sketch/examples/ex05_rx_to_framework.c) | P2 | received frames lent to a framework, released on any thread; GStreamer `unlock` | `mtl.h` | 1 | [2.7](#27-received-frames-handed-to-a-framework-ex05) | [examples §7](examples.md#7-received-frames-lent-to-a-framework) |
| [ex06_mxl_ring.c](sketch/examples/ex06_mxl_ring.c) | P5 | frame k lands in MXL grain k mod 8 | `mtl_mem.h` | 4 | [2.8](#28-into-an-mxl-ring-by-frame-number-ex06) | [examples §8](examples.md#8-receive-into-an-mxl-ring-by-frame-number) |
| [ex07_av_anc_playout.c](sketch/examples/ex07_av_anc_playout.c) | P3 | video, audio and captions from one file, started together on one timeline, exact RTP | `mtl_sync.h`, `mtl_util.h` | 3 | [2.9](#29-video-audio-and-captions-from-one-file-ex07), [2.17](#217-media-time-and-launch-time) | [examples §9](examples.md#9-video-audio-and-captions-from-one-file) |
| [ex08_progressive_rows.c](sketch/examples/ex08_progressive_rows.c) | P4 | rows leave before the frame is finished (SDI-to-IP gateway) | `mtl_sync.h` | 6 | [2.10](#210-rows-leave-before-the-frame-is-finished-ex08) | [examples §10](examples.md#10-rows-that-leave-before-the-frame-is-finished) |
| [ex09_split_forwarder.c](sketch/examples/ex09_split_forwarder.c) | P5 | one 4K frame in, four HD streams out, no copy | `mtl_util.h` (includes `mtl_mem.h`) | 4 | [2.11](#211-one-4k-frame-in-four-hd-streams-out-no-copy-ex09) | [examples §11](examples.md#11-one-4k-frame-in-four-hd-streams-out-no-copy) |
| [ex10_processor.c](sketch/examples/ex10_processor.c) | P5 | a processor that keeps the input's media time and RTP | `mtl_util.h` | 3 | [2.12](#212-a-processor-that-keeps-the-inputs-timing-ex10) | [examples §12](examples.md#12-a-processor-that-keeps-the-inputs-timing) |
| [ex11_signal.c](sketch/examples/ex11_signal.c) | P9, any service | a service in a Kubernetes pod: SIGTERM, probes from health, a bounded network-first shutdown with a report | `mtl_observe.h` | 1 | [2.13](#213-a-service-in-a-kubernetes-pod-ex11) | [examples §13](examples.md#13-a-service-in-a-kubernetes-pod-signals-probes-bounded-shutdown) |
| [ex12_rtp_packets.c](sketch/examples/ex12_rtp_packets.c) | P5, P7 | RTP passthrough: app-built packets, library pacing, both legs; RX packet chunks | `mtl_packet.h` | 2P | [2.14](#214-rtp-passthrough-you-build-the-packets-ex12) | [examples §14](examples.md#14-rtp-passthrough-the-application-builds-the-packets) |
| [ex13_nmos_switch.c](sketch/examples/ex13_nmos_switch.c) | P9 | one IS-05 PATCH as one update: destinations and `rtp_enabled` on both legs at one instant; the planned instant (the 202 response) and the applied one (in `/active`) | `mtl.h` | 2; `MTL_UPDATE_REAPPLY` is Phase 7, later | [2.15](#215-an-is-05-activation-at-one-instant-ex13) | [examples §15](examples.md#15-nmos-is-05-switch-destinations-at-one-instant) |
| [examples_cpp.cpp](sketch/examples/examples_cpp.cpp) | P2 (bindings) | the same API from C++17: `MTL_INIT`, an option, an RAII lease guard, a stride-safe result read | `mtl.h`, `mtl_options.h` | 1 | [2.16](#216-the-same-api-from-c-examples_cppcpp) | [examples §16](examples.md#16-the-same-api-from-c) |
| [ex_common.h](sketch/examples/ex_common.h) | all | `ex_fail()`: the code, reason, field and detail of a failure | `mtl.h` | 1 | [2.20](#220-what-a-return-code-tells-you) | [examples §2](examples.md#2-shared-by-the-examples) |
| (no file) | P1, P2 | an st20p program side by side with the unified calls | — | — | — | [examples §17](examples.md#17-an-st20p-program-side-by-side) |

Return codes are always negative `MTL_E*` constants with the Linux errno values; `0` is success,
and reads that return a count return at least 1.

Checking:

```bash
doc/unified-api/sketch/check.sh      # compiles every header and example
```

The mermaid blocks of this page use only what GitHub renders (flowchart, sequence,
stateDiagram-v2); each was rendered with mermaid 11 in headless Chromium and inspected.

## 2. The examples as pictures

### 2.1 Send video (ex01)

[ex01_tx_video.c](sketch/examples/ex01_tx_video.c), [examples §3](examples.md#3-send-video-smallest-program).

```mermaid
flowchart LR
    A["fill a typed config<br/>MTL_TX, MTL_VIDEO,<br/>MTL_FPS_59_94, MTL_YUV422_10"]:::app --> B["mtl_session_open:<br/>MTL creates and starts it"]:::mtl
    B --> C["acquire a frame"]:::mtl
    C --> D["draw into it"]:::app
    D --> E["submit"]:::mtl
    E --> C
    E --> W(("network")):::net
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

A typed config (`MTL_INIT(&sc)`, then `direction`, `essence`, `flows[0]` with `mtl_flow_ipv4()`,
`video.raster` and `video.format`) describes the stream, and `mtl_session_open` creates and starts
it. `mtl_instance_open(NULL, &mt)` takes the ports from `MTL_PORTS` (`"null:1"` runs without a
NIC). MTL owns the buffers; each submitted frame goes out in the next slot (media mode AUTO, source
kind PLAYBACK, the epoch timeline, results off). Nothing has to be read back, and one
`mtl_session_close` at the end sends what is queued and retires the session.

### 2.2 Receive video on two networks (ex02)

[ex02_rx_video.c](sketch/examples/ex02_rx_video.c), [examples §4](examples.md#4-receive-video-on-two-st-2022-7-legs).

```mermaid
flowchart LR
    N1(("network A")):::net --> M["MTL merges<br/>both legs"]:::mtl
    N2(("network B")):::net --> M
    M --> D["dequeue a frame"]:::mtl
    D --> R["read it<br/>status, media index"]:::app
    R --> L["release"]:::mtl
    L --> D
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

Two flows in the config (`sc.flows[0]` and `sc.flows[1]`) are two ST 2022-7 legs on instance ports
0 and 1; packets merge by sequence number. A frame with lost packets still arrives, with
`u.status == MTL_RX_INCOMPLETE` and the gaps read as zero. `u.media_index` is valid when
`MTL_UNITF_INDEX_VALID` is set. `-MTL_EAGAIN` with no signal: `status.flags` lacks
`MTL_STATUS_RX_SIGNAL`.

### 2.3 One frame's life

[ex01_tx_video.c](sketch/examples/ex01_tx_video.c); the same picture, with the RX side, is in
[concepts.md §5](concepts.md#5-the-life-of-a-unit); the lease-table states behind it are in
[engine.md §3.2](engine.md#32-slot-states).

```mermaid
stateDiagram-v2
    direction LR
    [*] --> Free
    Free --> Yours: acquire
    Yours --> Free: release
    Yours --> Queued: submit
    Queued --> OnTheWire: its slot comes
    Queued --> Free: dropped or flushed, result recorded
    OnTheWire --> Free: sent, result recorded
```

While a frame is yours, MTL never touches it. After submit it is MTL's until it has been sent;
exactly one result says what happened (`MTL_TX_ON_TIME`, `LATE`, `DROPPED`, `FLUSHED` or
`FAILED`), or a counter when results are off. A failed first submit gives the slot back without a
result.

### 2.4 In your own event loop (ex03)

[ex03_event_loop.c](sketch/examples/ex03_event_loop.c), [examples §5](examples.md#5-a-sender-in-the-applications-epoll-loop).

```mermaid
flowchart LR
    D["drain: reap results,<br/>acquire, fill, submit"]:::app -->|"until -MTL_EAGAIN"| E["epoll_wait on<br/>the session's wait handle"]:::app
    E -->|"woken"| D
    classDef app fill:#dbeafe,stroke:#2563eb
```

The session is created with `MTL_SESSION_RESULTS`; `mtl_session_get_wait_handle(s,
MTL_WAIT_ACQUIRE | MTL_WAIT_RESULTS, &fd)` gives an eventfd. Every data call that finds nothing
returns `-MTL_EAGAIN` and arms the handle before it returns (R2), so "drain until nothing, then
sleep" never misses a wake-up. The slot is acquired before the source frame is taken, so no source
frame is taken without a place to go; `u.cookie` comes back in the result.

### 2.5 A session's life

[ex04_zero_copy_tx.c](sketch/examples/ex04_zero_copy_tx.c) (create, attach, start, stop, close). The
full machine with every transition is in [contract.md §4.1](contract.md#41-states); this is the
path an application sees.

```mermaid
stateDiagram-v2
    direction LR
    [*] --> CREATED: create
    CREATED --> RUNNING: start
    RUNNING --> STOPPED: stop
    STOPPED --> RUNNING: start
    RUNNING --> CLOSING: close
    STOPPED --> CLOSING: close
    CLOSING --> RETIRED: last lease returned
    RETIRED --> [*]
```

`mtl_session_open` is create plus start, so it goes straight to `MTL_STATE_RUNNING`. A start at a
later instant passes through ARMED; a stop passes through DRAINING or FLUSHING; a failed session
moves to ERROR, which only stop and close leave. Close always consumes the handle: 0 when retired,
1 while leases are still out ([§2.20](#220-what-a-return-code-tells-you)).

### 2.6 Zero copy from a framework's pool (ex04)

[ex04_zero_copy_tx.c](sketch/examples/ex04_zero_copy_tx.c), [examples §6](examples.md#6-zero-copy-from-a-frameworks-pool). Phase 4.

```mermaid
flowchart LR
    P["framework surface i"]:::app --> S["slot i<br/>(attached once)"]:::mtl
    S --> T["acquire slot i,<br/>submit"]:::mtl
    T --> W(("network")):::net
    W --> R["result for surface i"]:::mtl
    R --> F["give surface i back<br/>to the framework"]:::app
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

The config sets `MTL_SESSION_POOL_ATTACHED | MTL_SESSION_REQUIRE_DIRECT` and `pool_count`; one
`mtl_session_attach` imports the arena (`a.va`, `a.length`, `a.count`), and fails with a reason if
it is too small. `mtl_tx_acquire_slot(s, i, ...)` acquires exactly surface i, or returns
`-MTL_EAGAIN` while it is still in flight. Because it is your memory, every frame produces a result
(M3), and a surface goes back only when its result says the network is done with it. Stop with
`MTL_STOP_DRAIN`, reap, then close: 0 means the arena may be freed.

### 2.7 Received frames handed to a framework (ex05)

[ex05_rx_to_framework.c](sketch/examples/ex05_rx_to_framework.c), [examples §7](examples.md#7-received-frames-lent-to-a-framework).

```mermaid
flowchart LR
    D["dequeue<br/>(streaming thread)"]:::mtl --> G["wrap as a<br/>framework buffer"]:::app
    G --> U["downstream elements<br/>use it"]:::app
    U --> R["release<br/>(any thread, any order)"]:::mtl
    I["unlock: interrupt(s, 1)"]:::app -.->|"-MTL_ECANCELED"| D
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
```

A received frame stays valid until it is released, on whatever thread drops it. GStreamer `unlock`
and `unlock_stop` map to `mtl_session_interrupt(s, 1)` and `(s, 0)`: the blocked dequeue returns
`-MTL_ECANCELED` at once. `-MTL_ESHUTDOWN` means the element stopped or closed the session; close
returns 1 while downstream still holds buffers, and the session retires on the last release.

### 2.8 Into an MXL ring by frame number (ex06)

[ex06_mxl_ring.c](sketch/examples/ex06_mxl_ring.c), [examples §8](examples.md#8-receive-into-an-mxl-ring-by-frame-number). Phase 4.

```mermaid
flowchart LR
    F(("frame k<br/>from the network")):::net --> M["MTL writes it<br/>into grain k mod 8"]:::mtl
    M --> C["commit grain"]:::app
    C --> X(("MXL readers<br/>find frame k")):::net
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

The ring is the session's pool (`MTL_SESSION_POOL_ATTACHED | MTL_SESSION_RX_BY_INDEX |
MTL_SESSION_RX_LATEST`, `a.pitch = grain_size`). With RX_BY_INDEX the slot is the media index mod
`pool_count`, so `u.slot == media_index mod 8`. Frame numbers count from the SMPTE epoch (the
default timeline), so every process agrees on which grain holds frame k.

### 2.9 Video, audio and captions from one file (ex07)

[ex07_av_anc_playout.c](sketch/examples/ex07_av_anc_playout.c), [examples §9](examples.md#9-video-audio-and-captions-from-one-file). Phase 3.

```mermaid
flowchart LR
    F["file<br/>demuxer"]:::app --> V["video: frame k"]:::app
    F --> A["audio: sample n"]:::app
    F --> C["captions: frame k"]:::app
    V --> T["one timeline,<br/>one start of three"]:::mtl
    A --> T
    C --> T
    T --> W(("exact RTP for all three;<br/>captions k = video k")):::net
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

`mtl_timeline_create` (anchor `MTL_ANCHOR_AT_START`) gives the three TX sessions one timeline; each
uses `MTL_MEDIA_INDEX`, so `u.media_index` is the frame (video, ANC) or the first sample (audio).
`mtl_session_start(s, 3, NULL, NULL)` starts all three or none, and T0 is resolved once on the
common grid. MTL turns the indices into timestamps that agree exactly. Audio uses the copy path
`mtl_tx_write`; frames without captions still get the empty ANC packet. The arithmetic is in
[timing.md §10](timing.md#10-avanc-synchronisation).

### 2.10 Rows leave before the frame is finished (ex08)

[ex08_progressive_rows.c](sketch/examples/ex08_progressive_rows.c), [examples §10](examples.md#10-rows-that-leave-before-the-frame-is-finished). Phase 6 (row units).

```mermaid
flowchart LR
    H["next slot hint:<br/>which frame"]:::mtl --> A["acquire"]:::mtl
    A --> R1["draw rows 0-63<br/>submit, used = 64"]:::app
    R1 --> R2["draw rows 64-127<br/>submit, used = 128"]:::app
    R2 --> R3["... last rows<br/>submit, used = rows"]:::app
    R1 -.-> W(("first rows already<br/>on the wire")):::net
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

For an SDI-to-IP gateway (`unit = MTL_UNIT_ROWS`, `source_kind = MTL_SOURCE_GATEWAY`,
`media_mode = MTL_MEDIA_INDEX`): `mtl_tx_next_slot` says which frame is being filled, and each
submit of the same lease with a larger `used` publishes more rows; `used == rows` ends the frame.
The first rows are sent while the rest are still being drawn. A failing later submit ends the unit
where its rows stopped (option `tx.rows_late`) and its one result follows.

### 2.11 One 4K frame in, four HD streams out, no copy (ex09)

[ex09_split_forwarder.c](sketch/examples/ex09_split_forwarder.c), [examples §11](examples.md#11-one-4k-frame-in-four-hd-streams-out-no-copy). Phase 4.

```mermaid
flowchart LR
    R(("4K stream")):::net --> RX["received slot j"]:::mtl
    RX --> Q1["top-left view<br/>of slot j"]:::mtl
    RX --> Q2["top-right view"]:::mtl
    RX --> Q3["bottom-left view"]:::mtl
    RX --> Q4["bottom-right view"]:::mtl
    Q1 --> O(("four HD streams")):::net
    Q2 --> O
    Q3 --> O
    Q4 --> O
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

`mtl_session_get_pool_region(rx, &pool)` lends the receiver's library pool; each HD sender attaches
over it (`a.region`, `a.pitch = info.pool_slot_pitch`, `a.offset` = its quadrant, `a.stride[0]` =
the 4K stride), so TX slot j lies over RX slot j. `mtl_tx_send_slot(tx[q], in.slot, &how, 0)` with
`how.hold = in.lease` holds the received frame until each send has left; the RX slot is free once
it is released and every hold has completed ([§7.3](#73-forwarding-with-holds)). Nothing is copied
and nothing is freed early.

### 2.12 A processor that keeps the input's timing (ex10)

[ex10_processor.c](sketch/examples/ex10_processor.c), [examples §12](examples.md#12-a-processor-that-keeps-the-inputs-timing). Phase 3.

```mermaid
flowchart LR
    I(("input<br/>media time M")):::net --> D["dequeue"]:::mtl
    D --> P["process"]:::app
    P --> S["submit with<br/>media_tai_ns = M"]:::mtl
    S --> O(("output at M + fixed delay,<br/>same RTP")):::net
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

The output sessions use `MTL_MEDIA_TAI`, `MTL_SOURCE_CAPTURE` and `min_tx_delay_ns` = the
processing budget. The output carries the input's media time (only when the input has
`MTL_UNITF_TAI_VALID`), so for a compliant input its RTP timestamps equal the input's and the delay
is constant. Audio and ANC pass through `mtl_tx_write` with the received unit as the template.

### 2.13 A service in a Kubernetes pod (ex11)

[ex11_signal.c](sketch/examples/ex11_signal.c), [examples §13](examples.md#13-a-service-in-a-kubernetes-pod-signals-probes-bounded-shutdown).

```mermaid
flowchart LR
    K(("kubelet")):::net -->|"SIGTERM"| S["handler:<br/>note the time"]:::app
    S --> I["instance interrupt<br/>(signal-safe)"]:::mtl
    I --> W["every wait returns<br/>-MTL_ECANCELED"]:::mtl
    W --> J["workers exit,<br/>main thread joins"]:::app
    J --> X["instance shutdown,<br/>network first, in what<br/>is left of the grace"]:::mtl
    X --> L["summary to the<br/>termination log"]:::app
    K -.->|"probes"| H["get health<br/>(lock-free)"]:::mtl
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

The signals are blocked before open and unblocked once the handler has the handle, so none is
lost; the library installs no handler of its own (R8). `mtl_instance_interrupt(mt, 1)` is sticky,
so a worker cannot miss it between two waits. The budget counts from the signal:
`mtl_instance_shutdown(mt, MTL_SHUTDOWN_ALL_REFERENCES, budget, &r, sizeof(r))` finishes the unit on
the wire, leaves the groups, stops the devices and returns 0 (retired), 1 (quiesced: exit is safe)
or `-MTL_EIO` (a port could not be stopped: exit now). A second signal calls `mtl_instance_abort`,
which cuts the shutdown short. `mtl_instance_get_health` answers the probes: liveness fails only on
what a restart can fix (`MTL_HEALTH_LIVENESS`), readiness also on no link, no time lock and
shutdown (`MTL_HEALTH_READINESS`); after the shutdown began (`-MTL_ESHUTDOWN`) liveness stays 200
while readiness answers 503. The whole order is in
[deployment.md §4.2](deployment.md#42-shutdown) and [§10](#10-shutdown-pods-and-crash-safety).

### 2.14 RTP passthrough: you build the packets (ex12)

[ex12_rtp_packets.c](sketch/examples/ex12_rtp_packets.c), [examples §14](examples.md#14-rtp-passthrough-the-application-builds-the-packets). Phase 2P.

```mermaid
flowchart LR
    A["acquire<br/>a chunk of packet slots"]:::mtl --> B["write RTP packets<br/>+ a length per slot"]:::app
    B --> C["submit;<br/>UNIT_END on the<br/>frame's last chunk"]:::mtl
    C --> D["MTL adds UDP/IP/Ethernet,<br/>paces, sends on both legs"]:::mtl
    D --> W(("network")):::net
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

`sc.unit = MTL_UNIT_PACKETS`, `sc.packet.packets_per_unit`, and `sc.packet.set_fields =
MTL_PKT_SET_TIMESTAMP | MTL_PKT_SET_SSRC_PT`: MTL writes only the network headers and the RTP fields
asked for (none by default). The packet table is `mtl_pkt_tx_table(&u)`, a slot is
`mtl_pkt_slot(&u, n)`, and `MTL_SUBMIT_UNIT_END` marks the frame's last chunk. The same verbs as
frames. Receiving works the same way in reverse: chunks of packets with their leg, sequence
number and gap (`mtl_pkt_rx_table(&u)`), duplicates of the two legs already removed.

### 2.15 An IS-05 activation at one instant (ex13)

[ex13_nmos_switch.c](sketch/examples/ex13_nmos_switch.c), [examples §15](examples.md#15-nmos-is-05-switch-destinations-at-one-instant). The update with its planned and applied instants and per-leg enable is Phase 2; `MTL_UPDATE_REAPPLY` and mute are Phase 7, later.

```mermaid
sequenceDiagram
    participant C as NMOS controller
    participant N as Your Node
    participant M as MTL
    C->>N: PATCH destinations, rtp_enabled, activate at T
    N->>M: get_config, then update FLOWS + LEGS + REAPPLY, when T
    M-->>N: planned_tai_ns
    N->>M: get_status: update_seq
    N-->>C: 202, activation at the planned instant
    Note over M: at the slot boundary both legs switch, unit or not
    N->>M: get_status, or MTL_EVENT_UPDATE
    M-->>N: APPLIED for this update_seq, update_applied_tai_ns
    N->>N: /active shows activation_time
```

One PATCH is one `mtl_session_update`, all or nothing: if a leg cannot be prepared the stream keeps
its old configuration. The Node copies only what IS-05 changed (`ip`, `source_filter`,
`udp_port`) into the active configuration from `mtl_session_get_config`; SSRC, DSCP and payload
type stay. The switch is by the clock, so an idle sender activates too. With `master_enable` false
the Node sets every leg in `legs_disabled`: the session is muted (`MTL_STATUS_MUTED`) but stays
RUNNING (Phase 7). A status whose `update_seq` moved on, or whose `update_state` is
`MTL_UPDATE_STATE_REPLACED`, `CANCELLED` or `FAILED`, means this activation did not apply. The
update states are in [§11.2](#112-update-states); the contract in
[nmos-ipmx.md §4](nmos-ipmx.md#4-is-05-the-activation-contract).

### 2.16 The same API from C++ (examples_cpp.cpp)

[examples_cpp.cpp](sketch/examples/examples_cpp.cpp), [examples §16](examples.md#16-the-same-api-from-c).

```mermaid
flowchart TB
    G["TxLease guard created:<br/>nothing held"] --> A{"acquire"}
    A -->|"0"| H["held"]
    A -->|"an error"| N["nothing held"]
    H --> S{"submit"}
    S -->|"-MTL_EAGAIN"| H
    S -->|"0, or another error"| N
    H -->|"guard destroyed"| R["destructor calls<br/>mtl_tx_release"]
    N -->|"guard destroyed"| X["destructor does nothing"]
```

The guard mirrors the lease rule of `mtl_tx_submit`: on failure of a first submit the slot goes
back to the pool, except `-MTL_EAGAIN` (contention from a busy-loop thread), where the lease stays
the application's. Options are an array in the config (`MTL_OPT_LATE_POLICY` =
`MTL_LATE_SEND_LATE`); absent means the default. Results are read with `sizeof(r[0])` as the
record size, so a larger record type stays safe.

### 2.17 Media time and launch time

[ex07_av_anc_playout.c](sketch/examples/ex07_av_anc_playout.c), [ex10_processor.c](sketch/examples/ex10_processor.c); the source kinds are in [concepts.md §7.1](concepts.md#71-two-times-not-one), the formulas in [timing.md §4](timing.md#4-media-time-on-tx) and [§5](timing.md#5-launch-time).

```mermaid
flowchart LR
    K["frame k"]:::app --> M["media time<br/>M = T0 + k x period"]:::mtl
    M --> R["RTP = floor(M x 90 kHz)"]:::mtl
    M --> L["launch: the slot of M<br/>+ ST 2110-21 offset"]:::mtl
    R --> W(("packet")):::net
    L --> W
    classDef app fill:#dbeafe,stroke:#2563eb
    classDef mtl fill:#dcfce7,stroke:#16a34a
    classDef net fill:#f3f4f6,stroke:#6b7280
```

What a frame *is* (its media time, and so its RTP timestamp) is separate from when it *leaves*.
For a PLAYBACK source the first packet leaves at TVD − VRX0·TRS, TVD = N × TFRAME + TROFFSET.
A late frame is dropped or sent late (`tx.late_policy`), but never moves the frames after it.

### 2.18 Which header do I need?

The full table with function counts is [concepts.md §10](concepts.md#10-where-each-header-fits).

| You want to | Include | Phase |
|---|---|---|
| send or receive with MTL's buffers | `mtl.h` only | 1 |
| use your own memory, or lend one session's pool to another | `+ mtl_mem.h` | 4 |
| keep several essences in sync, or compute frame numbers | `+ mtl_sync.h` | 3 |
| build or parse RTP packets yourself | `+ mtl_packet.h` | 2P |
| read events, or wait on many sessions at once | `+ mtl_queue.h` | 1 (events), 2 (queues) |
| export stats, route logs, capture packets, answer probes, shut down with a report | `+ mtl_observe.h` | 1 |
| set a tuning knob | `+ mtl_options.h` | 1 |
| send from a byte buffer, or send a named slot in one call | `+ mtl_util.h` | 2 (copy path), 4 (named slots) |
| render or parse SDP | `+ mtl_sdp.h` | 7, later |
| send or read IPMX sender reports | `+ mtl_rtcp.h` | 7, later |
| encrypt with IPMX PEP | `+ mtl_crypto.h` | 7, later |

### 2.19 Under the hood: nothing waits on the pinned cores

The protocol is in [engine.md §5.2](engine.md#52-the-completing-context-protocol) and
[§7](engine.md#7-waking); the topic picture is [§6.2](#62-who-makes-the-wake-up-syscall).

```mermaid
sequenceDiagram
    participant T as MTL tasklet (pinned)
    participant K as MTL waker thread
    participant A as Your thread
    A->>A: acquire finds nothing: -MTL_EAGAIN, target armed
    A->>A: sleep on the wait handle
    T->>T: frame sent: result stored, DONE, fence
    T->>K: armed, so set the wake bit
    K->>A: eventfd write
    A->>A: drain: reap the result, acquire
```

The pinned cores only send and receive packets. They never run application code, never take a
lock an application holds, and leave the one system call of a wake-up to a library thread (W3, the
default in lcore mode; the direct write W2 is maintainer decision M6).

### 2.20 What a return code tells you

[ex_common.h](sketch/examples/ex_common.h) prints any of them with `mtl_error_name()`,
`mtl_reason_name()` and the field from `mtl_last_error()`. The full table is
[concepts.md §6.4](concepts.md#64-errors) and [contract.md §8.1](contract.md#81-codes).

| You get | It means | Do |
|---|---|---|
| `-MTL_EAGAIN` | nothing yet, now or by the timeout (the wait handle is armed) | try again, or sleep on the wait handle |
| `-MTL_ETIMEDOUT` | a DRAIN stop missed its deadline (never a close) | the rest was flushed |
| `-MTL_ECANCELED` | someone interrupted the waits | leave, or wait again after `interrupt(..., 0)` |
| `-MTL_ESHUTDOWN` | you stopped or closed the session or instance | leave the loop |
| `-MTL_EIO` | the session failed (ERROR), or a device could not be stopped | read `status.error_reason`; stop, then start again, or close |
| `-MTL_ENODEV` | the device was removed, or its reset failed | stop, update the session onto another port, start |
| `-MTL_EINVAL` | a wrong argument | `mtl_last_error()` names the field or option |
| `-MTL_EBUSY` | in use, or not allowed in this state | check the state first |
| `-MTL_EBADF`, `-MTL_ESTALE` | a wrong or closed handle; a lease already returned | fix the bug |
| close returns 1 | still retiring: leases are out, or the device holds units | `MTL_WAIT_RETIRED`, or `MTL_EVENT_SESSION_RETIRED` |

## 3. Architecture

### 3.1 The shape of the API

From [archive/REVISION-4.md §2](archive/REVISION-4.md), with the header names of today.

```mermaid
flowchart LR
    APP["Your application"] --> CORE["mtl.h<br/>instance, session, unit,<br/>start / stop, wait, errors"]
    APP -. "only if needed" .-> EXT["optional headers<br/>mtl_mem, mtl_sync, mtl_queue,<br/>mtl_packet, mtl_observe,<br/>mtl_options, mtl_util, ..."]
    APP -. "Phase 7, later" .-> L7["mtl_sdp, mtl_rtcp,<br/>mtl_crypto"]
    EXT --> CORE
    L7 --> CORE
    CORE --> LIB["libmtl_unified.so.0.rev"]
    LIB --> ENG["libmtl: today's engines"]
```

`mtl.h` has 32 functions; the 17 headers have 125, plus 14 declared under `MTL_LATER`. Every
optional header includes `mtl.h` and adds one job; no unified header includes a legacy one.

### 3.2 Layers

The layer picture (L3 API, L2 session core, L1 adapters, the slot interface, L0 pipelines and
sessions, tasklets, datapath, backends, and the legacy APIs beside them) is
[engine.md §1](engine.md#1-the-layer-picture). The one-box version for a talk is
[presentation/slides.md, "The idea in one picture"](presentation/slides.md#the-idea-in-one-picture).

### 3.3 Threads and the pinned cores

From [archive/02 §1](archive/02-architecture.md). The rules per context are in
[engine.md §2.2](engine.md#22-execution-contexts).

```mermaid
flowchart LR
    APP["Application threads<br/>any core, may block:<br/>control calls, data calls, waits"]
    TK["Pinned scheduler cores, tasklets only:<br/>TX builders and transmitters,<br/>RX tasklets, commands every visit"]
    WK["Library workers:<br/>recovery, auto-detect, ARP, flow<br/>and IGMP for updates, stalled-queue<br/>reset, deferred retire"]
    WA["Waker thread:<br/>the wake syscall"]
    AD["Admin thread: link monitor,<br/>publishes the time base, stats"]
    DS["Dispatch and log-sink threads:<br/>the only ones that run app code"]
    NIC(("NIC: DPDK PMD,<br/>AF_XDP, kernel<br/>socket, or null"))
    APP -->|"commands; slot interface"| TK
    TK -->|"lease table: DONE or<br/>READY + fence"| APP
    TK -->|"wake bit when armed"| WA
    WA -->|"eventfd"| APP
    APP -->|"blocking control work"| WK
    TK -.- AD
    DS -->|"your queue and<br/>log callbacks"| APP
    TK <--> NIC
```

Arrows into the tasklet world are lock-free hand-offs (commands, the slot interface); arrows out of
it are wait-free stores (DONE or READY plus a fence, a wake bit). No tasklet allocates, logs, takes
a lock an application thread can hold, or runs application code (R6).

### 3.4 Objects and ownership

From [archive/03 §1](archive/03-object-model-and-lifecycle.md) and
[archive/02 §3](archive/02-architecture.md), with today's objects: groups became start arrays,
CQ and EQ became the session's results and events plus optional queues, and the buffer handle
became the slot index.

```mermaid
flowchart TB
    I["mtl_instance_h<br/>ports, schedulers, time source;<br/>one reference per open if SHARED"]
    I --> P["ports 0..n<br/>link, pacing class, time state"]
    I --> S["mtl_session_h<br/>one essence, one direction,<br/>unique name"]
    I --> R["mtl_region_h<br/>memory MTL may DMA,<br/>refcounted"]
    I --> T["mtl_timeline_h<br/>exact T0 and grid;<br/>null = the SMPTE epoch"]
    I --> Q["mtl_queue_h<br/>results, readiness and events<br/>of bound sessions"]
    S --> F["flows 0..1 = legs<br/>admin, oper, flow state"]
    S --> PL["pool: slots by index<br/>library or attached"]
    S --> LT["lease table, results ring,<br/>events, status, stats"]
    PL --> L["mtl_lease_h<br/>access to one slot, now"]
    PL -.->|"attached over"| R
    S -.->|"runs on"| T
    S -.->|"bound to"| Q
```

Handles are typed 64-bit values; 0 is null, a closed handle is never reissued, and handle slots are
process-wide and never freed (R4). A buffer is named by its slot index, access to it by a lease.

### 3.5 Coexistence with the legacy API

Legacy and unified code on one instance, the slot interface between the cores, and the legacy
bridge: [migration.md §6](migration.md#6-coexistence).

### 3.6 The legacy headers and where they go

From [archive/simplification/S9 §2.2](archive/simplification/S9-hiding-session-headers.md),
checked against `include/` at `545a266a`. An arrow means "includes": every installed legacy header
reaches `st20_api.h` or `mtl_api.h`, which is why the set moves as one tier
([migration.md §8](migration.md#8-hiding-the-legacy-headers)).

```mermaid
flowchart TB
    API["mtl_api.h"] --> CFG["mtl_build_config.h<br/>(generated)"]
    SCH["mtl_sch_api.h"] --> API
    SHM["mtl_lcore_shm_api.h"] --> API
    ST["st_api.h"] --> API
    S20["st20_api.h<br/>(ST 20 and ST 22)"] --> ST
    S30["st30_api.h"] --> ST
    S40["st40_api.h"] --> ST
    S41["st41_api.h"] --> ST
    PIPE["st_pipeline_api.h"] --> S20
    P30["st30_pipeline_api.h"] --> S30
    P30 --> PIPE
    P40["st40_pipeline_api.h"] --> S40
    P40 --> PIPE
    COMB["experimental/<br/>st20_combined_api.h"] --> S20
    CI["st_convert_internal.h"] --> S20
    CI --> S30
    CA["st_convert_api.h"] --> CI
```

The install layout after Phase 1 (from S9 §6.2, with the tier names of
[migration.md §8.2](migration.md#82-three-tiers)):

```text
include/mtl/experimental/mtl.h + 16 optional headers     public    MTL_UNIFIED_EXPERIMENTAL, then MTL_1.0 (release F)
include/mtl/legacy/mtl_api.h, st_api.h, st20_api.h, ...  legacy    MTL_LEGACY_CORE, MTL_LEGACY_PIPELINE, MTL_LEGACY_SESSION
include/mtl/legacy/mtl_legacy_gate.h                     legacy    included first by every legacy header; reads MTL_LEGACY_STAGE
include/mtl/st20_api.h, ...                              stubs     #include "legacy/st20_api.h"; removed at F+1
lib/include/mtl_internal/                                internal  slot interface; session declarations from F+2; never installed
```

```mermaid
flowchart LR
    S0["Phase 0<br/>version nodes, soname,<br/>gate added, inert"] --> S1["Phase 1<br/>unified headers installed;<br/>legacy in mtl/legacy/ + stubs"]
    S1 --> S2["Phases 2-6<br/>pre-hide gaps closed;<br/>in-tree consumers port"]
    S2 --> F["release F<br/>deprecation warnings;<br/>unified API is MTL_1.0"]
    F --> F1["F+1<br/>#error unless MTL_LEGACY_API;<br/>stubs removed"]
    F1 --> F2["F+2 or later<br/>session headers internal;<br/>libmtl soname bump"]
```

### 3.7 The shape shared with libfabric and Rivermax

From [archive/12 §1](archive/12-familiarity-libfabric-and-rivermax.md); the call map is
[migration.md §13](migration.md#13-if-you-know-libfabric-or-rivermax).

```mermaid
flowchart LR
    A["register memory once<br/>fi_mr_reg, rmx_register_memory,<br/>mtl_mem_import + attach"] --> B["get a slot<br/>get_next_chunk,<br/>mtl_tx_acquire"]
    B --> C["fill"]
    C --> D["post or commit<br/>fi_send, commit_chunk,<br/>mtl_tx_submit"]
    D --> E["completion<br/>fi_cq_read, poll_for_completion,<br/>mtl_tx_reap"]
```

What only MTL adds is time: the submit carries a media time, and MTL derives RTP and the launch.

## 4. Lifecycle and states

### 4.1 The session state machine

The full machine of `enum mtl_state` (CREATED, ARMED, RUNNING, DRAINING, FLUSHING, STOPPED, ERROR,
CLOSING, RETIRED), with what each state allows, is
[contract.md §4.1](contract.md#41-states) and [§4.2](contract.md#42-what-each-state-allows). The
learner's version is [concepts.md §5.3](concepts.md#53-a-sessions-life); the application's path is
[§2.5](#25-a-sessions-life). It replaces revision 3's machine with DESTROYING
([archive/03 §3.1](archive/03-object-model-and-lifecycle.md)).

### 4.2 What close does

From [archive/03 §6.2](archive/03-object-model-and-lifecycle.md) and
[engine.md §9](engine.md#9-close-and-error-on-a-stalled-queue). `mtl_session_close(s, timeout)`
always consumes the handle; `mtl_session_get_state` on it answers CLOSING, then RETIRED.

```mermaid
stateDiagram-v2
    [*] --> Drain: close in ARMED, RUNNING or DRAINING
    [*] --> Detach: close in CREATED, STOPPED, FLUSHING or ERROR
    Drain --> Flush: deadline passed, rest FLUSHED (STOP_TIMEOUT)
    Drain --> Detach: every queued unit sent
    Flush --> Detach: queued units FLUSHED
    Detach --> DeviceRefs: tasklet ack, data callers left
    DeviceRefs --> Leases: last NIC and DMA reference gone
    Leases --> RETIRED: no lease or hold left
    RETIRED --> [*]
    note right of Leases
        close returns 1 here if leases are out;
        the last release posts the retire to a worker
    end note
```

Every step before RETIRED is the state CLOSING. Close returns 0 once retired, 1 while still
retiring (`MTL_WAIT_RETIRED`, `MTL_EVENT_SESSION_RETIRED` report the end). Unread results are
discarded and the session is unbound from every queue. Sessions attached over this session's pool
must close first. A queue that will not release its descriptors takes the stalled-queue path
([§10.2](#102-the-stalled-queue-path)).

### 4.3 Starting sessions together

From [archive/03 §5](archive/03-object-model-and-lifecycle.md) (groups, revision 3) and
[archive/simplification/S7 §8.4](archive/simplification/S7-samples-friction.md); groups became an
array start. The formulas are [timing.md §3.3](timing.md#33-the-start-formula) and
[§7](timing.md#7-start).

```mermaid
flowchart LR
    A["mtl_session_start(s, n, when, &t0)<br/>one timeline, one direction"] --> V["validate every session:<br/>any failure changes nothing"]
    V --> T0["resolve T0 once,<br/>on the timeline's grid"]
    T0 --> ARM["ARMED: TX takes preroll,<br/>RX is joined and filters<br/>media time before the start"]
    ARM --> RUN["RUNNING at the instant:<br/>index k at T0 + k x<br/>that session's period"]
```

Mixed timelines or directions are `-MTL_EINVAL` (`START_SET_MIXED`). On a timeline already
resolved, a start joins at the first feasible index, or at `when->value` with `MTL_AT_INDEX`.
Stop arrays may mix directions.

### 4.4 Legs and flow states

Per leg (`status.leg[]`, `MTL_EVENT_LEG_STATE`, `MTL_EVENT_FLOW_STATE`); the rules are
[contract.md §14](contract.md#14-legs-and-st-2022-7).

```mermaid
stateDiagram-v2
    state "TX leg" as TXL {
        [*] --> WAITING_NEIGHBOUR: start, or update of FLOWS
        WAITING_NEIGHBOUR --> RESOLVED: neighbour resolved
    }
    state "RX leg" as RXL {
        [*] --> JOINING: IGMP join sent
        JOINING --> JOINED
        JOINING --> JOIN_FAILED
    }
```

`admin` is the leg's bit in `legs_disabled` (`MTL_UPDATE_LEGS`); `oper` is 1 when the link is up
and the flow is RESOLVED or JOINED. A TX unit on a leg still WAITING_NEIGHBOUR is
`MTL_TX_DROPPED` with reason `WAITING_NEIGHBOUR` there; an update does not wait for neighbours.
Every existing leg disabled means muted (Phase 7, later).

## 5. Data path and leases

### 5.1 One TX unit through the layers

From [archive/02 §4.1](archive/02-architecture.md), with today's calls. The slot interface and the
lease table are [engine.md §3](engine.md#3-the-slot-interface) and
[§5](engine.md#5-lease-table-and-result-materialisation).

```mermaid
sequenceDiagram
    autonumber
    participant A as App thread
    participant L2 as L2 session core
    participant E as Engine tasklets
    A->>L2: mtl_tx_acquire
    L2->>L2: slot FREE to APP_WRITABLE (CAS, MP-safe), results off: a DONE slot is taken directly
    L2-->>A: unit: lease, slot, planes, meta
    A->>A: fill the planes, set used, media time, cookie, hold
    A->>L2: mtl_tx_submit
    L2->>L2: check lease, layout, media index, horizon, on failure the slot goes back
    L2->>L2: media time M and RTP (exact), seq assigned, QUEUED, conversion here if DPC
    L2->>E: submit through the slot interface
    E->>E: pick-up at the slot: admission decision, late policy
    E->>E: build, pace, burst, last packet handed or last mbuf freed
    E->>L2: CAS claim, result into the lease table, DONE (release), fence
    E-->>A: if armed: wake bit, the waker signals the wait handle
    A->>L2: mtl_tx_reap
    L2-->>A: results in submission order
```

### 5.2 One RX unit through the layers

From [archive/02 §4.2](archive/02-architecture.md).

```mermaid
sequenceDiagram
    autonumber
    participant E as Engine RX tasklet
    participant L2 as L2 session core
    participant A as App thread
    participant D as Any thread
    E->>E: packets for media time M land in a slot (RECEIVING, slot by index with RX_BY_INDEX)
    E->>E: unit complete, or its due time passed: force-complete
    E->>L2: READY with RTP, arrival per leg, packet counts, fence, wake bit if armed
    A->>L2: mtl_rx_dequeue
    L2->>L2: media index and flags, zero-fill or conversion in the caller if granted
    L2-->>A: unit: lease, slot, planes, status (READY to APP_READING)
    A->>D: hand it on, TX units may hold it
    D->>L2: mtl_rx_release, any thread, any order
    L2->>L2: FREE once released and no hold is left, else HELD
```

The due time is the first arrival (earliest leg) + the unit period + `rx.flush_offset_ns`
([timing.md §11.7](timing.md#117-due-time-completion-and-2022-7-skew)).

### 5.3 Slot states in the lease table

TX: FREE, APP_WRITABLE, QUEUED, IN_FLIGHT, DONE; RX: FREE, RECEIVING, READY, APP_READING, HELD.
Both machines, with the CAS that moves each transition and the mapping onto today's st20p states,
are [engine.md §3.2](engine.md#32-slot-states). They replace revision 3's text machines in
[archive/03 §4.1, §4.2](archive/03-object-model-and-lifecycle.md).

### 5.4 Free, yours, MTL's: both directions

From [archive/simplification/S7 §8.4](archive/simplification/S7-samples-friction.md).

```mermaid
stateDiagram-v2
    state "Free" as F
    state "Yours: the app writes or reads it" as Y
    state "MTL's: sending or receiving" as M
    [*] --> F
    F --> Y: TX acquire
    Y --> M: TX submit
    M --> F: TX sent, result recorded
    F --> M: RX packets arrive
    M --> Y: RX dequeue
    Y --> F: TX release, RX release
```

### 5.5 One TX frame, step by step

From [archive/LEARN.md §4](archive/LEARN.md) and
[archive/simplification/S7 §8.4](archive/simplification/S7-samples-friction.md). The RX twin is
[concepts.md §5.2](concepts.md#52-rx).

```mermaid
sequenceDiagram
    autonumber
    participant App
    participant M as MTL
    participant N as Network
    App->>M: mtl_session_open
    loop every frame
        App->>M: mtl_tx_acquire
        M-->>App: a free frame: lease, planes
        App->>App: draw into it
        App->>M: mtl_tx_submit: media index k, cookie
        M->>N: packets, paced on the ST 2110-21 schedule
        App->>M: mtl_tx_reap, if results are on
        M-->>App: ON_TIME, margin_ns
    end
    App->>M: mtl_session_close
```

### 5.6 The lease table and the completing context

The two cache-line halves of a lease-table slot and the six steps of a completion (CAS claim,
result, DONE release store, fence, armed load, wake bit) are
[engine.md §5.1](engine.md#51-layout) and
[§5.2](engine.md#52-the-completing-context-protocol); every context that completes a unit today is
listed in [§5.3](engine.md#53-every-completing-context).

## 6. Waking

### 6.1 Arming, in the application's terms

A data call that finds nothing returns `-MTL_EAGAIN` and arms its target, so "drain until
`-MTL_EAGAIN`, then sleep on the wait handle" never misses a wake-up: [§2.4](#24-in-your-own-event-loop-ex03).
The armed-waiter protocol with its two fences is
[engine.md §7.1](engine.md#71-wait-targets-and-the-armed-waiter-protocol).

### 6.2 Who makes the wake-up syscall

From [archive/04 §4.1, §5](archive/04-threading-and-execution.md) and
[engine.md §7.2](engine.md#72-the-waker).

```mermaid
flowchart TB
    C["completing context:<br/>result stored, DONE, fence"] --> Q{"target armed?"}
    Q -->|"no"| N["done: no syscall"]
    Q -->|"yes, on a tasklet, W3"| B["set the session's bit in<br/>its scheduler's wake word,<br/>or the queue's ready summary"]
    Q -->|"yes, on a tasklet, W2"| W2["one non-blocking<br/>eventfd write()"]
    Q -->|"yes, any other context"| D["wake the waiter directly"]
    B --> W3["waker thread: sleeps until<br/>the earliest next_due_tsc,<br/>scans wake words, writes"]
    W3 --> A["application thread wakes,<br/>drains until -MTL_EAGAIN"]
    W2 --> A
    D --> A
```

W3 (the waker thread) is the default in lcore mode; W2 is the default in thread mode, and below
1 ms in lcore mode only if maintainer decision M6 accepts a syscall on a pinned core. An
application that never sleeps (W0) causes no wake-up at all.

### 6.3 Interrupts

From [engine.md §7.3](engine.md#73-wait-handles-and-interrupts).

```mermaid
flowchart LR
    S["mtl_session_interrupt(s, 1)"] --> F["sticky flag,<br/>one waker signal"]
    QI["mtl_queue_interrupt(q, 1)"] --> F
    I["mtl_instance_interrupt(mt, 1)<br/>(signal handlers)"] --> F
    F --> W["every data wait returns<br/>-MTL_ECANCELED at once,<br/>until interrupt(..., 0)"]
    C["stop or close"] --> X["-MTL_ESHUTDOWN:<br/>close wins over interrupt"]
```

`interrupt(..., 1)` is async-signal-safe; `interrupt(..., 0)` is CP. Stop and close still work
while interrupted.

### 6.4 Commands and acknowledgements

Immediate commands (stop, discard, detach, abort) and boundary commands (`mtl_session_update` with
FLOWS or LEGS, `MTL_DISCARD_REBASE`), and the flow-update picture with its APP, worker and tasklet
columns, are [engine.md §6](engine.md#6-commands-and-acknowledgements).

## 7. Memory

### 7.1 Four separate ideas

From [archive/05 §1](archive/05-memory-and-buffers.md) and
[archive/research/00 §7](archive/research/00-pr1610-design-review.md), with today's names.

```mermaid
flowchart LR
    R["region: where bytes live<br/>VA, length, backing, NUMA,<br/>IOVA per device, refcount"] --> S["slot: one buffer of a pool,<br/>named by its index;<br/>planes and meta, layout<br/>fixed when attached"]
    S --> L["lease: who may touch it now<br/>TX FREE, APP_WRITABLE, QUEUED,<br/>IN_FLIGHT, DONE;<br/>RX FREE, RECEIVING, READY,<br/>APP_READING, HELD"]
    L --> U["unit and result: this use<br/>media time, cookie, hold,<br/>launch; one terminal outcome"]
```

Who allocated the bytes, who may touch them now, and what this use means are separate questions,
so library and application memory run one data path. The rules are
[contract.md §9](contract.md#9-memory).

### 7.2 Where a session's memory comes from

From [archive/research/00](archive/research/00-pr1610-design-review.md) (executive
recommendation), with today's calls.

```mermaid
flowchart LR
    LP["library pool<br/>default, mtl.h"] --> SL["the session's slots"]
    subgraph REG["a region: mtl_region_h"]
        direction TB
        MA["mtl_mem_alloc<br/>library hugepages"]
        MI["mtl_mem_import<br/>application memory"]
        PR["mtl_session_get_pool_region<br/>another session's pool"]
        MD["mtl_mem_import_device<br/>GPU memory, MTL_LATER"]
    end
    REG -->|"mtl_session_attach"| SL
    AV["mtl_session_attach with va:<br/>imported for this session"] --> SL
    SL --> V["one data path:<br/>acquire, submit, dequeue, release"]
```

Application memory always produces results (rule MEM3); a pool over another session's library pool
submits every unit with a hold (`HOLD_REQUIRED`). Per-acquire layouts (`mtl_tx_acquire_layout`)
and per-unit RX destinations (`mtl_rx_provide`) are `MTL_LATER`.

### 7.3 Forwarding with holds

From [archive/05 §5](archive/05-memory-and-buffers.md) and [ex09](sketch/examples/ex09_split_forwarder.c).

```mermaid
sequenceDiagram
    participant RX as RX session
    participant A as App
    participant TX as Four TX sessions
    RX-->>A: dequeue: slot j, lease
    A->>TX: mtl_tx_send_slot(j), hold = lease, four times
    Note over RX: hold count of slot j is 4
    A->>RX: mtl_rx_release(lease)
    Note over RX: slot j is HELD, not FREE
    TX-->>RX: each TX unit's terminal outcome drops one hold
    Note over RX: hold count 0: slot j is FREE
```

A hold is +1 at the TX submit and −1 at that unit's terminal outcome, whatever it is; the owner
cannot change or detach its pool while attached sessions exist, and its close returns 1 until they
close.

### 7.4 Mapping onto today's internals

From [archive/05 §11](archive/05-memory-and-buffers.md); the slot-interface entry points are in
[engine.md §3](engine.md#3-the-slot-interface).

```mermaid
flowchart LR
    R["region<br/>import: extmem register;<br/>DMA map per device,<br/>eager or at first attach"] -->|"refcount + 1 per attach<br/>and per RX hold"| S["attached slot<br/>validated once against the<br/>requirements; addr, IOVA;<br/>stride becomes linesize (M9)"]
    S -->|"TX"| T["submit through the slot interface<br/>(today: put_ext_frame, then<br/>st20_tx_set_ext_frame)"]
    S -->|"RX"| X["st20p_rx_set_ext_frames<br/>at attach (MF9)"]
    T --> D["refcount - 1 at the<br/>once-only done hook"]
    X --> D2["refcount - 1 at the last of<br/>release and holds, or DMA drain"]
```

The teardown order (sessions before regions before the memory) is
[contract.md §9.10](contract.md#910-teardown-order).

## 8. Timing

### 8.1 Two times, not one

What a unit is (media time, RTP) and when it leaves (the slot, by source kind) are separate:
[concepts.md §7.1](concepts.md#71-two-times-not-one) (from [archive/LEARN.md §6](archive/LEARN.md))
and [§2.17](#217-media-time-and-launch-time).

### 8.2 Media modes

From [archive/06 §4](archive/06-timing-pacing-and-sync.md) and `enum mtl_media_mode`; the rules
are [timing.md §4](timing.md#4-media-time-on-tx) and [§5.2](timing.md#52-source-kinds-min_tx_delay_ns-and-the-slot-rule).

```mermaid
flowchart LR
    AUTO["MTL_MEDIA_AUTO<br/>the next slot"] --> M["media time M<br/>on the session's timeline"]
    IDX["MTL_MEDIA_INDEX<br/>T0 + media_index x period"] --> M
    TAI["MTL_MEDIA_TAI<br/>media_tai_ns,<br/>snapped to the grid"] --> M
    M --> RTP["RTP = floor(M x rate)"]
    M --> LA["launch by source kind:<br/>PLAYBACK the slot of M;<br/>CAPTURE the first slot at or after<br/>M + min_tx_delay_ns;<br/>GATEWAY the same frame, by rows"]
    SND["MTL_MEDIA_SENDER, Phase 7:<br/>the source's own instant,<br/>never snapped"] --> RS["RTP follows the source;<br/>launch = M + min_tx_delay_ns"]
```

The slot rule: the slot is the first N whose first-packet time TVD(N) − VRX0·TRS is at or after
M + `min_tx_delay_ns`. `MTL_SUBMIT_NOT_BEFORE` or `MTL_SUBMIT_EXACT` with `launch_tai_ns` override
the launch of one unit.

### 8.3 One video frame on the ST 2110-21 schedule

From [archive/research/12 §3.3](archive/research/12-st2110-timing-standards.md) (progressive,
gapped, sender type N, a PLAYBACK unit on the grid). The formulas are
[timing.md §5.1](timing.md#51-the-st-2110-21-model).

```text
 TAI ──┬─────────────────────┬─────────────────────────────────────────────┬────────────┬──▶
     E(N) = N·TFRAME       TVD = E(N) + TROFFSET = TPR0                TPR(NP−1)     E(N+1)
       │ RTP of frame N      │ ← read schedule: one packet every TRS →     │
       │ = floor(E(N)·90k)   │                                             │ vertical gap ≈
       │             ┌───────┘ the sender may run ahead by VRX_FULL packets │ TFRAME·(1−RACTIVE) − TROFFSET
       │             │ first packet on the wire = TVD − VRX0·TRS
       │◀── offset ─▶│
 JT-NM / EBU LIST: latency = first packet − RTP time in [0, 1 ms];
                   RTP offset = RTP − rtp(E(N)) in [−1, ceil(TROFFSET·90000) + 1] ticks
```

At 1080p59.94: TROFFSET = TRODEFAULT = 43/1125 × TFRAME, RACTIVE = 1080/1125. Today's MTL stamps
the TX-cursor time instead of E(N) by default ([§12.6](#126-the-video-tx-timeline-today)), which is
the mixed-API RTP hazard of [migration.md §6.4](migration.md#64-mixed-api-rtp-hazard).

### 8.4 Where the admission decision happens

submit, QUEUED, pick-up (the decision), first packet, last packet, with `margin_ns`,
`min_submit_lead_ns` and `pickup_slack_ns`: [timing.md §6.1](timing.md#61-where-the-decision-happens)
(from [archive/06 §7.1](archive/06-timing-pacing-and-sync.md)).

### 8.5 Video, audio and ANC on one timeline

The worked mechanism at 59.94p and 48 kHz (T0, RTP per essence, launch per essence) is
[timing.md §10.1](timing.md#101-mechanism); the example picture is
[§2.9](#29-video-audio-and-captions-from-one-file-ex07), the start picture [§4.3](#43-starting-sessions-together).

### 8.6 Where the time comes from

From `enum mtl_time_source` in `mtl.h`; the table of sources is
[timing.md §2.2](timing.md#22-time-sources).

```mermaid
flowchart LR
    A["MTL_TIME_SOURCE_AUTO"] --> P{"a disciplined<br/>NIC PHC?"}
    P -->|"yes"| PHC["PHC"]
    P -->|"no"| T{"CLOCK_TAI with a<br/>non-zero kernel offset?"}
    T -->|"yes"| CT["CLOCK_TAI"]
    T -->|"no"| S["SYSTEM_TAI: ESTIMATED,<br/>time state FREERUN;<br/>the FREERUN source<br/>from Phase 7"]
    N["named: PTP_BUILTIN, PHC,<br/>CLOCK_TAI, SYSTEM_TAI, USER"] --> X["that source only;<br/>AUTO never starts<br/>the built-in PTP client"]
```

LOCKED exists only for `PTP_BUILTIN`, `PHC`, `CLOCK_TAI` and `USER`. AUTO re-evaluating while
running (moving to a better source, holdover, fallback by `time.fallback`) is Phase 7. Every
source feeds the published time base, read wait-free by the tasklets:
[engine.md §8](engine.md#8-the-published-time-base), [timing.md §2.4](timing.md#24-the-published-time-base).

### 8.7 RX timing

Media time and media index from RTP, and the due time of a unit, are formula pictures in
[timing.md §11.2](timing.md#112-media-time-and-media-index-from-rtp) and
[§11.7](timing.md#117-due-time-completion-and-2022-7-skew). Timing without PTP (`MTL_MEDIA_SENDER`)
is [timing.md §13](timing.md#13-timing-without-ptp-phase-7-later) (Phase 7, later).

## 9. Results, events and errors

### 9.1 What happens to a submitted TX unit

From `enum mtl_tx_status` and [contract.md §6.3](contract.md#63-statuses-and-reasons).

```mermaid
flowchart TB
    S["accepted submit:<br/>seq assigned"] --> X{"removed while queued,<br/>or not sendable?"}
    X -->|"stop FLUSH, discard, close,<br/>withdraw, abort, ERROR"| FL["MTL_TX_FLUSHED"]
    X -->|"no"| D{"picked up by<br/>its deadline?"}
    D -->|"yes"| B["built and paced"]
    D -->|"no: tx.late_policy"| LP{"policy"}
    LP -->|"DROP (INDEX, TAI)"| DR["MTL_TX_DROPPED<br/>its slot stays empty"]
    LP -->|"SEND_LATE, in tolerance"| LT["MTL_TX_LATE"]
    LP -->|"RESLOT (AUTO)"| RS["a later slot,<br/>MTL_TXR_RESLOTTED"]
    RS --> B
    X -->|"not sendable: no neighbour,<br/>link down, every leg disabled"| DR
    B -->|"sent"| OT["MTL_TX_ON_TIME"]
    B -->|"device or queue failure"| FA["MTL_TX_FAILED"]
```

Every accepted submission has exactly one terminal outcome: a result (published in submission
order, never lost: the ring holds `pool_count` entries and acquire reserves one) or, with results
off, a counter. The reasons per status are in the contract table; the testable identities
(accepted = published + suppressed + not yet terminal) are
[contract.md §6.2](contract.md#62-lossless-ordered-exactly-once).

### 9.2 Events and shared queues

From `mtl_queue.h` and [contract.md §10](contract.md#10-events-and-queues); the ready summary
behind a queue is [engine.md §5.6](engine.md#56-shared-queues-mtl_queueh).

```mermaid
flowchart LR
    SE["session events<br/>mtl_session_read_events"] -->|"MTL_BIND_EVENTS: a copy"| Q["mtl_queue_h<br/>one wait handle"]
    RES["TX results<br/>mtl_tx_reap"] -->|"MTL_BIND_RESULTS: instead"| Q
    RDY["RX readiness<br/>mtl_rx_dequeue"] -->|"MTL_BIND_READY"| Q
    INST["port, time, instance and<br/>session-state events<br/>MTL_SUB_*"] --> Q
    Q --> RD["mtl_queue_reap,<br/>mtl_queue_ready,<br/>mtl_queue_read_events"]
    Q --> DSP["or a dispatch thread<br/>calls your function"]
```

Bind in CREATED or STOPPED. Results stay lossless and in submission order per session; events are
coalesced and may overflow (`MTL_EVENT_OVERFLOW`: re-read the getters, since every state an event
reports also has a getter). A dispatch function may not close, stop or update a bound session,
close the queue or close the instance (`-MTL_EDEADLK`).

### 9.3 Return codes

[§2.20](#220-what-a-return-code-tells-you), and in full [contract.md §8](contract.md#8-errors-and-reasons).

## 10. Shutdown, pods and crash safety

### 10.1 The instance shutdown order

The network-first sequence (kubelet, application, MTL, network and NIC, MtlManager) is
[deployment.md §4.2](deployment.md#42-shutdown) (from
[archive/16 §2.2](archive/16-kubernetes-and-crash-safety.md)); the one-row version for a talk is
[presentation/slides.md, "Running in a Kubernetes pod"](presentation/slides.md#running-in-a-kubernetes-pod),
and the application's side is [§2.13](#213-a-service-in-a-kubernetes-pod-ex11).

### 10.2 The stalled-queue path

From [archive/03 §6.2](archive/03-object-model-and-lifecycle.md) and
[engine.md §9](engine.md#9-close-and-error-on-a-stalled-queue). Used by close, ERROR entry, link
loss and shutdown.

```mermaid
flowchart TB
    A["bounded idle cleanup:<br/>rte_eth_tx_done_cleanup, up to<br/>max(2 x completion latency, 10 ms)"] --> D{"descriptors released?"}
    D -->|"yes"| OK["device references gone:<br/>SESSION_RETIRED may follow"]
    D -->|"no, dedicated queue"| QS["worker: tx queue stop, then start;<br/>free callbacks claim each unit once<br/>(FAILED or FLUSHED/CLOSE)"]
    D -->|"no, shared queue"| SH["mark the session's frames;<br/>reset at the queue's last user;<br/>session stays CLOSING"]
    QS --> D2{"stopped and restarted?"}
    D2 -->|"yes"| OK
    D2 -->|"no"| PR["port reset: every queue of the port"]
    SH --> OK
    PR --> D3{"reset worked?"}
    D3 -->|"yes"| OK
    D3 -->|"no"| QU["quarantine: nothing it can reach is freed;<br/>instance close -MTL_EIO (QUEUE_QUARANTINED);<br/>a later open of the port -MTL_EBUSY"]
```

Whether iavf and ice release chained external mbufs on queue stop is spike S8.

### 10.3 Instance phases and probes

From `mtl_observe.h` (`enum mtl_phase`, `MTL_HEALTH_*`) and
[deployment.md §4.10](deployment.md#410-health-and-probes).

```mermaid
stateDiagram-v2
    direction LR
    [*] --> LINKS: open returns, devices up
    LINKS --> TIME: links up
    TIME --> READY: time base no longer acquiring
    LINKS --> SHUTDOWN: close or shutdown
    TIME --> SHUTDOWN: close or shutdown
    READY --> SHUTDOWN: close or shutdown
```

| Probe ([ex11](sketch/examples/ex11_signal.c)) | 503 when |
|---|---|
| startup, liveness | before open returns; any `MTL_HEALTH_LIVENESS` bit: `SCHED_STALLED`, `WORKER_STALLED`, `SESSION_LOST`, `DEVICE_FAULT`. After the shutdown began: 200 until exit |
| readiness | before open returns; any `MTL_HEALTH_READINESS` bit: the liveness bits and `STARTING`, `NO_LINK`, `TIME_UNLOCKED`, `MANAGER_LOST`, `SHUTTING_DOWN`; after the shutdown began (`-MTL_ESHUTDOWN`) |

`mtl_instance_get_health` is lock-free from any thread; liveness never depends on packets, links or
PTP lock, so a grandmaster outage or a switch reboot never restarts a pod. `MTL_HEALTH_DEGRADED`
is information only.

### 10.4 Crash safety

What holds after each kind of ending (close, shutdown, abort, exit, SIGKILL, fork) is
[contract.md §2.5](contract.md#25-what-holds-after-each-kind-of-ending) and
[deployment.md §4.5](deployment.md#45-the-crash-contract).

## 11. NMOS and IPMX

### 11.1 One IS-05 activation

[§2.15](#215-an-is-05-activation-at-one-instant-ex13); the contract is
[nmos-ipmx.md §4](nmos-ipmx.md#4-is-05-the-activation-contract).

### 11.2 Update states

From `enum mtl_update_state` and the `mtl_session_update` comment in `mtl.h`. Phase 2; cancel is
Phase 7, later.

```mermaid
stateDiagram-v2
    direction LR
    [*] --> NONE
    NONE --> PENDING: update posted while ARMED or RUNNING
    NONE --> APPLIED: update in CREATED or STOPPED
    PENDING --> APPLIED: the slot boundary at or after when
    PENDING --> FAILED: e.g. the time base stepped (TIME_STEP)
    PENDING --> REPLACED: a newer update
    PENDING --> CANCELLED: update with parts 0 (Phase 7)
```

The status reports the last update call; each posted update (not a dry run, not a cancel) adds 1
to `update_seq`, so a Node reads `update_seq` after the call and matches `MTL_EVENT_UPDATE` (new =
state, value[0] = applied TAI, value[1] = seq). FAILED keeps the old configuration;
`update_applied_tai_ns` is `INT64_MIN` until APPLIED.

### 11.3 Where PEP sits in a packet (Phase 7, later)

From [archive/interop/I1 §6.1](archive/interop/I1-ipmx-requirements.md); the design is
[nmos-ipmx.md](nmos-ipmx.md) (PEP, `mtl_crypto.h`).

```text
| Eth 14 | IPv4 20 | UDP 8 | RTP 12, X=1 | RFC 8285 ext: 0xBEDE + len (4) + CTR Short (4) or Full (16) | payload header (clear) | payload: 16-byte slices, the last may be partial | [MAC 8, MAC modes] |
```

## 12. Today's engines

What `lib/` does at `545a266a`, drawn from the research notes; citations are `lib/src/st2110/`
lines, re-checked at that commit.

### 12.1 Execution contexts today

From [archive/research/03 §1](archive/research/03-scheduler-threading.md); what changes is
[engine.md §2](engine.md#2-the-pinned-core-rules).

```mermaid
flowchart TB
    APP["application threads:<br/>control create, free, update;<br/>fast get, put, mbuf;<br/>wait BLOCK_GET"]
    subgraph PIN["pinned EAL lcores (default)"]
        SCH["lcore k: mtl_sch loop over tasklets:<br/>video builder and transmitter, audio,<br/>RX video and audio, ANC, fastmeta,<br/>CNI, PTP, SRSS, user tasklets"]
        PKT["lcore m: rv_pkt_lcore_func<br/>(ST20_RX_FLAG_USE_MULTI_THREADS)"]
        TAP["lcore t: TAP background thread"]
        CB["user callbacks: get_next_frame,<br/>notify_*, query_ext_frame;<br/>session spinlock held (H1)"]
    end
    subgraph LIB["library pthreads"]
        ADM["mtl_admin (6 s), stat, CNI,<br/>SRSS, socket TX and RX"]
        EAL["TSC calibration at init,<br/>EAL interrupt and alarm thread"]
    end
    PLG["plugin threads:<br/>ST22 encoders, decoders, converters"]
    APP -->|"API calls"| SCH
    SCH -->|"calls, on the pinned core"| CB
```

### 12.2 The blocking get and put wake-up today

From [archive/research/03 §3.3](archive/research/03-scheduler-threading.md); hazard H3 in
[engine.md §2.1](engine.md#21-what-todays-code-does) (`pipeline/st20_pipeline_tx.c:29-34`,
`:41-43`, `:774-789`).

```mermaid
sequenceDiagram
    participant A as App thread (st20p_tx_get_frame)
    participant T as Tasklet (frame done)
    A->>A: claim FREE to IN_USER (CAS) fails
    A->>A: lock block_wake_mutex
    A->>A: claim again, then cond_timedwait (releases the mutex)
    T->>T: store FREE (release)
    T->>T: user notify_frame_available, user code on the pinned core
    T->>T: lock block_wake_mutex: may FUTEX_WAIT while the app holds it
    T->>A: cond_signal: FUTEX_WAKE syscall if a waiter
    T->>T: unlock: FUTEX_WAKE if contended
```

The unified core replaces this with the armed-waiter protocol and the waker
([§6.2](#62-who-makes-the-wake-up-syscall)).

### 12.3 A TX video frame today

From [archive/research/04 §2.1](archive/research/04-memory-buffers.md) (`struct st_frame_trans`).

```mermaid
flowchart LR
    E["EXT only: the app sets the frame,<br/>st20_tx_set_ext_frame (:4502)"] --> F["FREE<br/>refcnt 0"]
    F -->|"builder get_next_frame:<br/>refcnt 0 checked (:1936-1944),<br/>inc (:1963)"| S["SENDING<br/>refcnt 1"]
    S --> P["per packet: attach_extbuf,<br/>sh_info + 1 (:1293-1298),<br/>or a copy into the mbuf"]
    P --> CB["sh_info reaches 0: DPDK calls<br/>tv_frame_free_cb (:116-141):<br/>notify_frame_done, refcnt - 1"]
    CB --> F
```

With no chain, `tv_frame_free_cb` is called directly after the last packet is built (`:2130-2134`).
Recovery and stop force `tv_notify_frame_done`, `refcnt--` and `sh_info = 0` (`:4295-4300`), the
double-completion window SF-39 and the zeroed count SF-41 of
[engine.md §5.3](engine.md#53-every-completing-context).

### 12.4 An RX video frame today

From [archive/research/04 §2.2](archive/research/04-memory-buffers.md).

```mermaid
stateDiagram-v2
    direction LR
    [*] --> FREE
    FREE --> ASSEMBLING: rv_get_frame scan, refcnt + 1 (st_rx_video_session.c:205-220)
    ASSEMBLING --> APP: complete, notify_frame_ready(addr)
    ASSEMBLING --> FREE: incomplete without RECEIVE_INCOMPLETE, or dynamic ext query failed
    APP --> FREE: st20_rx_put_framebuff(addr) (:4692), rv_put_frame refcnt - 1 (:222-232)
```

A negative `notify_frame_ready` return puts a complete frame back (`:937-944`); on an incomplete
frame the return is ignored (`:978`). The dynamic ext-frame query is at `:1259`.

### 12.5 The st20p TX paths today

From [archive/research/04 §2.3](archive/research/04-memory-buffers.md); the pipeline states are
`pipeline/st20_pipeline_tx.h:11-20`, mapped onto the lease table in
[engine.md §3.2](engine.md#32-slot-states).

```mermaid
flowchart LR
    P["put_ext_frame"] -->|"derive"| D["st20_tx_set_ext_frame"] --> CV["CONVERTED"] --> IT["IN_TRANSMITTING"] --> ND["NIC done:<br/>frame_done,<br/>notify_frame_done"]
    P -->|"internal converter"| IC["convert in the caller thread"] --> NI["notify_frame_done(src) at once:<br/>network not done"]
    P -->|"plugin converter"| RD["READY"] --> PC["plugin converts,<br/>convert_put_frame"] --> NP["notify_frame_done(src):<br/>network not done"]
```

In the two converter paths the source frame is reusable before the network is done with the
converted one; the unified result is the transport outcome on every path.

### 12.6 The video TX timeline today

From [archive/research/05 §3.1](archive/research/05-timing-pacing.md).

```text
  epoch N boundary = N·T_FRAME          (T = the field period if interlaced)
  |◀──────────────────────────────── T_FRAME ───────────────────────────────▶|
  N·T       start_N = N·T + TR_OFFSET − VRX·TRS                          (N+1)·T
  |──────────────────|══════ ~total_pkts × TRS (active) ══════|──── idle ────|
                     ^ packet 0 target (tsc_time_cursor)
  RTP (default)    = round_tick(start_N)          not N·T
  RTP (EPOCH flag) = round(90k × N·T)             ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH
  the builder decides N at get_next_frame(), about ring_count (≤ 512 packets) × TRS before the previous frame ends
```

### 12.7 TX queueing stages today

From [archive/research/05 §7.1](archive/research/05-timing-pacing.md) (video pipeline).

```mermaid
flowchart LR
    A["app put_frame"] --> F["framebuffs:<br/>FIFO by seq"]
    F --> C["converter or plugin<br/>(optional): CONVERTED"]
    C --> B["builder get_next_frame:<br/>the epoch is decided here"]
    B --> R["rte_ring, up to 512 packets<br/>(about 1.9 ms at 1080p60)"]
    R --> T["transmitter:<br/>TSC, RL or TSN wait"]
    T --> Q["NIC TX queue<br/>nb_tx_desc, default 512"]
    Q --> W["RL shaper, wire"]
```

### 12.8 The legacy instance and session lifecycle

From [archive/research/13 §1.1, §1.2](archive/research/13-lifecycle-errors-abi.md); the unified
calls that replace them are [migration.md §2](migration.md#2-call-map-per-legacy-family).

```mermaid
stateDiagram-v2
    direction LR
    state "Instance (mtl_handle)" as INST {
        [*] --> INITIALIZED: mtl_init, ports and admin up
        INITIALIZED --> STARTED: mtl_start, schedulers run
        STARTED --> INITIALIZED: mtl_stop, ports stay up
        INITIALIZED --> [*]: mtl_uninit, EAL cleaned up, no second init
    }
    state "Session (st20, st20p, ...)" as SESS {
        [*] --> ACTIVE: create, attached to a scheduler
        ACTIVE --> [*]: free, handle dangling
    }
```

With `MTL_FLAG_DEV_AUTO_START_STOP`, init calls start and `mtl_stop()` is a no-op; `mtl_abort()`
only sets a flag. A legacy session has no start or stop: its tasklet runs while the instance is
started, and a fatal error sets `active = false` (`ST_EVENT_FATAL_ERROR`, TX video only). Today
`mtl_uninit` with live sessions may self-deadlock (SP-01).

## 13. Roadmap

### 13.1 Phases

From [archive/14 §0](archive/14-implementation-roadmap.md) and
[archive/LIST-OF-CHANGES.md §18](archive/LIST-OF-CHANGES.md), with the scope of
[implementation-plan.md §6](implementation-plan.md#6-m7-the-remaining-phases). Port first (D-98):
Phases 1–6 port what MTL does today, plus the Kubernetes lifecycle.

```mermaid
flowchart TB
    P0["Phase 0: decisions, soname and version script,<br/>spikes, check.sh in CI, design review"] --> P1
    P0 --> P05["Phase 0.5, independent: st_timeline_* helper,<br/>ST30P and ST40P flags, SF-15, on the legacy API"]
    P0 --> E["engines track: E1-E13, R1, R2, EK1-EK21;<br/>legacy gets every fix, wire changes opt-in"]
    E --> P1["Phase 1: core, null backend, ST20 over st20p,<br/>instance close, health, shutdown"]
    P1 --> P2["Phase 2: every essence, queues, stats schema;<br/>operators: legs, update, link monitor"]
    P2 --> P2P["Phase 2P: packet units, generic RTP essence"]
    P2P --> P3["Phase 3: timing core: timelines, start arrays,<br/>time sources, ANC windows"]
    P3 --> P4["Phase 4: memory: regions, attach,<br/>named slots, holds, RX_BY_INDEX"]
    P4 --> P5["Phase 5: robustness at the I tier:<br/>VF reset, recovery on a worker, fault matrix"]
    P5 --> P6["Phase 6: row units, plugin rewrites,<br/>legacy re-base, ABI freeze MTL_1.0"]
    P6 -.-> P7["Phase 7, later: NMOS contract extras, SDP,<br/>IPMX timing and RTCP, IPMX profile, PEP"]
```

### 13.2 The branch

The ST20-first branch (M0 skeleton to M6 nightly) is
[implementation-plan.md §2](implementation-plan.md#2-where-the-branch-sits-in-the-phases); the
one-row delivery picture for a talk is
[presentation/slides.md, "Delivery"](presentation/slides.md#delivery).

## 14. Diagrams left in the archive

Every other diagram of the old set is either carried by a picture above or by another maintained
document (see the index), or left in the archive on purpose:

| Diagram | Where | Why it stays there |
|---|---|---|
| layers L0–L4 (text) | [archive/02 §2](archive/02-architecture.md) | revision-3 names (`mtl_unified.h`, `mtl_simple.h`, L4); replaced by [engine.md §1](engine.md#1-the-layer-picture) |
| handle bit layout (text) | [archive/03 §2.2](archive/03-object-model-and-lifecycle.md) | revision 3: type and generation fields in the handle; revision 4 has distinct C handle types and process-wide slots that are never freed (R4) |
| group create, add, start (text) | [archive/03 §5](archive/03-object-model-and-lifecycle.md) | groups were replaced by start arrays ([§4.3](#43-starting-sessions-together)) |
| results capacity (text) | [archive/07 §2.1](archive/07-completions-events-and-errors.md) | built on the completion modes NONE, EXCEPTIONS, ALL, which were rejected; the rule now is in [contract.md §6.2](contract.md#62-lossless-ordered-exactly-once) |
| exactly-once identities, `BY_INDEX` sizing, the timing formulas (text) | [archive/07 §6](archive/07-completions-events-and-errors.md), [archive/05 §5.1](archive/05-memory-and-buffers.md), [archive/06](archive/06-timing-pacing-and-sync.md), [archive/research/12](archive/research/12-st2110-timing-standards.md) | formulas, not pictures; carried in [contract.md §6.2](contract.md#62-lossless-ordered-exactly-once), [§9.8](contract.md#98-rx-placement) and [timing.md](timing.md) §2–§11 |
| the minimal alternative: `stream`, `mtl_get`, `mtl_put` | [archive/simplification/S6 §1](archive/simplification/S6-minimal-alternative.md) | a rejected alternative |
| five words | [archive/LEARN.md §2](archive/LEARN.md) | superseded by "ten words", [concepts.md §2.1](concepts.md#21-ten-words) |
| PR #1610's object model and lifecycle; the research lists of questions, policies and lifecycles | [archive/research/01 §1.1, §1.4](archive/research/01-pr1610-analysis.md), [archive/research/00](archive/research/00-pr1610-design-review.md) | they describe PR #1610's code or are lists, not pictures; the outcome is [§7.1](#71-four-separate-ideas), [§7.2](#72-where-a-sessions-memory-comes-from) and [migration.md §14](migration.md#14-where-the-code-goes-and-pr-1610) |
| progressive `publish` chain | [archive/06 §9](archive/06-timing-pacing-and-sync.md), [archive/research/00 §12](archive/research/00-pr1610-design-review.md) | revision-3 `mtl_tx_publish`; rows are now resubmits with a larger `used` ([§2.10](#210-rows-leave-before-the-frame-is-finished-ex08)) |
| event and completion queue shape (sq, cq, waiter word, doorbell) | [archive/research/03 §7.2](archive/research/03-scheduler-threading.md) | a research proposal, replaced by the lease table and `mtl_queue.h` ([§9.2](#92-events-and-shared-queues)) |
| proposed session state machine | [archive/research/13 §6.1](archive/research/13-lifecycle-errors-abi.md) | superseded by [contract.md §4.1](contract.md#41-states) |
| Rivermax object model and memory hierarchy | [archive/research/10 §1.4, §2.2](archive/research/10-rivermax.md) | another product's API; the mapping is [migration.md §13](migration.md#13-if-you-know-libfabric-or-rivermax) |
| new memory model mapped onto internals (research version) | [archive/research/04 §7](archive/research/04-memory-buffers.md) | the earlier form of [§7.4](#74-mapping-onto-todays-internals) |

## 15. Index of every diagram

Every diagram of the maintained set: the mermaid and text pictures of this page and of the other
maintained documents, and the three tables that stand in for pictures here. Text blocks of
[timing.md](timing.md) are formula pictures. Diagrams left in the archive are listed in
[§14](#14-diagrams-left-in-the-archive).

| # | Diagram | Where | Shows | Kind |
|---|---|---|---|---|
| 1 | 2.1 Send video (ex01) | [diagrams.md#21-send-video-ex01](#21-send-video-ex01) | ex01: typed config, open, acquire, draw, submit | flowchart |
| 2 | 2.2 Receive video on two networks (ex02) | [diagrams.md#22-receive-video-on-two-networks-ex02](#22-receive-video-on-two-networks-ex02) | ex02: two ST 2022-7 legs merged, dequeue, release | flowchart |
| 3 | 2.3 One frame's life | [diagrams.md#23-one-frames-life](#23-one-frames-life) | a TX frame: free, yours, queued, on the wire | state |
| 4 | 2.4 In your own event loop (ex03) | [diagrams.md#24-in-your-own-event-loop-ex03](#24-in-your-own-event-loop-ex03) | ex03: drain until -MTL_EAGAIN, then epoll | flowchart |
| 5 | 2.5 A session's life | [diagrams.md#25-a-sessions-life](#25-a-sessions-life) | the application's path through the session states | state |
| 6 | 2.6 Zero copy from a framework's pool (ex04) | [diagrams.md#26-zero-copy-from-a-frameworks-pool-ex04](#26-zero-copy-from-a-frameworks-pool-ex04) | ex04: framework surface i is slot i; result frees it | flowchart |
| 7 | 2.7 Received frames handed to a framework (ex05) | [diagrams.md#27-received-frames-handed-to-a-framework-ex05](#27-received-frames-handed-to-a-framework-ex05) | ex05: dequeue, wrap, release on any thread; unlock | flowchart |
| 8 | 2.8 Into an MXL ring by frame number (ex06) | [diagrams.md#28-into-an-mxl-ring-by-frame-number-ex06](#28-into-an-mxl-ring-by-frame-number-ex06) | ex06: frame k into MXL grain k mod 8 | flowchart |
| 9 | 2.9 Video, audio and captions from one file (ex07) | [diagrams.md#29-video-audio-and-captions-from-one-file-ex07](#29-video-audio-and-captions-from-one-file-ex07) | ex07: three essences, one timeline, one start | flowchart |
| 10 | 2.10 Rows leave before the frame is finished (ex08) | [diagrams.md#210-rows-leave-before-the-frame-is-finished-ex08](#210-rows-leave-before-the-frame-is-finished-ex08) | ex08: row units published by resubmits (Phase 6) | flowchart |
| 11 | 2.11 One 4K frame in, four HD streams out, no copy (ex09) | [diagrams.md#211-one-4k-frame-in-four-hd-streams-out-no-copy-ex09](#211-one-4k-frame-in-four-hd-streams-out-no-copy-ex09) | ex09: four TX pools over one RX pool, no copy | flowchart |
| 12 | 2.12 A processor that keeps the input's timing (ex10) | [diagrams.md#212-a-processor-that-keeps-the-inputs-timing-ex10](#212-a-processor-that-keeps-the-inputs-timing-ex10) | ex10: output keeps the input's media time and RTP | flowchart |
| 13 | 2.13 A service in a Kubernetes pod (ex11) | [diagrams.md#213-a-service-in-a-kubernetes-pod-ex11](#213-a-service-in-a-kubernetes-pod-ex11) | ex11: SIGTERM, interrupt, join, bounded shutdown, probes | flowchart |
| 14 | 2.14 RTP passthrough: you build the packets (ex12) | [diagrams.md#214-rtp-passthrough-you-build-the-packets-ex12](#214-rtp-passthrough-you-build-the-packets-ex12) | ex12: app-built RTP packets in chunks (Phase 2P) | flowchart |
| 15 | 2.15 An IS-05 activation at one instant (ex13) | [diagrams.md#215-an-is-05-activation-at-one-instant-ex13](#215-an-is-05-activation-at-one-instant-ex13) | ex13: one PATCH as one update; planned and applied instants | sequence |
| 16 | 2.16 The same API from C++ (examples_cpp.cpp) | [diagrams.md#216-the-same-api-from-c-examples_cppcpp](#216-the-same-api-from-c-examples_cppcpp) | examples_cpp.cpp: the RAII lease guard | flowchart |
| 17 | 2.17 Media time and launch time | [diagrams.md#217-media-time-and-launch-time](#217-media-time-and-launch-time) | media time, RTP and launch from frame k | flowchart |
| 18 | 2.18 Which header do I need? | [diagrams.md#218-which-header-do-i-need](#218-which-header-do-i-need) | header per job, with its phase | table |
| 19 | 2.19 Under the hood: nothing waits on the pinned cores | [diagrams.md#219-under-the-hood-nothing-waits-on-the-pinned-cores](#219-under-the-hood-nothing-waits-on-the-pinned-cores) | tasklet stores, waker wakes, the app drains | sequence |
| 20 | 2.20 What a return code tells you | [diagrams.md#220-what-a-return-code-tells-you](#220-what-a-return-code-tells-you) | the codes a loop sees, and what to do | table |
| 21 | 3.1 The shape of the API | [diagrams.md#31-the-shape-of-the-api](#31-the-shape-of-the-api) | mtl.h, optional headers, Phase 7 headers, libraries | flowchart |
| 22 | 3.3 Threads and the pinned cores | [diagrams.md#33-threads-and-the-pinned-cores](#33-threads-and-the-pinned-cores) | application, library and pinned threads and their hand-offs | flowchart |
| 23 | 3.4 Objects and ownership | [diagrams.md#34-objects-and-ownership](#34-objects-and-ownership) | instance, ports, sessions, regions, timelines, queues, leases | flowchart |
| 24 | 3.6 The legacy headers and where they go | [diagrams.md#36-the-legacy-headers-and-where-they-go](#36-the-legacy-headers-and-where-they-go) | today's legacy include graph | flowchart |
| 25 | 3.6 The legacy headers and where they go | [diagrams.md#36-the-legacy-headers-and-where-they-go](#36-the-legacy-headers-and-where-they-go) | install layout of the public, legacy and internal tiers | text |
| 26 | 3.6 The legacy headers and where they go | [diagrams.md#36-the-legacy-headers-and-where-they-go](#36-the-legacy-headers-and-where-they-go) | stages of hiding the legacy headers, Phase 0 to F+2 | flowchart |
| 27 | 3.7 The shape shared with libfabric and Rivermax | [diagrams.md#37-the-shape-shared-with-libfabric-and-rivermax](#37-the-shape-shared-with-libfabric-and-rivermax) | register, slot, fill, submit, completion in three APIs | flowchart |
| 28 | 4.2 What close does | [diagrams.md#42-what-close-does](#42-what-close-does) | the steps inside CLOSING until RETIRED | state |
| 29 | 4.3 Starting sessions together | [diagrams.md#43-starting-sessions-together](#43-starting-sessions-together) | array start: validate, resolve T0, arm, run | flowchart |
| 30 | 4.4 Legs and flow states | [diagrams.md#44-legs-and-flow-states](#44-legs-and-flow-states) | per-leg flow states, TX and RX | state |
| 31 | 5.1 One TX unit through the layers | [diagrams.md#51-one-tx-unit-through-the-layers](#51-one-tx-unit-through-the-layers) | a TX unit across app, L2 and the engine | sequence |
| 32 | 5.2 One RX unit through the layers | [diagrams.md#52-one-rx-unit-through-the-layers](#52-one-rx-unit-through-the-layers) | an RX unit across the engine, L2 and the app | sequence |
| 33 | 5.4 Free, yours, MTL's: both directions | [diagrams.md#54-free-yours-mtls-both-directions](#54-free-yours-mtls-both-directions) | free, yours, MTL's for TX and RX | state |
| 34 | 5.5 One TX frame, step by step | [diagrams.md#55-one-tx-frame-step-by-step](#55-one-tx-frame-step-by-step) | one TX frame per loop iteration | sequence |
| 35 | 6.2 Who makes the wake-up syscall | [diagrams.md#62-who-makes-the-wake-up-syscall](#62-who-makes-the-wake-up-syscall) | armed or not; W3 waker, W2 direct, direct wake | flowchart |
| 36 | 6.3 Interrupts | [diagrams.md#63-interrupts](#63-interrupts) | session, queue and instance interrupts; close wins | flowchart |
| 37 | 7.1 Four separate ideas | [diagrams.md#71-four-separate-ideas](#71-four-separate-ideas) | region, slot, lease, unit and result | flowchart |
| 38 | 7.2 Where a session's memory comes from | [diagrams.md#72-where-a-sessions-memory-comes-from](#72-where-a-sessions-memory-comes-from) | library pool, alloc, import, device import, pool lending | flowchart |
| 39 | 7.3 Forwarding with holds | [diagrams.md#73-forwarding-with-holds](#73-forwarding-with-holds) | an RX slot held by four TX units | sequence |
| 40 | 7.4 Mapping onto today's internals | [diagrams.md#74-mapping-onto-todays-internals](#74-mapping-onto-todays-internals) | regions and attached slots onto ext frames | flowchart |
| 41 | 8.2 Media modes | [diagrams.md#82-media-modes](#82-media-modes) | AUTO, INDEX, TAI, SENDER to M, RTP and launch | flowchart |
| 42 | 8.3 One video frame on the ST 2110-21 schedule | [diagrams.md#83-one-video-frame-on-the-st-2110-21-schedule](#83-one-video-frame-on-the-st-2110-21-schedule) | E(N), TVD, read schedule, first packet, JT-NM windows | text |
| 43 | 8.6 Where the time comes from | [diagrams.md#86-where-the-time-comes-from](#86-where-the-time-comes-from) | AUTO's choice of time source | flowchart |
| 44 | 9.1 What happens to a submitted TX unit | [diagrams.md#91-what-happens-to-a-submitted-tx-unit](#91-what-happens-to-a-submitted-tx-unit) | ON_TIME, LATE, DROPPED, FLUSHED, FAILED | flowchart |
| 45 | 9.2 Events and shared queues | [diagrams.md#92-events-and-shared-queues](#92-events-and-shared-queues) | what a queue gathers and how it is read | flowchart |
| 46 | 10.2 The stalled-queue path | [diagrams.md#102-the-stalled-queue-path](#102-the-stalled-queue-path) | cleanup, queue stop and start, port reset, quarantine | flowchart |
| 47 | 10.3 Instance phases and probes | [diagrams.md#103-instance-phases-and-probes](#103-instance-phases-and-probes) | LINKS, TIME, READY, SHUTDOWN | state |
| 48 | 10.3 Instance phases and probes | [diagrams.md#103-instance-phases-and-probes](#103-instance-phases-and-probes) | which health bits fail which probe | table |
| 49 | 11.2 Update states | [diagrams.md#112-update-states](#112-update-states) | NONE, PENDING, APPLIED, FAILED, REPLACED, CANCELLED | state |
| 50 | 11.3 Where PEP sits in a packet (Phase 7, later) | [diagrams.md#113-where-pep-sits-in-a-packet-phase-7-later](#113-where-pep-sits-in-a-packet-phase-7-later) | PEP packet layout (Phase 7) | text |
| 51 | 12.1 Execution contexts today | [diagrams.md#121-execution-contexts-today](#121-execution-contexts-today) | today's lcores, pthreads and plugin threads | flowchart |
| 52 | 12.2 The blocking get and put wake-up today | [diagrams.md#122-the-blocking-get-and-put-wake-up-today](#122-the-blocking-get-and-put-wake-up-today) | today's BLOCK_GET mutex and condvar on the tasklet | sequence |
| 53 | 12.3 A TX video frame today | [diagrams.md#123-a-tx-video-frame-today](#123-a-tx-video-frame-today) | today's st_frame_trans refcount on TX | flowchart |
| 54 | 12.4 An RX video frame today | [diagrams.md#124-an-rx-video-frame-today](#124-an-rx-video-frame-today) | today's RX frame refcount | state |
| 55 | 12.5 The st20p TX paths today | [diagrams.md#125-the-st20p-tx-paths-today](#125-the-st20p-tx-paths-today) | today's st20p put_ext_frame paths | flowchart |
| 56 | 12.6 The video TX timeline today | [diagrams.md#126-the-video-tx-timeline-today](#126-the-video-tx-timeline-today) | today's epoch, start_N, RTP default and EPOCH flag | text |
| 57 | 12.7 TX queueing stages today | [diagrams.md#127-tx-queueing-stages-today](#127-tx-queueing-stages-today) | today's queueing from put_frame to the wire | flowchart |
| 58 | 12.8 The legacy instance and session lifecycle | [diagrams.md#128-the-legacy-instance-and-session-lifecycle](#128-the-legacy-instance-and-session-lifecycle) | today's mtl_handle and session lifecycle | state |
| 59 | 13.1 Phases | [diagrams.md#131-phases](#131-phases) | Phases 0 to 7 and the engines track | flowchart |
| 60 | 2.1 Ten words | [concepts.md#21-ten-words](concepts.md#21-ten-words) | instance, session, unit, results, events, timeline, options | flowchart |
| 61 | 5.1 TX | [concepts.md#51-tx](concepts.md#51-tx) | a TX unit's life, including dropped and flushed | state |
| 62 | 5.2 RX | [concepts.md#52-rx](concepts.md#52-rx) | an RX unit from packets to release | sequence |
| 63 | 5.3 A session's life | [concepts.md#53-a-sessions-life](concepts.md#53-a-sessions-life) | the main session states | state |
| 64 | 7.1 Two times, not one | [concepts.md#71-two-times-not-one](concepts.md#71-two-times-not-one) | media time, RTP and the source kinds | flowchart |
| 65 | 8. Memory and zero copy in one page | [concepts.md#8-memory-and-zero-copy-in-one-page](concepts.md#8-memory-and-zero-copy-in-one-page) | zero copy from a framework's pool | flowchart |
| 66 | 4.1 States | [contract.md#41-states](contract.md#41-states) | every state and transition of a session | state |
| 67 | 2.4 The published time base | [timing.md#24-the-published-time-base](timing.md#24-the-published-time-base) | the published time base record and its reader | text |
| 68 | 3.1 The timeline object | [timing.md#31-the-timeline-object](timing.md#31-the-timeline-object) | M(k) = T0 + k x P | text |
| 69 | 3.3 The start formula | [timing.md#33-the-start-formula](timing.md#33-the-start-formula) | the start formula for T0 | text |
| 70 | 4.2 Derived RTP: exact, and its exceptions | [timing.md#42-derived-rtp-exact-and-its-exceptions](timing.md#42-derived-rtp-exact-and-its-exceptions) | derived RTP per essence | text |
| 71 | 5.1 The ST 2110-21 model | [timing.md#51-the-st-2110-21-model](timing.md#51-the-st-2110-21-model) | the ST 2110-21 model and formulas | text |
| 72 | 6.1 Where the decision happens | [timing.md#61-where-the-decision-happens](timing.md#61-where-the-decision-happens) | submit to last packet: deadline and margins | text |
| 73 | 9.2 The ANC transmit window and live ANC | [timing.md#92-the-anc-transmit-window-and-live-anc](timing.md#92-the-anc-transmit-window-and-live-anc) | the ANC transmit window | text |
| 74 | 10.1 Mechanism | [timing.md#101-mechanism](timing.md#101-mechanism) | video, audio and ANC on one timeline, worked | text |
| 75 | 11.2 Media time and media index from RTP | [timing.md#112-media-time-and-media-index-from-rtp](timing.md#112-media-time-and-media-index-from-rtp) | media time and media index from RX RTP | text |
| 76 | 11.7 Due time, completion and 2022-7 skew | [timing.md#117-due-time-completion-and-2022-7-skew](timing.md#117-due-time-completion-and-2022-7-skew) | the RX due time and skew budget | text |
| 77 | 13. Timing without PTP (Phase 7, later) | [timing.md#13-timing-without-ptp-phase-7-later](timing.md#13-timing-without-ptp-phase-7-later) | RTP and launch without PTP (Phase 7) | text |
| 78 | 1. The layer picture | [engine.md#1-the-layer-picture](engine.md#1-the-layer-picture) | L3 to backends, and the legacy APIs | flowchart |
| 79 | 3.2 Slot states | [engine.md#32-slot-states](engine.md#32-slot-states) | the TX lease-table states | state |
| 80 | 3.2 Slot states | [engine.md#32-slot-states](engine.md#32-slot-states) | the RX lease-table states | state |
| 81 | 5.2 The completing-context protocol | [engine.md#52-the-completing-context-protocol](engine.md#52-the-completing-context-protocol) | completing context and reader | text |
| 82 | 6. Commands and acknowledgements | [engine.md#6-commands-and-acknowledgements](engine.md#6-commands-and-acknowledgements) | a flow update: APP, worker, tasklet, ack | text |
| 83 | 7.1 Wait targets and the armed-waiter protocol | [engine.md#71-wait-targets-and-the-armed-waiter-protocol](engine.md#71-wait-targets-and-the-armed-waiter-protocol) | the armed-waiter protocol | text |
| 84 | 8. The published time base | [engine.md#8-the-published-time-base](engine.md#8-the-published-time-base) | the published time base record | text |
| 85 | 9. Close and ERROR on a stalled queue | [engine.md#9-close-and-error-on-a-stalled-queue](engine.md#9-close-and-error-on-a-stalled-queue) | the steps of a session close | text |
| 86 | 2. Where the branch sits in the phases | [implementation-plan.md#2-where-the-branch-sits-in-the-phases](implementation-plan.md#2-where-the-branch-sits-in-the-phases) | the branch milestones M0 to M7+ | flowchart |
| 87 | 6. Coexistence | [migration.md#6-coexistence](migration.md#6-coexistence) | legacy and unified code on one instance | flowchart |
| 88 | 4.2 Shutdown | [deployment.md#42-shutdown](deployment.md#42-shutdown) | the network-first shutdown in a pod | sequence |
| 89 | The idea in one picture | [presentation/slides.md#the-idea-in-one-picture](presentation/slides.md#the-idea-in-one-picture) | the idea: app, core, engines, pinned cores | flowchart |
| 90 | Nothing on the pinned cores | [presentation/slides.md#nothing-on-the-pinned-cores](presentation/slides.md#nothing-on-the-pinned-cores) | the waker hand-off | sequence |
| 91 | A/V/ANC in sync by construction | [presentation/slides.md#avanc-in-sync-by-construction](presentation/slides.md#avanc-in-sync-by-construction) | A/V/ANC on one timeline | flowchart |
| 92 | RTP passthrough, coherently | [presentation/slides.md#rtp-passthrough-coherently](presentation/slides.md#rtp-passthrough-coherently) | RTP passthrough | flowchart |
| 93 | Running in a Kubernetes pod | [presentation/slides.md#running-in-a-kubernetes-pod](presentation/slides.md#running-in-a-kubernetes-pod) | the pod shutdown in one row | flowchart |
| 94 | Delivery | [presentation/slides.md#delivery](presentation/slides.md#delivery) | the phases in one row | flowchart |
