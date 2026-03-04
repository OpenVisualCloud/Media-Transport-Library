/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: Consumer / display stub implementation
 *
 * Architecture:
 *   The consumer loop runs as two phases per iteration:
 *     Phase 1 (HOT): Drain ALL pending RDMA events via ReadGrainNonBlocking.
 *                     Each event is enqueued for Phase 2 processing.
 *                     This keeps the RDMA CQ drained and prevents QP errors.
 *     Phase 2 (WARM): Read ONE pending grain for thumbnail / metrics.
 *                     Only one per loop iteration to keep CQ drain responsive.
 */

#include "poc_consumer.h"
#include "poc_latency.h"
#include "poc_stats.h"
#include "poc_thumbnail.h"
#ifdef POC_14
#include "thumbnail_14.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <immintrin.h>

/* ── Pending-read ring buffer ── */
#define READ_RING_SIZE 64   /* must be power of 2 */
typedef struct {
    uint16_t  entry_index;
    uint16_t  slice_index;
    uint64_t  grain_index;
    uint32_t  grain_flags;
} pending_read_t;

void poc_consumer_run(poc_consumer_ctx_t *ctx)
{
    poc_mxl_sink_t       *sink  = ctx->sink;
    poc_fabrics_target_t *ft    = ctx->ft;
    poc_stats_t          *stats = ctx->stats;

    uint64_t frame_count = 0;

    /* Read ring: produced by event drain, consumed by Phase 2 */
    pending_read_t rd_ring[READ_RING_SIZE];
    uint32_t rd_head = 0;  /* next write position */
    uint32_t rd_tail = 0;  /* next read position */

    /* Thumbnail generation — every frame for real-time MJPEG preview */
    bool do_thumb = (ctx->thumb_dir != NULL);
    uint64_t thumb_interval_frames = 1;   /* every frame → 30fps preview */
    uint64_t thumb_frame_counter = 0;
    if (do_thumb) {
        if (poc_thumbnail_init(ctx->thumb_dir, ctx->src_width, ctx->src_height) != 0) {
            fprintf(stderr, "[CONSUMER] Thumbnail init failed, disabling\n");
            do_thumb = false;
        }
    }

    printf("[CONSUMER] Starting consumer loop (thumbnail=%s)\n",
           do_thumb ? "ON" : "OFF");

    uint32_t idle_spins = 0;   /* 3-tier back-off counter */

    /* ── CQ batch instrumentation ── */
    uint64_t last_event_ns     = 0;    /* timestamp of last CQ event */
    uint64_t batch_report_ns   = 0;    /* next batch stats report time */
    uint64_t batch_max_gap_ns  = 0;    /* max inter-event gap in period */
    uint32_t batch_max_size    = 0;    /* max events per Phase-1 drain */
    uint32_t batch_max_idle    = 0;    /* max idle_spins at event arrival */
    uint64_t batch_total_events = 0;   /* total events in period */
    uint32_t batch_total_rounds = 0;   /* total drain rounds in period */
    const uint64_t BATCH_REPORT_INTERVAL_NS = 10000000000ULL; /* 10s */

    while (*ctx->running) {
        /* ════════════════════════════════════════════════
         * PHASE 1: Drain ALL pending RDMA events (HOT PATH)
         * Keep calling ReadGrainNonBlocking until no more events.
         * This prevents CQ overflow which causes QP errors.
         * ════════════════════════════════════════════════ */
        int events_this_round = 0;
        for (;;) {
            uint16_t entry_index = 0;
            uint16_t slice_index = 0;

            mxlStatus st = mxlFabricsTargetReadGrainNonBlocking(ft->target,
                                                                  &entry_index,
                                                                  &slice_index);
            if (st == MXL_ERR_NOT_READY)
                break;

            if (st != MXL_STATUS_OK) {
                fprintf(stderr, "[CONSUMER] TargetReadGrainNonBlocking error: %d\n", st);
                break;
            }

            events_this_round++;
            uint64_t t_event_ns = poc_now_realtime_ns();

            /* ── Track inter-event gap ── */
            if (last_event_ns > 0) {
                uint64_t gap_ns = t_event_ns - last_event_ns;
                if (gap_ns > batch_max_gap_ns)
                    batch_max_gap_ns = gap_ns;
                /* Immediate alert on large gap (>25ms = 1.5 frames) */
                if (gap_ns > 25000000ULL) {
                    fprintf(stderr,
                        "[BATCH-ALERT] gap=%.1f ms  idle_spins=%u  "
                        "events_so_far=%d\n",
                        gap_ns / 1e6, idle_spins, events_this_round);
                }
            }
            last_event_ns = t_event_ns;
            atomic_fetch_add(&stats->target_events, 1);

            /* Read grain header */
            mxlGrainInfo grain_info;
            memset(&grain_info, 0, sizeof(grain_info));
            st = mxlFlowWriterGetGrainInfo(sink->writer, entry_index, &grain_info);
            if (st != MXL_STATUS_OK) {
                fprintf(stderr, "[CONSUMER] GetGrainInfo(slot=%u) failed: %d\n",
                        entry_index, st);
                continue;
            }

            /* Commit the grain so local readers can see it */
            uint8_t *recv_payload = NULL;
            {
                mxlGrainInfo open_info;
                st = mxlFlowWriterOpenGrain(sink->writer, grain_info.index,
                                             &open_info, &recv_payload);
                if (st == MXL_STATUS_OK) {
                    open_info.validSlices = slice_index;
                    open_info.flags = grain_info.flags;
                    mxlFlowWriterCommitGrain(sink->writer, &open_info);
                }
            }

            /* lat_consume: RDMA event → grain committed */
            {
                uint64_t t_commit_ns = poc_now_realtime_ns();
                poc_latency_record(&stats->lat_consume,
                                   poc_ns_to_us(t_commit_ns - t_event_ns));
            }

            /* lat_e2e: sender enqueue → receiver event (PTP-synced) */
            if (recv_payload) {
                uint64_t sender_ts = poc_frame_ts_read(recv_payload);
                if (sender_ts) {
                    if (t_event_ns > sender_ts) {
                        uint64_t e2e_us = poc_ns_to_us(t_event_ns - sender_ts);
                        poc_latency_record(&stats->lat_e2e, e2e_us);

                        /* ── E2E anomaly diagnostics ── */
                        {
                            static uint64_t prev_sender_ts = 0;
                            static uint64_t prev_event_ns  = 0;
                            static uint64_t diag_count     = 0;
                            static bool     anomaly_seen   = false;

                            if (prev_sender_ts > 0) {
                                int64_t ts_delta_us =
                                    (int64_t)(sender_ts - prev_sender_ts) / 1000;
                                int64_t ev_delta_us =
                                    (int64_t)(t_event_ns - prev_event_ns) / 1000;

                                /* First 30 anomalous frames: dump full detail */
                                if (e2e_us > 5000 && diag_count < 30) {
                                    fprintf(stderr,
                                        "[E2E-DIAG] e2e=%luus entry=%u "
                                        "ts_delta=%ldus ev_delta=%ldus "
                                        "sender_ts=%lu event_ns=%lu\n",
                                        e2e_us, entry_index,
                                        ts_delta_us, ev_delta_us,
                                        sender_ts, t_event_ns);
                                    diag_count++;
                                }

                                /* Detect non-monotonic or stale sender_ts */
                                if (!anomaly_seen &&
                                    (ts_delta_us < 0 ||
                                     ts_delta_us > 25000 ||
                                     (ts_delta_us < 10000 && ts_delta_us != 0))) {
                                    fprintf(stderr,
                                        "[E2E-TS-ANOMALY] ts_delta=%ldus "
                                        "ev_delta=%ldus entry=%u e2e=%luus "
                                        "raw[0..31]=",
                                        ts_delta_us, ev_delta_us,
                                        entry_index, e2e_us);
                                    /* Hex dump first 32 bytes of payload */
                                    for (int b = 0; b < 32 && b < (int)grain_info.grainSize; b++)
                                        fprintf(stderr, "%02x", recv_payload[b]);
                                    fprintf(stderr, "\n");
                                    anomaly_seen = true;
                                }
                            }
                            prev_sender_ts = sender_ts;
                            prev_event_ns  = t_event_ns;
                        }
                    } else {
                        /* Clock skew: receiver behind sender — PTP not synced? */
                        static uint64_t skew_count = 0;
                        if (++skew_count == 1 || (skew_count % 1800) == 0) {
                            fprintf(stderr, "[E2E] Clock skew (n=%lu): "
                                    "sender=%lu rx=%lu diff=%ld ms\n",
                                    skew_count, sender_ts, t_event_ns,
                                    (int64_t)(t_event_ns - sender_ts) / 1000000);
                        }
                    }
                } else {
                    /* Magic mismatch — timestamp not in payload */
                    static uint64_t magic_miss = 0;
                    if (++magic_miss == 1 || (magic_miss % 1800) == 0) {
                        uint64_t raw = 0;
                        memcpy(&raw, recv_payload, sizeof(raw));
                        fprintf(stderr, "[E2E] No magic (n=%lu): "
                                "got 0x%016lx expect 0x%016lx\n",
                                magic_miss, raw, (uint64_t)POC_TS_MAGIC);
                    }
                }
            }

            atomic_fetch_add(&stats->consumer_frames, 1);

            /* Enqueue for Phase 2 (thumbnail) if full frame received */
            if (sink->reader && slice_index >= grain_info.totalSlices)
            {
                uint32_t next = (rd_head + 1) & (READ_RING_SIZE - 1);
                if (next != rd_tail) {
                    rd_ring[rd_head].entry_index = entry_index;
                    rd_ring[rd_head].slice_index = slice_index;
                    rd_ring[rd_head].grain_index = grain_info.index;
                    rd_ring[rd_head].grain_flags = grain_info.flags;
                    rd_head = next;
                }
            }
        }

        /* ════════════════════════════════════════════════
         * PHASE 2: Read ONE pending grain (if any)
         * Only one per loop iteration to keep CQ drain responsive.
         * ════════════════════════════════════════════════ */
        if (rd_tail != rd_head) {
            pending_read_t *pr = &rd_ring[rd_tail];
            rd_tail = (rd_tail + 1) & (READ_RING_SIZE - 1);

            uint8_t *payload = NULL;
            mxlGrainInfo read_info;
            mxlStatus st = mxlFlowReaderGetGrainNonBlocking(sink->reader,
                                                              pr->grain_index,
                                                              &read_info, &payload);
            if (st == MXL_STATUS_OK && payload) {
                frame_count++;

                if ((frame_count % 300) == 1) {
                    printf("[CONSUMER] Frame #%lu  slot=%u  index=%lu  "
                           "slices=%u/%u  size=%u\n",
                           frame_count, (unsigned)pr->entry_index,
                           (unsigned long)pr->grain_index,
                           pr->slice_index, read_info.totalSlices,
                           read_info.grainSize);
                }

                /* Generate JPEG thumbnail every frame for live preview */
                if (do_thumb && (++thumb_frame_counter % thumb_interval_frames) == 0) {
                    poc_thumbnail_write(payload, read_info.grainSize);
                }
#ifdef POC_14
                /* Multi-instance thumbnail for poc_14 */
                if (ctx->thumb16) {
                    poc14_thumb_write((poc14_thumb_ctx_t *)ctx->thumb16, payload, read_info.grainSize);
                }
#endif
            }
        }

        /* ── Batch instrumentation: track drain size and idle spins ── */
        if (events_this_round > 0) {
            batch_total_events += events_this_round;
            batch_total_rounds++;
            if ((uint32_t)events_this_round > batch_max_size)
                batch_max_size = (uint32_t)events_this_round;
            if (idle_spins > batch_max_idle)
                batch_max_idle = idle_spins;
        }

        /* ── Periodic batch report (every 10s) ── */
        {
            uint64_t now_ns = poc_now_realtime_ns();
            if (batch_report_ns == 0)
                batch_report_ns = now_ns + BATCH_REPORT_INTERVAL_NS;
            if (now_ns >= batch_report_ns) {
                double avg_batch = batch_total_rounds > 0
                    ? (double)batch_total_events / batch_total_rounds : 0;
                fprintf(stderr,
                    "[BATCH] events=%lu rounds=%u avg_batch=%.1f max_batch=%u "
                    "max_idle=%u max_gap_ms=%.1f\n",
                    (unsigned long)batch_total_events, batch_total_rounds,
                    avg_batch, batch_max_size, batch_max_idle,
                    batch_max_gap_ns / 1e6);
                batch_total_events = 0;
                batch_total_rounds = 0;
                batch_max_size = 0;
                batch_max_idle = 0;
                batch_max_gap_ns = 0;
                batch_report_ns = now_ns + BATCH_REPORT_INTERVAL_NS;
            }
        }

        /* If no events this round, adaptive back-off:
         *   Tier 1 (0-63):   _mm_pause — ~5 ns x86 hint
         *   Tier 2 (64-319): sched_yield — ~1-10 µs
         *   Tier 3 (320+):   usleep(10) — 10 µs sleep   */
        if (events_this_round == 0) {
            if (idle_spins < 64) {
                _mm_pause();
            } else if (idle_spins < 320) {
                sched_yield();
            } else {
                usleep(10);
            }
            idle_spins++;
        } else {
            idle_spins = 0;
        }
    }

    if (do_thumb)
        poc_thumbnail_cleanup();

    printf("[CONSUMER] Consumer loop ended after %lu frames\n", frame_count);
}
