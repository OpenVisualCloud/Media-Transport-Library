# Unified MTL API — samples at a glance

| | |
|---|---|
| Status | Visual companion — revision 4 |
| Date | 2026-10-01 |
| Reads with | [diagrams.md](diagrams.md) (the pictures), [10 — API sketch](../10-api-sketch.md) (the examples with their full text), [sketch/README.md](../../sketch/README.md) (the header set) |

This directory is the visual entry point to the proposed unified MTL API, in the spirit of
PR #1610's `doc/new_API/samples/diagrams.md`. It is design material; no library code implements it
yet.

**Every example compiles.** The files in [`../sketch/examples/`](../../sketch/examples/) are built
warning-free as C99 and C++17 (gcc and clang) by [`../sketch/check.sh`](../../sketch/check.sh),
against the headers. They are never linked: application hooks such as `render()` are declared,
not defined. Every function in the pictures exists in the headers.

## The examples

Personas are those of [01 §2](../01-goals-and-requirements.md): P1 simple generator or player, P2
framework integrator, P3 broadcast playout, P4 live capture or gateway, P5 zero-copy forwarder or
processor, P7 libfabric- or Rivermax-familiar developer, P9 operator or NMOS integrator.

| Example | Persona | What it shows | Headers | Picture |
|---|---|---|---|---|
| [ex01_tx_video.c](../../sketch/examples/ex01_tx_video.c) | P1 | the smallest sender: ports from `MTL_PORTS`, a typed config, acquire → draw → submit, close | `mtl.h` | [1](diagrams.md#1-send-video-ex01), [3](diagrams.md#3-one-frames-life) |
| [ex02_rx_video.c](../../sketch/examples/ex02_rx_video.c) | P1 | a receiver on two ST 2022-7 legs; lost packets read as zero | `mtl.h` | [2](diagrams.md#2-receive-video-on-two-networks-ex02) |
| [ex03_event_loop.c](../../sketch/examples/ex03_event_loop.c) | P2, P7 | a sender in the app's own epoll loop, a result per frame | `mtl.h` | [4](diagrams.md#4-in-your-own-event-loop-ex03) |
| [ex04_zero_copy_tx.c](../../sketch/examples/ex04_zero_copy_tx.c) | P2, P5 | a framework's pool attached in one call; surface i = slot i; close returns when the memory is free | `mtl_mem.h` | [5](diagrams.md#5-zero-copy-from-a-frameworks-pool-ex04) |
| [ex05_rx_to_framework.c](../../sketch/examples/ex05_rx_to_framework.c) | P2 | received frames lent to a framework, released on any thread; GStreamer `unlock` | `mtl.h` | [6](diagrams.md#6-received-frames-handed-to-a-framework-ex05) |
| [ex06_mxl_ring.c](../../sketch/examples/ex06_mxl_ring.c) | P5 | frame k lands in MXL grain k mod 8 | `mtl_mem.h` | [7](diagrams.md#7-into-an-mxl-ring-by-frame-number-ex06) |
| [ex07_av_anc_playout.c](../../sketch/examples/ex07_av_anc_playout.c) | P3 | video, audio and captions from one file, started together, exact RTP | `mtl_sync.h`, `mtl_util.h` | [8](diagrams.md#8-video-audio-and-captions-from-one-file-ex07), [15](diagrams.md#15-media-time-and-launch-time) |
| [ex08_progressive_rows.c](../../sketch/examples/ex08_progressive_rows.c) | P4 | rows leave before the frame is finished (SDI-to-IP gateway) | `mtl_sync.h` | [9](diagrams.md#9-rows-leave-before-the-frame-is-finished-ex08) |
| [ex09_split_forwarder.c](../../sketch/examples/ex09_split_forwarder.c) | P5 | one 4K frame in, four HD streams out, no copy | `mtl_mem.h`, `mtl_util.h` | [10](diagrams.md#10-one-4k-frame-in-four-hd-streams-out-no-copy-ex09) |
| [ex10_processor.c](../../sketch/examples/ex10_processor.c) | P5 | a processor that keeps the input's media time and RTP | `mtl_util.h` | [11](diagrams.md#11-a-processor-that-keeps-the-inputs-timing-ex10) |
| [ex11_signal.c](../../sketch/examples/ex11_signal.c) | P9, any service | a service in a Kubernetes pod: SIGTERM, probes from health, a bounded network-first shutdown with a report | `mtl_observe.h` | [12](diagrams.md#12-a-service-in-a-kubernetes-pod-ex11) |
| [ex12_rtp_packets.c](../../sketch/examples/ex12_rtp_packets.c) | P5, P7 | RTP passthrough: app-built packets, library pacing, both legs; RX packet chunks | `mtl_packet.h` | [13](diagrams.md#13-rtp-passthrough-you-build-the-packets-ex12) |
| [ex13_nmos_switch.c](../../sketch/examples/ex13_nmos_switch.c) | P9 | one IS-05 PATCH as one update (re-apply is Phase 7): destinations and `rtp_enabled` on both legs at one instant; the planned instant (the 202 response) and the applied one (in `/active`) | `mtl_util.h` | [14](diagrams.md#14-an-is-05-activation-at-one-instant-ex13) |
| [examples_cpp.cpp](../../sketch/examples/examples_cpp.cpp) | P2 (bindings) | the same API from C++17: `MTL_INIT`, options, an RAII lease guard | `mtl.h`, `mtl_options.h` | — |

## How to read the pictures

| Notation | Meaning |
|---|---|
| blue box | your application's own code |
| green box | an MTL call, or MTL's work |
| grey circle | the network, or another system (MXL readers, an NMOS controller) |
| arrow labels | verb names only; the arguments are in the example |
| one picture, one idea | at most about eight boxes; error handling is in [picture 18](diagrams.md#18-what-a-return-code-tells-you) |

Return codes are always negative `MTL_E*` constants with the Linux errno values; `0` is
success, and array reads return a count.

## Checking

```bash
doc/unified-api/sketch/check.sh      # compiles every header and example
```

The mermaid blocks in [diagrams.md](diagrams.md) were rendered with mermaid 11 in headless
Chromium and inspected; they use only the subset GitHub renders (flowchart, sequence,
stateDiagram-v2).
