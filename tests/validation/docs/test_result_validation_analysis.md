# Test Result Validation — Analysis & Improvement Plan

> Discussion notes on how MTL validation tests decide pass/fail, what that
> actually proves, and where the gaps are. Captured for follow-up review.

---

## 1. Three layers of validation

Every refactored test goes through up to three layers. Most tests stop at
layer 2.

### Layer 1 — Process exit code (mandatory)

[`RxTxApp.execute_test`](../mtl_engine/RxTxApp.py) checks the return code of
the spawned `RxTxApp` process:

```python
bad_rc = {124: "timeout", 137: "SIGKILL", 143: "SIGTERM"}
if cp.return_code != 0:
    log_fail(...); return False
```

Catches: crashes, hangs killed by timeout, OOM kills, segfaults.
Does **not** catch anything semantic.

### Layer 2 — Log scraping for "OK" lines (default for ~all refactored tests)

The C app emits one summary line per session at shutdown. Example from
[`tests/tools/RxTxApp/src/rx_st22p_app.c#L239`](../../tools/RxTxApp/src/rx_st22p_app.c):

```c
notce("%s(%d), %s, fps %f, %d frame received\n", __func__, idx,
      ST_APP_EXPECT_NEAR(framerate, s->expect_fps, s->expect_fps * 0.05)
          ? "OK" : "FAILED",
      framerate, s->stat_frame_total_received);
```

Python in [`mtl_engine/rxtxapp.py`](../mtl_engine/rxtxapp.py) (`check_rx_output`,
`check_tx_output`) just counts `app_<dir>_<type>_result … OK` lines and
asserts `ok_cnt == replicas`.

| ✅ Proves | ❌ Does NOT prove |
|---|---|
| Process started, ran, exited cleanly | Pixel content matches |
| RX achieved ≥95 % of expected FPS | Audio sample values match |
| Frame count > 0 (st40p) | Ancillary payload bytes match |
| Session was created without error | Frames arrived **in order** |
| | No silent corruption (RTP seq jumps, dropped packets within FPS tolerance) |
| | Color format / endianness / stride correctness |
| | PTP timing correctness |

The ±5 % FPS tolerance window is loose enough to mask thousands of dropped
frames in a long test (e.g. 60 fps × 30 s = 1800 frames; 5 % = 90 frames
silently lost while still printing `OK`).

### Layer 3 — Byte-level integrity (rare, opt-in)

Two implementations exist; only a handful of tests use them.

#### A. `mtl_engine/integrity.py` — simple hash compare

`check_st20p_integrity(src, out, frame_size)`:
- MD5 each frame-sized chunk of input.
- MD5 each frame-sized chunk of output.
- Compare position-by-position up to `min(len(src), len(out))`.

`check_st30p_integrity` — same for raw audio.

Used by [`test_integrity_refactored.py`](../tests/single/st20p/integrity/test_integrity_refactored.py).

**Hidden weaknesses:**
- Compares from frame 0. If RX dropped the first frame of the looped source,
  every subsequent frame mismatches → false fail or undetected shift.
- Output silently truncated to `len(src)` frames (the source loops on TX but
  RX may have started mid-loop).
- No SSIM/PSNR — a single corrupted byte fails the frame; lossy codecs
  (st22p) cannot use this at all.

#### B. `common/integrity/` — smarter, OCR-aligned

`VideoIntegritor` ([`video_integrity.py`](../common/integrity/video_integrity.py)):
- Same MD5-per-frame, but `shift_src_chunk_by_first_frame_no` extracts a
  frame number from the YUV using OpenCV + Tesseract OCR (the source media
  is generated with a frame-number burn-in).
- Falls back to **hash-based alignment**: hash the first RX frame, find that
  hash in the source list, rotate the source list — finally a way to detect
  "we started receiving from frame 73."

`FileAudioIntegrityRunner` ([`audio_integrity.py`](../common/integrity/audio_integrity.py)):
- Knows the source loops. Compares only `min(src_frames, out_frames)`,
  ignores trailing repeats.

