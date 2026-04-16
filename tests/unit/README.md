# Unit Tests — RX Redundancy Filters

## Scope

These tests exercise the **per-packet RX redundancy filtering** logic for
ST2110-20 (video), ST2110-30 (audio), and ST2110-40 (ancillary data).
Each test suite calls the production packet handler directly through a
lightweight C harness — no NIC ports, no real network, no sudo required.

### What is tested

| Suite | Production function | Production source file |
|-------|---------------------|------------------------|
| ST40 (ancillary) | `rx_ancillary_session_handle_pkt()` | `lib/src/st2110/st_rx_ancillary_session.c` |
| ST30 (audio) | `rx_audio_session_handle_frame_pkt()` | `lib/src/st2110/st_rx_audio_session.c` |
| ST20 (video) | `rv_handle_frame_pkt()` | `lib/src/st2110/st_rx_video_session.c` |

Each handler is exposed for direct calling via a `fuzz_handle_pkt` wrapper
(compiled under `MTL_ENABLE_FUZZING_ST*` defines). The tests cover:

- **Validation filters** — PT, SSRC, F-bits (interlace), payload length
- **Redundancy filtering** — timestamp-based and seq-based duplicate rejection
- **Per-port statistics** — packets, bytes, OOO counters per port
- **Threshold bypass** — force-accept after N consecutive redundant errors
- **Slot management** (ST20) — bitmap dedup, frame-gone path, slot reuse
- **Frame assembly** (ST30, ST20) — frame completion, incomplete frame eviction
- **Edge cases** — seq/timestamp wraparound, backward arrivals, cross-port interleaving

### What is NOT tested

The full RX pipeline (burst polling, tasklet scheduling, DMA offload, frame
callbacks to the application) is out of scope. These tests isolate the
single-packet decision logic only.

## Architecture

```
  test.cpp  ──►  harness.h (opaque C API)  ──►  harness.c  ──►  production handler
  (gtest)        ut_feed_pkt()                   builds mbuf     rx_*_handle_pkt()
                 ut_stat_*()                     calls handler   (lib/src/st2110/)
```

### Why the C/C++ split?

Internal MTL headers use `new` as a C variable name, which is a reserved
keyword in C++. The harness `.c` files include the internal headers and
expose an **opaque context** (`ut_test_ctx*`) through a pure-C API in the
`.h` files. The gtest `.cpp` files never include internal MTL headers.

### What the harness stubs

| Component | How it's stubbed |
|-----------|-----------------|
| DPDK EAL | `rte_eal_init()` with `--no-huge --no-pci --vdev=net_null0` |
| Packet buffers | `rte_pktmbuf_pool_create()` with 2048 mbufs |
| Packet ring (ST40) | `rte_ring_create()` — 512 entries, drained after each test |
| Frame pool (ST30, ST20) | Pre-allocated arrays with atomic refcounting |
| PTP clock (ST30, ST20) | Stub returning monotonically increasing microseconds |
| Session structs | Zeroed and minimally initialized inline |

### Test geometry

| Suite | Packet layout | Frame size |
|-------|--------------|------------|
| ST40 | RFC 8331 ANC RTP header | Per-packet (ring enqueue) |
| ST30 | PCM16 2ch 48kHz 1ms — 192 bytes/pkt | 8192 bytes, 42 pkts/frame |
| ST20 | RFC 4175 YUV422-10bit 16×2 — 40 bytes/pkt | 80 bytes, 2 pkts/frame |

## Build & Run

```bash
# Configure (one-time)
meson setup build_unit -Denable_unit_tests=true

# Build
ninja -C build_unit

# Run
LD_LIBRARY_PATH=build_unit/lib ./build_unit/tests/unit/UnitTest
```

## File listing

```
tests/unit/
├── meson.build                        # Build definition
├── README.md                          # This file
├── st40_rx_redundancy_harness.h       # ST40 opaque C API
├── st40_rx_redundancy_harness.c       # ST40 harness (builds ANC mbufs, calls handler)
├── st40_rx_redundancy_test.cpp        # ST40 gtest cases + main()
├── st30_rx_redundancy_harness.h       # ST30 opaque C API
├── st30_rx_redundancy_harness.c       # ST30 harness (builds audio mbufs, frame pool)
├── st30_rx_redundancy_test.cpp        # ST30 gtest cases
├── st20_rx_redundancy_harness.h       # ST20 opaque C API
├── st20_rx_redundancy_harness.c       # ST20 harness (builds video mbufs, slot setup)
└── st20_rx_redundancy_test.cpp        # ST20 gtest cases
```
