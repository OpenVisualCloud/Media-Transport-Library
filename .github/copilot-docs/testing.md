# Testing

## Test Suite Overview

MTL has **four** testing layers, each targeting a different fidelity/speed tradeoff:

| Layer | Framework | Language | What it tests | Speed |
|-------|-----------|----------|---------------|-------|
| **Integration tests** (gtest) | Google Test | C++ | Internal APIs via loopback sessions | Minutes |
| **Fuzz tests** | LLVM libFuzzer | C | RX packet parsers with random input | Hours (continuous) |
| **Validation tests** (pytest) | pytest | Python | Full-stack E2E via RxTxApp/FFmpeg/GStreamer processes | 10s of minutes |
| **Shell script tests** | bash | Shell | RxTxApp JSON-driven loopback scenarios | Minutes each |

---

## 1. Integration Tests (C++ / gtest)

**Location:** `tests/integration_tests/`

### Binaries

Three executables built from `tests/meson.build`:

| Binary | Purpose | Links MTL? |
|--------|---------|------------|
| `KahawaiTest` | Main ST2110 integration tests + noctx tests | Yes |
| `KahawaiUfdTest` | Custom UDP file descriptor (`mufd_*`) tests | Yes |
| `KahawaiUplTest` | LD_PRELOAD UDP interception tests | **No** (uses standard POSIX sockets) |

### Build Flags
- Non-release: defines `MTL_SIMULATE_PACKET_DROPS=1` (needed for noctx redundancy tests)
- gtest ≥ 1.9.0 sets `MTL_GTEST_AFTER_1_9_0` (switches `INSTANTIATE_TEST_CASE_P` → `INSTANTIATE_TEST_SUITE_P`)
- Optional ASAN via `enable_asan` meson option

### Architecture — Shared MTL Context

- `main()` in `tests.cpp` creates **one** global `mtl_init()` instance for the entire run
- `st_test_ctx()` returns global `st_tests_context*` — holds MTL handle, IPs, plugin handles
- Sessions are ephemeral (created/destroyed per test), but DPDK cannot reinit within a process
- Test mock encoder/decoder/converter plugins are registered globally before `RUN_ALL_TESTS()`

### Port Loopback Model
- `--p_port` (TX) and `--r_port` (RX) — two NICs or VFs wired for loopback
- Random IPs in `197.x.x.1`, multicast in `239.x.x.x`
- `same_dual_port` flag when both ports are identical (e.g., `kernel:lo`)

### CLI Flags

| Flag | Purpose |
|------|---------|
| `--p_port` / `--r_port` | Primary/redundant NIC (PCI/`kernel:eth0`/`af_xdp:eth0`) |
| `--p_sip` | Override primary source IP |
| `--port_list` | Comma-separated up to 8 ports |
| `--lcores` | EAL lcore list |
| `--log_level` | debug/info/notice/warning/error |
| `--level` | `all` or `mandatory` (filter test level) |
| `--pacing_way` | auto/rl/tsn/tsc/ptp/be |
| `--rss_mode` | l3/l3_l4/none |
| `--no_ctx_tests` | Skip `mtl_init()`, run only noctx tests |
| `--dma_dev` | DMA device list |
| `--tsc` | TSC pacing |
| `--tx_no_chain` | Disable TX mbuf chaining |
| `--auto_start_stop` | Auto start/stop flag |

### Test Suites

