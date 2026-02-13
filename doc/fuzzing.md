# ST40/ST30/ST20/ST22 RX Fuzzing

This repository now provides libFuzzer harnesses that exercise the runtime ST40 RX
ancillary packet path, the ancillary helper utilities, the ST30 RX audio frame path,
and the ST20/ST22 RX video frame pipelines. The harnesses are built only when the
dedicated Meson option is enabled because they pull in DPDK and need an EAL
environment.

## Build Requirements

- Clang/LLVM with `-fsanitize=fuzzer` support **or** a standalone `libFuzzer` library.
- DPDK must already be available, as required by the main library build.
- Linux is recommended; the harnesses rely on EAL flags such as `--no-huge` and
  `--in-memory`.

## Enabling The Harnesses

You can turn the fuzzers on either directly through Meson **or** via the top-level
`build.sh` helper:

```sh
meson setup build -Denable_fuzzing=true
```

```sh
./build.sh release enable_fuzzing
# or export MTL_BUILD_ENABLE_FUZZING=true before invoking build.sh
```

Add `-Denable_asan=true` (or `MTL_BUILD_ENABLE_ASAN=true`) if you also want
AddressSanitizer instrumentation. Existing build directories can be reconfigured with
`meson configure build -Denable_fuzzing=true`.

Meson injects the `MTL_ENABLE_FUZZING_ST40`, `MTL_ENABLE_FUZZING_ST30`,
`MTL_ENABLE_FUZZING_ST20`, and `MTL_ENABLE_FUZZING_ST22` defines so the RX
implementation exposes the minimal helper hooks required by the fuzzers.

## Building

After configuration simply run the standard build:

```sh
ninja -C build
```

The following fuzz targets will be produced inside `build/tests/fuzz/`:

- `st40_rx_rtp_fuzz` – feeds arbitrary RTP payloads through
  `rx_ancillary_session_handle_pkt`, using a lightweight DPDK/EAL bootstrap.
- `st40_ancillary_helpers_fuzz` – targets the pure helper functions in
  `st_ancillary.c` (`st40_set_udw`, `st40_get_udw`, parity helpers, checksum, etc.).
- `st30_rx_frame_fuzz` – injects fuzzed RFC3550 audio RTP packets into the
  frame-level ST30 RX path, allocating a tiny in-memory framebuffer to exercise the
  sequencing and framing logic.
- `st20_rx_frame_fuzz` – drives the frame-level ST20 video RX path, including slot
  management, RTP parsing, bitmap tracking, and framebuffer assembly for
  RFC4175-formatted video payloads.
- `st22_rx_frame_fuzz` – targets the ST22 codestream RX path, validating packetized
  codestream parsing, JPVS/COLR box handling, and codestream reassembly logic.

## Running

Each target is a standalone libFuzzer executable. A minimal invocation looks like:

```sh
./build/tests/fuzz/st40_rx_rtp_fuzz -runs=1000 corpus_dir
```

The harness configures EAL internally (`--no-huge --in-memory --no-shconf -c1 -n1`),
so no additional environment setup is required beyond access to `/dev/hugepages` not
being mandatory.

A similar invocation drives the ST30 frame harness:

```sh
./build/tests/fuzz/st30_rx_frame_fuzz -max_total_time=60 corpus_audio
```

And the new ST20/ST22 video harnesses can be exercised with:

```sh
./build/tests/fuzz/st20_rx_frame_fuzz -max_total_time=60 corpus_video
./build/tests/fuzz/st22_rx_frame_fuzz -max_total_time=60 corpus_codestream
```

For long fuzzing sessions point the binary at a writable corpus directory. The helper
fuzzer works the same way:

```sh
./build/tests/fuzz/st40_ancillary_helpers_fuzz -max_total_time=60 corpus_helpers
```

### Pytest integration

The validation suite drives every fuzz target with long-running libFuzzer passes and
streams the combined libFuzzer/MTL output into
`tests/validation/logs/latest/pytest.log`. Execute:

```sh
pytest tests/validation/fuzzing/test_fuzzing.py -k fuzz
```

Each test carries `@pytest.mark.nightly` and runs `-runs=500000` iterations by default
(override via `MTL_FUZZ_TEST_RUNS`). That keeps quick developer test shards short while
allowing the nightly job to exercise a deeper corpus.

### Logging during fuzzing

All harnesses automatically raise the MTL and DPDK log level to `DEBUG` and install a
custom printer that forwards every log line to `stderr`. When invoked through pytest (or
manually), you will see `MTL:`-prefixed lines for interesting events—payload-type / SSRC
mismatches, redundant packets, enqueue failures, etc.—inside the test logs without any
extra configuration.

## Notes

- The RX harness drains the session ring immediately after notifying the callback so
  mbufs are always released, which keeps memory usage stable even under ASan.
- Inputs must be at least the size of an RFC8331 ancillary header (62 bytes) to ensure
  the handler never reads beyond the provided mbuf.
- All harnesses cap their input size to fit inside a standard DPDK mbuf (2 KiB) to
  avoid oversized allocations during fuzzing.
- The ST30 harness requires inputs large enough to contain an RFC3550 audio header
  (12 bytes). Packets larger than 2 KiB are truncated so they still fit within a
  single mbuf.
- The ST20 harness expects RFC4175-style video payloads (12-byte RTP header plus
  video-specific extensions). Packets shorter than the combined header size are
  ignored.
- The ST22 harness operates on RFC9134 codestream packets. Inputs must include at
  least the base JP2K header; larger payloads exercise the box parsing and codestream
  completion/marker logic.
