# R2 — Citation re-pin and Markdown hygiene log

| | |
|---|---|
| Date | 2026-09-29 |
| Baseline | `main` @ `545a266a` (every lookup used `git show 545a266a:<path>`; the working tree was not read for evidence) |
| Scope | top-level documents: `README.md`, `00-summary.md`, `01`–`14`, `OPEN-QUESTIONS.md`, `DECISIONS.md`, `side-findings.md`. `research/` and `reviews/` not touched (except this file) |
| Input | [C4 §5 and §6](C4-consistency-audit.md) |
| Method | every backticked `path:N[-M]` and `:N` continuation (226 spans) resolved with `git ls-tree -r --name-only 545a266a`, and the cited lines read at `545a266a` and compared with the claim around the citation. Out of scope and left alone: DPDK `pci_common.c:478-493` (1 span) and research-note references |

Line numbers in the "Location" column refer to the documents after this pass.

## 1. Citations changed

| # | Location | Old | New | Reason |
|---|---|---|---|---|
| 1 | `03-object-model-and-lifecycle.md:396` | `dev/mt_dev.c:324,495-498` | `dev/mt_dev.c:324,499-502` | the re-init reject (`if (eal_initted) … return -EIO`) is at 499-502; 495-498 is `argv` `--` handling |
| 2 | `06-timing-pacing-and-sync.md:679` (E1) | `st_tx_video_session.c:762`, `:1774` | `st_tx_video_session.c:692`, `:762` | the row names `tv_sync_pacing` (692) and `tv_update_rtp_time_stamp` (762); 1774 is `tv_pacing_required_tai` |
| 3 | `side-findings.md:25` (SF-07) | `lib/src/mt_main.c:869-870` | `lib/src/mt_main.c:864-865` | working-tree line; "only map for MTL_PORT_P" + `rte_dev_dma_map` at 864-865 (C4 §5.1 #29) |
| 4 | `side-findings.md:29` (SF-11) | `st_tx_video_session.c:2449-2455` | `st_tx_video_session.c:2468-2474` | working-tree lines; invalid codestream size → `tv_notify_frame_done` at 2468-2474 (C4 #31) |
| 5 | `side-findings.md:37` (SF-19) | `mt_main.c:534/542` | `mt_main.c:534` | 534 is the only write of `instance_in_reset` at HEAD; 542 was the working-tree line of the same statement |
| 6 | `side-findings.md:48` (SF-30) | `mt_main.c:416` | `mt_main.c:404` | working-tree line; `port_params[i].flags \|= …` at 404 (C4 #33) |
| 7 | `side-findings.md:49` (SF-31) | `mt_main.c:707-763` | `mt_main.c:700-758` | working-tree lines; `mtl_get_lcore` 700 … `mtl_abort` ends 758 (C4 #34) |
| 8 | `side-findings.md:63` (SP-01) | `st_tx_video_session.c:3734`, `:3838-3845`; `st_tx_audio_session.c:2536` | `st_tx_video_session.c:3803`, `:3907-3914`; `st_tx_audio_session.c:2538` | working-tree lines; re-lock in `tv_mgr_detach` at 3803, `tv_mgr_uinit` loop at 3907-3914, audio `mgr_detach` re-lock at 2538 (C4 #30) |
| 9 | `side-findings.md:64` (SP-02) | `st_rx_video_session.c:3050-3053` | `st_rx_video_session.c:3053-3056` | working-tree lines; `DATA_PATH_ONLY` → `mt_rxq_get(impl, port, NULL)` at 3053-3056 |
| 10 | `side-findings.md:66` (SP-04) | `st_tx_video_session.c:4731` | `st_tx_video_session.c:4802` | working-tree line; `st20_tx_free` is at 4786 and reads `s_impl->sch` at 4802 |
| 11 | `side-findings.md:87` (DD-05) | `doc/design.md:384` | `doc/design.md:390` | `doc/design.md` is edited in the working tree; `ST40P_TX_FLAG_EXT_FRAME` sentence at 390 (C4 #35) |
| 12 | `side-findings.md:90` (DD-08) | `doc/design.md:604` | `doc/design.md:618` | "RTP passthrough mode is the only supported" at 618 (C4 #35) |
| 13 | `side-findings.md:92` (DD-10) | `mt_util.c:940` | `mt_util.c:962` | working-tree line; `dpdk_afxdp_port_prefix = "dpdk_af_xdp:"` at 962 |

C4 §5.1 rows 29–32 and 36 were already applied before this pass: `05` cites `mt_main.c:864-865` and
`st_rx_video_session.c:4287-4291`, `03` cites `st_tx_video_session.c:3798-3914`, and DD-02 cites
`dev/mt_dev.c:344` with a KB section instead of a KB line. Row 21 (`mt_ptp.c:1062-1067`, SF-21) was left
as it is: at HEAD, 1062-1067 is the `if/else` that installs `ptp_from_eth`.

Checked and left as they are, although the citation is loose by one line or covers one line more than
needed. Each still contains the cited code:

| Location | Citation | Note |
|---|---|---|
| `04:62` | `dev/mt_af_xdp.c:522-526` | `xdp_tx_wakeup` starts at 523, `send()` at 525 |
| `08:183` | `st_video_transmitter.c:38` | the hang check is at 38, the `err()` at 39 |
| `side-findings.md:21` (SF-03) | `st_header.h:154-161` | the union is 154-162 |
| `side-findings.md:47` (SF-29) | `mt_main.c:1100-1127` | `mtl_ptp_read_time` ends at 1123 |
| `side-findings.md:100` (DD-13f) | `st20_api.h:576-577` | the wrong doc comment is at 577, the field at 578 |

## 2. Citations unverified

None. Every in-scope citation was either found to contain the claimed code at `545a266a` or re-pinned
(§1). No `**[unverified cite]**` marker was added.

## 3. Claims to review (content, not line numbers)

These are not moved lines. The text around the citation, or a related uncited sentence, does not match
HEAD. Wording was not changed.

| Location | Claim | At `545a266a` |
|---|---|---|
| `side-findings.md:98` (DD-13d) | `ST30P_RX_FLAG_DATA_PATH_ONLY` "points to a nonexistent `st30p_rx_get_queue_meta`" | `st30_pipeline_api.h:243` points to `st30_rx_get_queue_meta`, which exists (`st30_api.h:926`) but takes an `st30_rx_handle`. What does not exist is `st30p_rx_get_queue_meta`. R02 §4 #33 states this correctly |
| `05-memory-and-buffers.md:353` (M6) | "Validate `buf_len` and IOVA for RX dedicated ext frames" (`st_rx_video_session.c:439-450`) | IOVA is already rejected when `0` or `MTL_BAD_IOVA` (446-450); only `buf_len` is unchecked. SF-17 says it correctly |
| `09-media-modes-and-backends.md:226`; `06-timing-pacing-and-sync.md:625` | "HW RX timestamps need a PF on E810" / HW = "PF with `MTL_FLAG_ENABLE_HW_TIMESTAMP`" | HEAD enables RX timestamps on any port that offers the offload¹, and `99b96c16` adds an iavf-VF workaround for them², so VFs are expected to use them. Needs confirmation |

¹ `dev/mt_dev.c:2422-2440` (`rx_offload_capa & RTE_ETH_RX_OFFLOAD_TIMESTAMP`).

² `dev/mt_dev.c:1020-1028`; `99b96c16` is an ancestor of HEAD. The working tree removes the workaround,
which may be where the PF-only reading came from.

## 4. Markdown hygiene fixes

Tooling: `markdownlint`, `markdownlint-cli2` and `mdl` are not on `PATH`. The cached
`npx markdownlint-cli2` (and `markdownlint-cli` 0.49) fail on Node 18 (regular expression `v` flag). The cached
`markdownlint-cli` 0.43.0 (`~/.npm/_npx/28c848719c97653c`) runs, and was used with
`.github/linters/.markdown-lint.yml`. Before this pass it reported 15 × MD013. After the pass it reports
0 findings on the top-level documents. MD013 skips lines whose overflow has no whitespace, so 3 rows
in `00-summary.md` over 400 characters were found only by the scripted character count. Scripted checks
also covered table cell counts (unescaped `|`), nested-list indentation under ordered items, heading
punctuation, relative links and `#q-…` anchors.

### 4.1 Lines over 400 characters (18 → 0)

Each fix moved part of a table cell, unchanged in content, into a numbered footnote directly under the
same table (the existing `04` style, `¹ …`).

| File | Row (before → after) | Length | Moved into the footnote |
|---|---|---|---|
| `00-summary.md` | 102 → 102, fn ¹ | 492 → < 400 | C1 finding's parenthetical (st20p frees before callback; …) |
| `00-summary.md` | 103 → 103, fn ² | 429 → < 400 | "grid table errors; missing ANC/FMD keep-alive; ST22 not CBR" |
| `00-summary.md` | 104 → 104, fn ³ | 480 → < 400 | tail of the C3 change list |
| `04-threading-and-execution.md` | 36 → 36, fn ² | 512 → < 400 | "every other library busy loop (RX packet lcore, TAP lcore)" and the tail of "May run" |
| `04-threading-and-execution.md` | 72 → 74, fn ³ | 536 → < 400 | DP examples after `mtl_tx_submit` |
| `04-threading-and-execution.md` | 74 → 76, fn ⁴ | 431 → < 400 | `MTL_TIMEOUT_INFINITE` / 0 semantics |
| `04-threading-and-execution.md` | 203 → 209, fn ⁵ | 463 → < 400 | `mt_txq_done_cleanup`, rate limit, dedicated queues (Q-CMP-7) |
| `04-threading-and-execution.md` | 328 → 336, fn ⁶ ⁷ | 534 → < 400 | citations `dev/mt_dev.c:1603-1608`, `mt_ptp.c:393-402`; TAI formula and Q-THR-7a |
| `06-timing-pacing-and-sync.md` | 232 → 232, fn ¹ | 407 → < 400 | `struct mtl_anc_packet { … }` field list |
| `06-timing-pacing-and-sync.md` | 244 → 246, fn ² | 403 → < 400 | "(allowed as production intent within ±TFRAME, ST 2110-10 §7.6.3)" |
| `06-timing-pacing-and-sync.md` | 290 → 294, fn ³ | 407 → < 400 | JT-NM window values |
| `06-timing-pacing-and-sync.md` | 303 → 309, fn ⁴ | 432 → < 400 | citations `st_tx_video_session.c:2468-2500`, `:750-757` |
| `06-timing-pacing-and-sync.md` | 305 → 311, fn ⁵ ⁶ | 494 → < 400 | TEPO/SDI raster sentence; citation `st_tx_ancillary_session.c:1108` |
| `08-observability.md` | 76 → 76, fn ¹ | 409 → < 400 | "(completion mode)" of `units_suppressed` |
| `11-abi-compatibility-and-migration.md` | 125 → 125, fn ¹ | 446 → < 400 | why TX still copies in v1 (Q-MEM-10) |
| `12-familiarity-libfabric-and-rivermax.md` | 31 → 31, fn ¹ | 443 → < 400 | `mtl_port_get_caps` / `mtl_capability_request` |
| `13-guarantees-and-tests.md` | 86 → 86, fn ¹ | 423 → < 400 | 2^32 wrap durations |
| `13-guarantees-and-tests.md` | 127 → 129, fn ² | 429 → < 400 | "no `malloc`/`rte_malloc*`/pool or ring create" |

### 4.2 Other checks (no change needed)

| Check | Result |
|---|---|
| Table rows with a cell count different from the header | 0 (the SF-30 `\|=` is already escaped) |
| Nested lists: MD007 (indent 2), and sub-items under `N.` at the content column | 0 findings |
| Headings ending in `. , ; : !` (MD026) | 0 |
| Relative links (`](file.md)`, `](research/…)`, `](reviews/…)`) | 331 checked, 0 broken |
| `#q-…` anchors into `OPEN-QUESTIONS.md` | 93 checked, 0 missing |
| Doc-to-doc `NN:line` references that the footnote insertions could shift | none in the top-level documents |