| Suite | File(s) | What |
|-------|---------|------|
| `Main` | `st_test.cpp` | `mtl_init` validation, start/stop, lcores, bandwidth calc, format utils |
| `Misc` | `tests.cpp` | Version, memcpy, malloc, PTP, log level, NUMA, ST10 timestamp conversions |
| `St20_tx`/`St20_rx` | `st20/` (14 files) | Session-level video: create/free, digest, ext_frame, FPS, detect, dump, user_pacing |
| `St20p` | `st20p_test.cpp` | Pipeline video: plugin registration, format conversion, digest, RTCP, ext_frame |
| `St22_tx`/`St22_rx` | `st22_test.cpp` | Compressed video session: create/free, digest, FPS |
| `St22p` | `st22p_test.cpp` | Compressed pipeline: encode/decode, logo, failure simulation |
| `St30_tx`/`St30_rx` | `st30_test.cpp` | Audio session: PCM8/16/24, frame + RTP modes, digest |
| `St30p` | `st30p_test.cpp` | Audio pipeline: get/put frame, digest |
| `St40_tx`/`St40_rx` | `st40_test.cpp` | Ancillary: DID/SDID/UDW construction, parity, checksum, digest |
| `Sch` | `sch_test.cpp` | Scheduler create/max, tasklet registration + runtime add |
| `Dma` | `dma_test.cpp` | DMA copy/fill with offset/alignment sweeps (skip if no DMA) |
| `Cvt` | `cvt_test.cpp` (4349 lines!) | Pixel format conversion at NONE/AVX512/AVX512_VBMI2 SIMD levels, round-trip |
| `Api` / `Loop` | `ufd_test.cpp` / `ufd_loop_test.cpp` | `mufd_*` socket API, loopback unicast/multicast/poll |
| `Loop` | `upl_loop_test.cpp` | LD_PRELOAD POSIX socket: sendmsg, recvmsg, GSO, select, epoll, SO_REUSEPORT |
| `NoCtxTest` | `noctx/` | Isolated MTL init per test (see below) |

### Core Verification Patterns

| Pattern | How |
|---------|-----|
| **Data integrity (digest)** | TX: fill frame with random data → SHA256. RX: receive → SHA256 → compare |
| **FPS accuracy** | Count received frames over timed window, assert ≥ expected |
| **Create/free stress** | `create_free_test(base, step, repeat)` — create N, then add/remove in loops |
| **Expected failure** | Create with invalid params (0 ports, 100 ports, bad FB count) → expect NULL |
| **Level filtering** | `para.level = ST_TEST_LEVEL_MANDATORY` always runs; `ALL` needs `--level all` |

### Noctx Tests (`tests/integration_tests/noctx/`)