Used by: `rss_mode/audio`, `st30p_ptime`, integrity tests.
**Not** used by st20p (most tests) / st22p / st40p / fastmetadata refactored
tests in general.

---

## 2. What "PASS" really means per session type

| Session type | Default check | Strongest available check | Gap |
|---|---|---|---|
| **st20p** (uncompressed video) | Layer 2 (FPS ±5 %) | Layer 3A (MD5 frames) — `test_integrity_refactored` only | Most refactored st20p tests skip Layer 3 |
| **st22p** (compressed video) | Layer 2 (FPS ±5 %) | None — MD5 doesn't work on lossy output | No correctness check at all |
| **st30p / audio** | Layer 2 + opt-in `FileAudioIntegrityRunner` | Layer 3B (MD5 chunks, loop-aware) | Many tests have `integrity_check` toggle defaulting on, but not universally |
| **st40p** (ancillary) | Just "frames received" line, no count threshold | None | 1 frame received passes the test |
| **fastmetadata** | Layer 2 only | None | Payload bytes never compared |
| **st41 (fmd)** | Layer 2 only | None | Same |

---

## 3. Concrete weaknesses

1. **Layer 2 alone is the silent-corruption escape hatch.** A test that
   drops 200 of 4500 frames still hits 95 % and prints `OK`.
2. **Frame alignment in `check_st20p_integrity` is broken** — assumes RX
   starts at source frame 0. The `VideoIntegritor` hash-shift logic should
   be ported into the simple checker (~10 lines).
3. **st22p has no integrity check at all.** Lossy codecs need SSIM/PSNR,
   not MD5.
4. **Ancillary / fastmetadata payload is never byte-checked.** The C app
   counts frames; nothing compares the ANC payload TX sent vs RX wrote.
5. **st40p RX check is weakest** (`success_token = "frames received"` with
   no count threshold) — a single frame received passes the test.
6. **Error-line scanning is missing.** Output is scraped only for `OK`.
   Lines like `RX_VIDEO_SESSION...packet drop`, `pkts dropped due to seq`,
   `cni: ipv4 invalid`, `RTCP nack`, `crc_err` are ignored.
7. **No RTP-level check is wired up.** The codebase has
   `compliance/PcapComplianceClient` and `netsniff` capture machinery, but
   most refactored tests don't enable it.

---

## 4. Improvements ranked by ROI

| # | Improvement | Effort | Catches |
|---|---|---|---|
| 1 | Add "no error tokens in stdout" check in `execute_test` (regex over `err\(`, `pkts dropped`, `RTCP nack`, `crc_err`, `seq.*err`) | tiny | broad regression detection across **all** tests, instantly |
| 2 | Port hash-shift alignment into `check_st20p_integrity` | tiny | fixes silent false-fails in integrity tests |
| 3 | Tighten C `expect_fps` tolerance from 5 % to 1 % (or parse `stat_frame_total_received` and assert `>= test_time * fps - small_delta`) | tiny | catches bulk packet loss in every test using Layer 2 |
| 4 | Harden st40p check: require `frames_received >= test_time * fps * 0.95` | tiny | prevents 1-frame-passes-test scenario |
| 5 | Add per-test pcap capture + RTP seq-gap analyzer (pieces already exist in `compliance/`) | medium | proves zero loss without comparing payloads |
| 6 | Add SSIM-based integrity for st22p tests (`skimage.metrics.structural_similarity`, threshold ≥ 0.99) | medium | makes lossy-codec tests actually meaningful |
| 7 | Side-file payload compare for anc / fastmetadata: TX writes payload, RX writes parsed ANC, MD5-compare | medium | first real correctness check for these formats |

---

## 5. Bottom line

Layer 1 + Layer 2 together prove **"the program ran and produced approximately
the right number of frames."** They do **not** prove "the bytes you sent are
the bytes you received."

Only the integrity-runner tests do that today, and only for raw video/audio.
Items #1, #2, #3, #4 above are <1 day of work each and would dramatically
raise confidence in the existing refactored test suite without adding any
new test files.
