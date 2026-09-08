---
description: "Use when editing the acceptance_tests execution engine under tests/acceptance/mtl_engine/ — the Application ABC and its RxTxApp/FFmpeg adapters, the UNIVERSAL_PARAMS vocabulary, media registry, compliance session, integrity helpers, and reporting. Covers which module owns what, the adapter contract, and the legacy modules not to extend."
name: "MTL Acceptance Tests — Engine & Adapters"
applyTo: "tests/acceptance/mtl_engine/**"
---

# Editing `tests/acceptance/mtl_engine/`

Architecture: [doc/acceptance-design.md](../../doc/acceptance-design.md) §2, §5.

## Module ownership — put the code in the right file

| File | Owns | Do NOT put here |
|---|---|---|
| `application_base.py` | Everything application-**agnostic**: params, process ladders, timeouts, PTP budget, oracle dispatch | Any RxTxApp or FFmpeg specific knowledge |
| `rxtxapp.py` | RxTxApp JSON config + argv + its result parsing | Generic lifecycle logic |
| `ffmpeg.py` | FFmpeg argv + its result parsing | Generic lifecycle logic |
| `config/universal_params.py` | The **only** list of legal test kwargs | Defaults only one adapter understands, unless that adapter strips them in `set_params()` |
| `rxtxapp_config.py` | Empty JSON config templates | Logic (it is data) |
| `media_files.py` | Asset registry + `parse_fps_to_pformat` | File I/O |
| `pcap_compliance.py` | `CaptureIntent`, `ComplianceSession`, `NO_COMPLIANCE`, mismatch detectors | Netsniff process control (that is `create_pcap_file/netsniff.py`) |
| `integrity.py` | Frame/sample comparison + size math | Test policy about *when* to run it |
| `execute.py` | `run()`, `log_fail()`, stale-process cleanup | Anything app-specific |
| `csv_report.py`, `stash.py` | Result recording | Pass/fail decisions |

## The adapter contract

A new adapter subclasses `Application` and implements exactly four methods:
`get_app_name`, `get_executable_name`, `_create_command_and_config`,
`validate_results`. Optional hooks: `require_encoder`, `prepare_execution`,
`_resolve_capture_dst_ip`, `set_params` (to strip adapter-only kwargs).

- **Adapters never import each other.** `RxTxApp` and `FFmpeg` are siblings;
  shared behaviour goes in the base class or a helper module.
- **Only override a concrete base method when behaviour genuinely differs,
  and comment why.** `FFmpeg.execute_test()` is the precedent: its traffic
  only flows once the last process is up, so it arms capture on
  `after_last_start`.
- **`self.params` is reset on every `create_command()`.** Adapter instances
  are session-scoped and reused; never cache derived state across calls, and
  snapshot `CaptureIntent` once per run rather than re-reading params later.
- **Unknown kwargs must raise.** `set_params()` rejects anything not in
  `UNIVERSAL_PARAMS`; strip adapter-private kwargs before calling `super()`.

## Failure semantics

- Oracles return `True` on success, `False` on soft failure. Never return
  `0`/`1` — the MTL C convention is inverted from Python truthiness and does
  not apply here.
- Route failures through `_fail_validation()`, which **always raises**
  `AssertionError`; `_dispatch_validate` converts it to `False` when
  `fail_on_error=False` (used by performance binary-search loops). Do not
  swallow it yourself and do not make it return.
- **Never degrade silently.** A missing config, a failed upload, or an
  unparseable response is a failure, not a skipped check. If a code path
  cannot evaluate what it was asked to evaluate, fail or at minimum
  `logger.warning` — never `return` quietly.
- The compliance verdict and `validate_results()` must each run even if the
  other fails (`_finalize_run` catches them independently).
- **An oracle that needs a longer run must lengthen the run, not lower its
  bar.** `GStreamer._graded_wall_clock()` is the precedent: every oracle it has
  grades a duration — st20p/st40p by MTL's per-interval frame counters, which
  need two completed 10 s intervals, and st30p by bytes per second of window,
  where a short window is mostly pipeline startup. So a `test_time` under 30 s
  extends the wall clock (with a `logger.warning`) instead of excusing the
  check; st40p has no byte oracle to fall back on, so relaxing the interval
  count there would turn a false fail into a false pass. Extend only the local
  wall clock: `self.params["test_time"]` stays the requested value, because
  `_apply_ptp_extension` also adds up to 50 s of non-delivering time and a byte
  floor charged for it would fail a healthy PTP run. `gen_config.py` sizes the
  ramdisk against the *extended* window — over-provision disk, under-state
  delivery.
- **The RX dump must cover the whole window, and one adapter grades that it
  does.** No adapter caps the dump by default — RxTxApp leaves
  `rx_max_file_size` at 0 and the GStreamer st20p sink is a plain `filesink` —
  because a one-frame sink would let a byte floor and the MD5 integrity check
  pass on a session that delivered one frame out of the window. Only
  `GStreamer._expected_rx_bytes()` then checks the dump is full-length; RxTxApp
  grades delivery from MTL's log counters instead, and FFmpeg's
  `check_output_video_yuv` only asserts `output_file_size > 0`. So when adding a
  *log-counter* oracle, RxTxApp is the parity reference and FFmpeg is not; a
  *byte* oracle has only the GStreamer one to copy. The media ramdisk is sized
  for the full dump by `configs/gen_config.py::_media_ramdisk_gib()`.

## Legacy modules — do not extend

`RxTxApp.py` (procedural, still the backend for `tests/dual/st20p|st30p|st40/`
and `tests/single/performance/`), `ffmpeg_app.py` (command builders called by
`ffmpeg.py`), and `GstreamerApp.py` (everything under `tests/dual/gstreamer/`
and `tests/single/gstreamer/`; those 8 modules get no wall-clock floor and run
for exactly the configured `test_time`)
predate the adapter model. Add new functionality to `Application`, not to
these. Note the capitalisation trap: `RxTxApp.py` is legacy, `rxtxapp.py` is
modern.

## Before handing back

Run the checks in
[mtl-acceptance-harness.instructions.md](mtl-acceptance-harness.instructions.md#before-handing-back).