Tests requiring **isolated MTL init/uninit per test** (DPDK can't reinit, so each runs as separate process invocation).

- `NoCtxTest` fixture: fresh `st_tests_context` per test, `mtl_init()` in setup, `mtl_uninit()` in teardown
- OOP design: `Session`, `Handlers`, `PipelineHandlerBase<>` templates, `FrameTestStrategy` pattern
- Requires 4 ports (`--port_list=PCI1,PCI2,PCI3,PCI4`)
- Run all via `noctx/run.sh` (serial execution, 10s cooldown between tests)

**Test cases:** st20p/st30p/st40p redundant latency with simulated packet loss, user pacing/timestamps, st40 interleaved mode, split ANC tests

### Running Integration Tests

```bash
# All tests
./build/tests/KahawaiTest --p_port 0000:af:00.0 --r_port 0000:af:00.1

# Specific suite
./build/tests/KahawaiTest --p_port ... --r_port ... --gtest_filter="St20p.*"

# Specific test
./build/tests/KahawaiTest ... --gtest_filter="St20p.digest_rtcp_s1"

# Mandatory only (faster)
./build/tests/KahawaiTest ... --level mandatory

# UDP FD tests
./build/tests/KahawaiUfdTest --p_port ... --r_port ...

# UDP preload (must LD_PRELOAD the interposition library)
LD_PRELOAD=libmtl_udp_preload.so ./build/tests/KahawaiUplTest --p_sip ... --r_sip ...

# Noctx (serial, separate process per test)
cd tests/integration_tests/noctx && ./run.sh
```

---

## 2. Fuzz Tests (libFuzzer)

**Location:** `tests/fuzz/`

### Strategy

Fuzzes **RX packet parsers** — the primary untrusted-input attack surface. Each harness injects randomized bytes into the internal packet handler, bypassing the network stack entirely.

### Harnesses

| Target | File | What is fuzzed | Min input size |
|--------|------|-----------------|----------------|
| `st20_rx_frame_fuzz` | `st20/st20_rx_frame_fuzz.c` | ST2110-20 RFC4175 video RX parsing | `sizeof(st20_rfc4175_rtp_hdr)` |
| `st22_rx_frame_fuzz` | `st22/st22_rx_frame_fuzz.c` | ST2110-22 RFC9134 compressed video RX | `sizeof(st22_rfc9134_video_hdr)` |
| `st30_rx_frame_fuzz` | `st30/st30_rx_frame_fuzz.c` | ST2110-30 audio RX parsing | `sizeof(st_rfc3550_audio_hdr)` |
| `st40_rx_rtp_fuzz` | `st40/st40_rx_rtp_fuzz.c` | ST2110-40 ancillary RX parsing | `sizeof(st_rfc8331_anc_hdr)` |
| `st40_ancillary_helpers_fuzz` | `st40/st40_ancillary_helpers_fuzz.c` | UDW, checksum, parity helpers | 1 byte |

### Architecture

1. **One-time init:** Minimal DPDK EAL (`--no-huge --no-shconf -c1 -n1 --no-pci --vdev=net_null0`) + mbuf pool
2. **Per-input:** Full session context reset (deterministic state) → copy fuzz data into `rte_mbuf` → call `*_fuzz_handle_pkt()`
3. **Library hooks:** `#ifdef MTL_ENABLE_FUZZING_*` gates expose thin wrapper functions (e.g., `st_rx_video_session_fuzz_handle_pkt()`) that forward to the real internal handler

### Attack Surface Coverage

| Surface | Covered |
|---------|---------|
| ST2110-20 video RX | Yes |
| ST2110-22 compressed video RX | Yes |
| ST2110-30 audio RX | Yes |
| ST2110-40 ancillary RX | Yes |
| ST40 helper functions | Yes |
| TX paths, control plane, PTP/RTCP, UDP/socket, CNI | **No** |

### Build & Run

```bash
# Build (requires clang for libFuzzer)
meson setup build_fuzz -Denable_fuzzing=true -Denable_asan=true
ninja -C build_fuzz

# Run
mkdir -p corpus/st20
./build_fuzz/tests/fuzz/st20_rx_frame_fuzz corpus/st20
./build_fuzz/tests/fuzz/st22_rx_frame_fuzz -max_total_time=300 corpus/st22
./build_fuzz/tests/fuzz/st40_ancillary_helpers_fuzz corpus/st40_helpers  # no DPDK needed
```

No seed corpus or dictionaries shipped — create from pcap captures for best coverage.

### Gotcha
- Each fuzz input is a **single isolated packet** — multi-packet sequence bugs won't be found
- 4 of 5 harnesses require DPDK EAL (heavier than typical fuzz targets)
- ASAN opt-in only; no MSAN/UBSAN integration currently

---

## 3. Validation Tests (Python / pytest)

**Location:** `tests/validation/`

### What It Is

End-to-end test framework that launches **real MTL applications** (RxTxApp, FFmpeg, GStreamer) over actual or loopback NICs, then validates results through log parsing, frame integrity, FPS checks, and optional EBU LIST compliance.

**Key insight:** Tests do NOT call MTL's C API. They orchestrate external processes via SSH.

### Directory Structure

```text
tests/validation/
├── conftest.py          # Fixtures, CLI options, session/function scope setup
├── pytest.ini           # Markers: smoke, nightly, dual, verified, ptp
├── requirements.txt     # 99 deps including mfd-* Intel test infra packages
├── common/              # nicctl (VF mgmt), MtlManager, integrity runners
├── compliance/          # EBU LIST REST client for ST2110 compliance
├── configs/             # topology_config.yaml + test_config.yaml + gen_config.py
├── create_pcap_file/    # netsniff-ng / tcpdump wrappers
├── fuzzing/             # Python wrapper that runs libFuzzer binaries
├── mtl_engine/          # Core: RxTxApp, FFmpeg, GStreamer adapters
│   ├── rxtxapp.py       # New refactored RxTxApp class
│   ├── RxTxApp.py       # Legacy procedural RxTxApp interface
│   ├── ffmpeg_app.py    # FFmpeg MTL plugin command builder
│   ├── GstreamerApp.py  # GStreamer MTL plugin pipeline builder
│   ├── integrity.py     # MD5 per-frame comparison
│   ├── media_files.py   # Test media catalog (YUV, PCM, ANC)
│   ├── media_creator.py # Generate media on-the-fly (GStreamer, sox)
│   ├── ip_pools.py      # IP address generation from session_id
│   └── execute.py       # Local/remote process execution via mfd_connect
└── tests/
    ├── single/          # TX+RX same host
    │   ├── st20p/       # Video: fps, format, resolution, interlace, integrity, pacing
    │   ├── st22p/       # Compressed video (JPEG-XS)
    │   ├── st30p/       # Audio (PCM8/16/24, channels, sampling, ptime)
    │   ├── ancillary/   # ST2110-40
    │   ├── st41/        # Fast Metadata
    │   ├── ffmpeg/      # FFmpeg plugin integration
    │   ├── gstreamer/   # GStreamer plugin integration
    │   ├── performance/ # Max-replica binary search
    │   ├── kernel_socket/ # Kernel socket backend
    │   ├── xdp/         # AF_XDP backend
    │   ├── dma/, rss_mode/, rx_timing/, ptp/, udp/, virtio_user/
    │   └── ...
    ├── dual/            # TX on host A, RX on host B
    │   ├── st20p/, st30p/, st40/, ffmpeg/, gstreamer/
    └── xfail.py         # Known-issue xfail markers
```

### Configuration

Two YAML files drive everything:
- **`topology_config.yaml`** — hosts (SSH creds, PCI BDFs), network interfaces
- **`test_config.yaml`** — `session_id` (seeds IP pool: `192.168.<sid>.*`), `mtl_path`, `media_path`, ramdisk, EBU server, capture config

Generate them: `python configs/gen_config.py --pci_bdf ... --ssh_user ... --ebu_server ...`

### Key Fixtures

| Fixture | Scope | Purpose |
|---------|-------|---------|
| `nic_port_list` | session | Auto-creates VFs via `nicctl.sh` |
| `mtl_manager` | session | Starts MtlManager daemon on all hosts |
| `init_ip_address_pools` | session | Seeds IP pool from `session_id` |
| `rxtxapp` | session | Returns `RxTxApp` application instance |
| `pcap_capture` | function | Runs `netsniff-ng`, uploads pcap to EBU LIST |
| `media_file` | function | Copies media to ramdisk, cleanup after |
| `setup_interfaces` | function | VF/PF creation + cleanup |

### Test Patterns

**Legacy (procedural):**
```python
config = rxtxapp.create_empty_config()
config = rxtxapp.add_st20p_sessions(config, nic_port_list=..., ...)
rxtxapp.execute_test(config, build=mtl_path, test_time=..., host=host)
```

**Refactored (OOP):**
```python
rxtxapp.create_command(session_type="st20p", nic_port_list=..., ...)
rxtxapp.execute_test(build=mtl_path, test_time=..., host=host)
```

Both coexist (e.g., `test_fps.py` + `test_fps_refactored.py`).

### Validation Methods

| Method | When used |
|--------|-----------|
| **Log parsing** | Every test: regex for `app_tx_*_result OK` / `app_rx_*_result OK` |
| **FPS performance** | Parse `TX_VIDEO_SESSION` fps readings, compare ±2 tolerance |
| **Frame integrity** | MD5 per-frame hash of source vs received YUV/PCM |
| **EBU LIST compliance** | Upload pcap → REST API → check `not_compliant_streams == 0` |
| **Packet counts** | UDP tests: verify ≥ 99% delivery |

### Pytest Markers
- `@pytest.mark.smoke` — fast subset
- `@pytest.mark.nightly` — full suite (includes fuzz wrapper)
- `@pytest.mark.dual` — two-host tests
- `@pytest.mark.ptp` — requires PTP sync

### Running

```bash
cd tests/validation
pip install -r requirements.txt

# Smoke
sudo pytest --topology_config=configs/topology_config.yaml \
  --test_config=configs/test_config.yaml -m smoke

# Nightly
sudo pytest ... -m nightly

# Specific test
sudo pytest ... "tests/single/st20p/fps/test_fps.py::test_fps[p29-ParkJoy_1080p]"

# Dual-host
sudo pytest ... -m dual
```

Root required for NIC management, ramdisk mounting, process control.

### External Tool Dependencies
- `RxTxApp`, `MtlManager`, `nicctl.sh` (from MTL build)
- `netsniff-ng` or `tcpdump` (packet capture)
- `phc2sys` / `ptp4l` (PTP sync)
- `ffmpeg` with MTL plugin, `gst-launch-1.0` with MTL plugin
- `sox` (audio generation), `tesseract-ocr` (integrity OCR)
- EBU LIST server (compliance—optional)

---

## 4. Test Tools

**Location:** `tests/tools/`

### RxTxApp (`tests/tools/RxTxApp/`)

The **universal test vehicle** — a JSON-driven reference application for all ST2110 session types. Used by both shell scripts and the pytest validation framework.

**Max sessions:** 180 video, 1024 audio, 180 ancillary, 180 fast metadata (each direction)

**Flow:** Parse CLI/JSON → `mtl_init()` → create all sessions → `mtl_start()` → run for `--test_time` seconds → `mtl_stop()` → report pass/fail via `st_app_result()`

**JSON config directories** (under `tests/tools/RxTxApp/`):

| Directory | Content |
|-----------|---------|
| `loop_json/` | ~80+ loopback configs: every resolution/fps combo, session types, redundant, shared queues |
| `audio_json/` | Audio-specific: shared queue, redundant, st30p |
| `native_af_xdp_json/` | AF_XDP backend: loopback, shared, RSS, redundant |
| `kernel_socket_json/` | Kernel socket: loopback, multicast, UDP client/server |
| `rss_json/` | RSS mode: multicast, unicast, redundant |
| `redundant_json/` | Header-split + standard TX redundant |
| `dhcp_loop_json/` | DHCP-based loopback |
| `perf_json/` | 32-video RX-only performance |

### Shell Test Scripts (`tests/tools/RxTxApp/script/`)

| Script | What it does |
|--------|-------------|
| `loop_test.sh` | Iterates all JSONs in `loop_json/`, runs RxTxApp 100s each |
| `loop_ebu_test.sh` | Same + `--rx_timing_parser` for EBU ST2110-21 timing compliance |
| `st2110_test.sh` | Runs `KahawaiTest` gtest binary with P_PORT/R_PORT args |
| `afxdp_test.sh` | AF_XDP backend loopback |
| `multi_port_test.sh` | Multi-port configs |
| `hdr_split_test.sh` | Header-split RX tests (PF PMD) |
| `ufd_gtest.sh` | `KahawaiUfdTest` in 6 modes (dedicated/shared/RSS ×lcore) |

### GStreamer Test Tools (`tests/tools/gstreamer_tools/`)

Two GStreamer plugin elements:

| Element | Type | Purpose |
|---------|------|---------|
| `anc_generator` | GstBaseSrc | Generates ST2110-40 ancillary data buffers (cycles through 4 packet patterns) |
| `time_inserter` | Filter | Injects TAI timestamps into video PTS for latency measurement |

### Latency Measurement (`tests/tools/latency_measurement/`)

Measures end-to-end ST2110-20/22 video latency:
1. TX FFmpeg overlays timestamp at top of frame → sends via `mtl_st20p`
2. RX FFmpeg overlays its own timestamp at bottom → saves MPEG
3. Python OCR script (`text_detection.py`) reads both timestamps, computes delta

Requires PTP-synced clocks, FFmpeg with MTL+freetype, tesseract, OpenCV.

---

## 5. Test Config File (`tests/config.json`)

Example loopback test config for RxTxApp:
- 2 interfaces (PCI + IPs)
- 1 TX st20p session: 1920×1080 @ 25fps, YUV422 10-bit, BPM, multicast 239.0.0.1:20000
- 1 matching RX session on second interface
- Gap pacing, no RTCP, no display

---

## 6. Key Design Decisions & Gotchas

- **DPDK can't reinit** — the gtest binary uses a single global `mtl_init()`. Tests that need fresh init use the noctx pattern (separate process per test via `run.sh`)
- **Two code paths in pytest** — legacy procedural `RxTxApp.py` (1248 lines) coexists with refactored OOP `rxtxapp.py`. New tests should use the refactored version
- **mfd-* packages** — the pytest framework depends heavily on Intel-internal `mfd-connect`, `mfd-network-adapter`, etc. for SSH/NIC automation. These are required for running validation tests
- **Converter tests are huge** — `cvt_test.cpp` at 4349 lines tests every pixel format conversion at multiple SIMD levels. When adding a new format, add round-trip tests here
- **Fuzz harnesses are single-packet** — each input is one isolated packet. Multi-packet protocol state machine bugs (e.g., sequence-dependent vulnerabilities) are not covered
- **Root is required** for pytest validation tests (NIC VF management, ramdisk mounting)
- **EBU LIST compliance** is optional — requires an external EBU LIST server configured in `test_config.yaml`
- **Test level filtering** — integration tests have `MANDATORY` vs `ALL` levels. CI typically runs `mandatory`; `all` is for thorough local validation
- **UDP preload tests** (`KahawaiUplTest`) do NOT link MTL — they use standard POSIX socket API with LD_PRELOAD interception. This validates that the preload library transparently accelerates unmodified applications
