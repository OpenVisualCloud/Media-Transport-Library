/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host A: MXL bridge worker implementation (zero-copy)
 *
 * Zero-copy data path:
 *   1. query_ext_frame (MTL lcore): assigns MXL grain payload buffer as DMA target
 *   2. NIC DMAs received ST 2110-20 packets directly into the grain payload
 *   3. notify_frame_ready (MTL lcore): enqueues frame entry with flow_index
 *   4. Bridge thread: opens grain (metadata only), commits, triggers RDMA
 *   5. NO memcpy in the hot path
 */

#include <errno.h>
#include <immintrin.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "poc_json_utils.h"
#include "poc_latency.h"
#include "poc_mxl_bridge.h"
#include "poc_stats.h"
#include "poc_v210_utils.h"

/* Maximum RDMA pipeline depth (ring array size).
 * Effective depth may be lower — see bridge->pipeline_depth. */
#define POC_PIPELINE_DEPTH 10

/* ── Helpers ── */
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ════════════════════════════════════════════════════════════
 * query_ext_frame — called from MTL lcore when a new frame
 * begins arriving.  We assign the next MXL grain's payload
 * buffer so the NIC DMAs directly into it.
 *
 * Scans all grain slots round-robin for a free one.  This
 * avoids livelock if a specific slot is temporarily stuck.
 * ════════════════════════════════════════════════════════════ */
int poc_mxl_bridge_query_ext_frame(void* priv, struct st20_ext_frame* ext_frame,
                                   struct st20_rx_frame_meta* meta) {
  poc_mxl_bridge_t* bridge = (poc_mxl_bridge_t*)priv;
  (void)meta;

  /* Scan only DMA-mapped slots (0..pipeline_depth-1).
   * Grains beyond pipeline_depth are NOT DMA-mapped and have no valid
   * iova, so the NIC cannot DMA into them. */
  uint32_t dma_count = bridge->pipeline_depth;
  uint32_t start = atomic_load(&bridge->next_slot) % dma_count;
  for (uint32_t i = 0; i < dma_count; i++) {
    uint32_t slot = (start + i) % dma_count;
    poc_grain_slot_t* gs = &bridge->grain_slots[slot];

    if (atomic_load_explicit(&gs->in_use, memory_order_acquire)) continue;

    /* Claim the slot */
    atomic_store_explicit(&gs->in_use, true, memory_order_release);
    /* Assign a monotonically increasing grain index such that
     * grain_index % grain_count == slot.  MXL internally maps
     * grain_index to a buffer via (grain_index % grainCount), so
     * this alignment guarantees the NIC DMA target buffer matches
     * the buffer MXL will read during CommitGrain/TransferGrain.
     *
     * Without this, slots 0..pipeline_depth-1 would get arbitrary
     * flow_indices that map to non-DMA-mapped grain buffers (>= 8),
     * causing RDMA of stale data and permanent slot lockup. */
    {
      uint64_t min_gi = atomic_load(&bridge->next_grain_index);
      uint64_t r = min_gi % bridge->grain_count;
      uint64_t gi;
      if (r <= slot) {
        gi = min_gi + (slot - r);
      } else {
        gi = min_gi + (bridge->grain_count - r + slot);
      }
      atomic_store(&bridge->next_grain_index, gi + 1);
      gs->flow_index = gi;

      /* Guard against MXL ImmData uint16_t truncation:
       * ImmDataGrain encodes ringBufIdx = uint16_t(grainIndex).
       * If grainIndex >= 65536, the lower 16 bits wrap and the
       * receiver misidentifies the RDMA region, reading stale data.
       * Reset the counter well before the wrap boundary.
       * 65534 = 7 * 9362 = largest multiple-of-grainCount ≤ 65535. */
      if (atomic_load(&bridge->next_grain_index) >= 65534) {
        atomic_store(&bridge->next_grain_index, bridge->grain_count);
      }
    }

    /* Advance round-robin start for next call */
    atomic_store(&bridge->next_slot, (slot + 1) % dma_count);

    /* Provide the grain payload buffer to MTL */
    ext_frame->buf_addr = gs->addr;
    ext_frame->buf_iova = gs->iova;
    ext_frame->buf_len = gs->buf_len;
    ext_frame->opaque = gs; /* passed back via meta->opaque in notify */

    return 0;
  }

  /* All slots busy */
  return -EBUSY;
}

/* ════════════════════════════════════════════════════════════
 * Phase 1 init: MXL writer + grain address pre-computation
 *               + DMA mapping of grain buffers
 * ════════════════════════════════════════════════════════════ */
int poc_mxl_bridge_init(poc_mxl_bridge_t* bridge, const poc_config_t* cfg, mtl_handle mtl,
                        poc_frame_queue_t* queue, poc_stats_t* stats,
                        volatile bool* running) {
  memset(bridge, 0, sizeof(*bridge));
  bridge->mtl = mtl;
  bridge->queue = queue;
  bridge->stats = stats;
  bridge->running = running;
  bridge->frame_counter = 0;
  atomic_init(&bridge->next_slot, 0);
  atomic_init(&bridge->next_grain_index, 0);

  mxlStatus st;
  char* flow_json = NULL;

  /* ── Load flow JSON ── */
  size_t json_len = 0;
  flow_json = poc_load_file(cfg->flow_json_path, &json_len);
  if (!flow_json) {
    fprintf(stderr, "[BRIDGE] Failed to load flow JSON: %s\n", cfg->flow_json_path);
    return -1;
  }

  /* ── Create MXL instance ── */
  bridge->mxl_instance = mxlCreateInstance(cfg->mxl_domain, "");
  if (!bridge->mxl_instance) {
    fprintf(stderr, "[BRIDGE] mxlCreateInstance failed\n");
    free(flow_json);
    return -1;
  }

  /* ── Create FlowWriter ── */
  bool created = false;
  st = mxlCreateFlowWriter(bridge->mxl_instance, flow_json, NULL, &bridge->writer,
                           &bridge->config_info, &created);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlCreateFlowWriter failed: %d\n", st);
    free(flow_json);
    goto err;
  }

  bridge->grain_count = bridge->config_info.discrete.grainCount;

  /* Cap pipeline depth to leave headroom for MTL frame reception.
   * With N grains, at most N-3 can be in-flight RDMA transfers,
   * leaving ≥3 free slots for: 1 receiving + 1 queued + 1 margin. */
  if (bridge->grain_count >= 4) {
    uint32_t max_pipe = bridge->grain_count - 3;
    bridge->pipeline_depth =
        (max_pipe < POC_PIPELINE_DEPTH) ? max_pipe : POC_PIPELINE_DEPTH;
  } else {
    bridge->pipeline_depth = 1;
  }

  printf("[BRIDGE] FlowWriter ready (grainCount=%u, pipelineDepth=%u, sliceSize[0]=%u)\n",
         bridge->grain_count, bridge->pipeline_depth,
         bridge->config_info.discrete.sliceSizes[0]);

  free(flow_json);

  if (bridge->grain_count > POC_MAX_GRAIN_SLOTS) {
    fprintf(stderr, "[BRIDGE] grain_count %u exceeds max %d\n", bridge->grain_count,
            POC_MAX_GRAIN_SLOTS);
    goto err;
  }

  /* ── Pre-compute grain payload addresses ──
   * Open each grain slot, remember its payload address, then commit
   * with validSlices=0 (empty) to release the writer state. */
  printf("[BRIDGE] Pre-computing grain payload addresses...\n");
  for (uint32_t i = 0; i < bridge->grain_count; i++) {
    mxlGrainInfo ginfo;
    uint8_t* payload = NULL;

    st = mxlFlowWriterOpenGrain(bridge->writer, i, &ginfo, &payload);
    if (st != MXL_STATUS_OK) {
      fprintf(stderr, "[BRIDGE] OpenGrain(%u) for addr discovery failed: %d\n", i, st);
      goto err;
    }

    bridge->grain_slots[i].addr = payload;
    bridge->grain_slots[i].buf_len = ginfo.grainSize;
    bridge->grain_size = ginfo.grainSize;
    atomic_init(&bridge->grain_slots[i].in_use, false);

    /* Empty commit to release writer state */
    ginfo.validSlices = 0;
    st = mxlFlowWriterCommitGrain(bridge->writer, &ginfo);
    if (st != MXL_STATUS_OK) {
      fprintf(stderr, "[BRIDGE] CommitGrain(%u) empty failed: %d\n", i, st);
      goto err;
    }

    printf("[BRIDGE]   grain[%u]: addr=%p  size=%u\n", i, (void*)payload,
           ginfo.grainSize);
  }

  /* ── DMA-map grains used for NIC zero-copy — COALESCED ──
   * Instead of mapping each grain individually (which exhausts DPDK's
   * limited memseg list at ~116 entries for 14+ streams × 9 grains),
   * find the min/max addresses across pipeline_depth grains and map
   * a single contiguous region.  MXL allocates grains from a
   * contiguous pool, so they are adjacent in virtual memory. */
  {
    size_t pg_sz = mtl_page_size(mtl);
    uint32_t dma_count = bridge->pipeline_depth;

    /* Find min/max addresses across grains 0..dma_count-1 */
    uint8_t* region_lo = bridge->grain_slots[0].addr;
    uint8_t* region_hi = bridge->grain_slots[0].addr + bridge->grain_size;

    for (uint32_t i = 1; i < dma_count; i++) {
      uint8_t* a = bridge->grain_slots[i].addr;
      if (a < region_lo) region_lo = a;
      uint8_t* e = a + bridge->grain_size;
      if (e > region_hi) region_hi = e;
    }

    /* Page-align: round down start, round up end */
    uint8_t* base = (uint8_t*)((uintptr_t)region_lo & ~(pg_sz - 1));
    size_t total = mtl_size_page_align((size_t)(region_hi - base), pg_sz);

    printf("[BRIDGE] Coalesced DMA mapping: base=%p  size=%zu  (grains=%u)\n",
           (void*)base, total, dma_count);

    mtl_iova_t iova = mtl_dma_map(mtl, base, total);
    if (iova == MTL_BAD_IOVA) {
      fprintf(stderr, "[BRIDGE] Coalesced mtl_dma_map failed\n");
      goto err;
    }

    /* Store coalesced mapping info in the bridge */
    bridge->dma_region_base = base;
    bridge->dma_region_size = total;
    bridge->dma_region_iova = iova;
    bridge->dma_region_mapped = true;

    /* Compute each grain's IOVA relative to the single mapping */
    for (uint32_t i = 0; i < dma_count; i++) {
      poc_grain_slot_t* gs = &bridge->grain_slots[i];
      gs->iova = iova + (gs->addr - base);
      gs->dma_mapped = true; /* flag for ext_frame use */

      printf("[BRIDGE]   grain[%u]: addr=%p  iova=0x%lx\n", i, (void*)gs->addr,
             (unsigned long)gs->iova);
    }
  }

  printf(
      "[BRIDGE] Zero-copy grain pool ready (grain_count=%u, dma_mapped=%u, "
      "grain_size=%zu)\n",
      bridge->grain_count, bridge->pipeline_depth, bridge->grain_size);

  /* Init used indices 0..grainCount-1 for address discovery;
   * start the monotonic counter at grainCount so MXL sees fresh indices */
  atomic_store(&bridge->next_grain_index, bridge->grain_count);

  /* ── Create Fabrics instance ── */
  st = mxlFabricsCreateInstance(bridge->mxl_instance, &bridge->fab_instance);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlFabricsCreateInstance failed: %d\n", st);
    goto err;
  }

  /* ── Create Initiator ── */
  st = mxlFabricsCreateInitiator(bridge->fab_instance, &bridge->initiator);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlFabricsCreateInitiator failed: %d\n", st);
    goto err;
  }

  /* ── Get regions from FlowWriter (creator mmap → RDMA-registrable) ── */
  st = mxlFabricsRegionsForFlowWriter(bridge->writer, &bridge->regions);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlFabricsRegionsForFlowWriter failed: %d\n", st);
    goto err;
  }

  /* ── Resolve provider ── */
  mxlFabricsProvider provider = MXL_FABRICS_PROVIDER_TCP;
  if (cfg->fabrics_provider[0]) {
    mxlFabricsProviderFromString(cfg->fabrics_provider, &provider);
  }

  /* ── Setup initiator ── */
  mxlFabricsInitiatorConfig init_cfg = {
      .endpointAddress =
          {
              .node = cfg->fabrics_local_ip,
              .service = cfg->fabrics_local_port,
          },
      .provider = provider,
      .regions = bridge->regions,
      .deviceSupport = false,
  };

  st = mxlFabricsInitiatorSetup(bridge->initiator, &init_cfg);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlFabricsInitiatorSetup failed: %d\n", st);
    goto err;
  }

  return 0;

err:
  poc_mxl_bridge_destroy(bridge);
  return -1;
}

/* ════════════════════════════════════════════════════════════
 * Phase 2: Connect fabrics to the remote target.
 * Called after st20_rx_create sets bridge->rx_handle.
 * ════════════════════════════════════════════════════════════ */
int poc_mxl_bridge_connect(poc_mxl_bridge_t* bridge, const poc_config_t* cfg) {
  mxlStatus st;

  /* ── Parse target info from Host B ── */
  if (!cfg->target_info_str[0]) {
    fprintf(stderr,
            "[BRIDGE] No target_info_str provided. "
            "Run receiver first and copy targetInfo here.\n");
    return -1;
  }

  st = mxlFabricsTargetInfoFromString(cfg->target_info_str, &bridge->target_info);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlFabricsTargetInfoFromString failed: %d\n", st);
    return -1;
  }

  /* ── Add target to initiator ── */
  st = mxlFabricsInitiatorAddTarget(bridge->initiator, bridge->target_info);
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] mxlFabricsInitiatorAddTarget failed: %d\n", st);
    return -1;
  }

  /* ── Drive progress until connected ── */
  printf("[BRIDGE] Connecting to remote target...\n");
  uint64_t deadline = now_ms() + 10000; /* 10s timeout */
  while (now_ms() < deadline) {
    st = mxlFabricsInitiatorMakeProgressNonBlocking(bridge->initiator);
    if (st == MXL_STATUS_OK) {
      printf("[BRIDGE] Connected to remote target!\n");
      break;
    }
    if (st != MXL_ERR_NOT_READY) {
      fprintf(stderr, "[BRIDGE] MakeProgress error: %d\n", st);
      return -1;
    }
    usleep(1000);
  }
  if (st != MXL_STATUS_OK) {
    fprintf(stderr, "[BRIDGE] Timed out connecting to target\n");
    return -1;
  }

  return 0;
}

/* ── Greedy pipeline in-flight tracking ── */

typedef struct {
  uint32_t slot;
  uint64_t grain_index;
  uint64_t t2_ns;      /* dequeue timestamp */
  uint64_t t2b_ns;     /* just before RDMA post */
  uint64_t enqueue_ns; /* original enqueue time (for e2e latency) */
  /* NOTE: frame_addr (MTL RX buffer) is no longer tracked here.
   * put_framebuff is now called immediately after dequeue to
   * prevent MTL slot exhaustion with small framebuff_cnt. */
  uint64_t queue_lat_us;
} poc_inflight_entry_t;

/* ── Helper: drain all in-flight entries, record latencies, release slots ── */
static inline void drain_inflight(poc_mxl_bridge_t* bridge, poc_inflight_entry_t* ring,
                                  int* ring_head, int* inflight) {
  uint64_t t3_ns = poc_now_realtime_ns();
  while (*inflight > 0) {
    poc_inflight_entry_t* e = &ring[*ring_head];
    uint64_t bridge_lat_us = poc_ns_to_us(e->t2b_ns - e->t2_ns);
    uint64_t rdma_lat_us = poc_ns_to_us(t3_ns - e->t2b_ns);
    uint64_t total_lat_us = poc_ns_to_us(t3_ns - e->enqueue_ns);

    poc_latency_record(&bridge->stats->lat_queue, e->queue_lat_us);
    poc_latency_record(&bridge->stats->lat_bridge, bridge_lat_us);
    poc_latency_record(&bridge->stats->lat_rdma, rdma_lat_us);
    poc_latency_record(&bridge->stats->lat_total, total_lat_us);
    atomic_fetch_add(&bridge->stats->fabrics_transfers, 1);

    /* NOTE: st20_rx_put_framebuff is now called immediately after
     * dequeue in the main loop, NOT here.  This decouples MTL
     * frame-slot lifetime from RDMA transfer lifetime, preventing
     * the slot exhaustion that causes "slot get frame fail" drops.
     * Only the grain in_use flag is released here (after RDMA). */
    atomic_store_explicit(&bridge->grain_slots[e->slot].in_use, false,
                          memory_order_release);

    *ring_head = (*ring_head + 1) % POC_PIPELINE_DEPTH;
    (*inflight)--;
  }
}

/* ── Helper: poll CQ; drain all in-flight if pending == 0 ──
 * Returns: 1 = drained, 0 = still pending, -1 = fatal error */
static inline int try_drain(poc_mxl_bridge_t* bridge, poc_inflight_entry_t* ring,
                            int* ring_head, int* inflight) {
  mxlStatus dst = mxlFabricsInitiatorMakeProgressNonBlocking(bridge->initiator);
  if (dst == MXL_STATUS_OK) {
    drain_inflight(bridge, ring, ring_head, inflight);
    return 1;
  }
  if (dst != MXL_ERR_NOT_READY) {
    fprintf(stderr, "[BRIDGE] MakeProgress error: %d — aborting\n", dst);
    *bridge->running = false;
    return -1;
  }
  return 0;
}

/* ════════════════════════════════════════════════════════════
 * Bridge worker main loop — GREEDY PIPELINE version
 *
 * Improvements over the previous batch-drain approach:
 *
 *  1. Deeper pipeline (8 vs 4): more RDMA/NIC-DMA overlap,
 *     fewer stalls when one transfer is slow.
 *
 *  2. Greedy fill: posts as many RDMA writes as the queue
 *     has frames ready for (up to PIPELINE_DEPTH) in a
 *     single iteration instead of exactly one per loop.
 *
 *  3. Interleaved progress polls: after each TransferGrain
 *     post, the CQ is polled.  When MakeProgress returns OK
 *     (_pending == 0), all in-flight slots are released
 *     immediately — even mid-fill.  This gives the earliest
 *     possible batch drain while still filling the pipe.
 *
 *  4. Non-blocking backpressure: when the pipeline is full,
 *     non-blocking spin-poll replaces MakeProgressBlocking.
 *     Eliminates the 1 ms blocking granularity and detects
 *     completion within one poll cycle.
 *
 * Note: MXL Fabrics API only signals "all pending done"
 * (MakeProgress returns OK when _pending == 0).  Per-transfer
 * completion isn't exposed.  The above optimisations minimise
 * the impact of this batch semantic.
 * ════════════════════════════════════════════════════════════ */
void poc_mxl_bridge_run(poc_mxl_bridge_t* bridge) {
  const int pipe_depth = (int)bridge->pipeline_depth;
  printf("[BRIDGE] Bridge worker started (greedy pipeline, depth=%d, grains=%u)\n",
         pipe_depth, bridge->grain_count);

  poc_inflight_entry_t ring[POC_PIPELINE_DEPTH]; /* sized to max, indexed by pipe_depth */
  int ring_head = 0, ring_tail = 0, inflight = 0;
  uint32_t idle_spins = 0; /* 3-tier back-off counter */

  while (*bridge->running) {
    /* ── Step 1: Poll CQ and drain if all transfers completed ── */
    if (inflight > 0) {
      if (try_drain(bridge, ring, &ring_head, &inflight) < 0) break;
    }

    /* ── Step 2: Greedy fill — dequeue + post until pipeline
     *    is full or the queue is empty ──────────────────────
     * After each RDMA post, poll CQ to catch early
     * completions and release grain slots ASAP.           */
    bool posted_any = false;
    idle_spins = (inflight > 0) ? 0 : idle_spins; /* reset on active */
    while (inflight < pipe_depth && *bridge->running) {
      poc_frame_entry_t entry;
      if (poc_queue_pop(bridge->queue, &entry) != 0) break; /* queue empty */

      /* Release MTL frame buffer slot immediately after dequeue.
       * The grain payload buffer remains pinned (in_use=true)
       * for the subsequent MXL commit + RDMA transfer — MTL does
       * not touch the ext_frame buffer after put_framebuff.
       *
       * This is the key fix: with framebuff_cnt=4 and pipeline_depth=2,
       * holding MTL slots until RDMA completion caused all 4 slots to
       * be occupied simultaneously (2 RDMA + 1 queue + 1 DMA), blocking
       * new frame reception ("slot get frame fail").  Releasing here
       * decouples MTL slot lifetime from RDMA lifetime. */
      st20_rx_put_framebuff(bridge->rx_handle, entry.frame_addr);

      posted_any = true;
      atomic_fetch_add(&bridge->stats->bridge_frames, 1);

      /* ── Latency T2: bridge dequeue timestamp ── */
      uint64_t t2_ns = poc_now_realtime_ns();
      uint64_t queue_lat_us = poc_ns_to_us(t2_ns - entry.enqueue_ns);

      /* ── Derive grain slot from monotonic flow index ── */
      uint64_t grain_index = entry.flow_index;
      uint32_t slot = (uint32_t)(grain_index % bridge->grain_count);

      /* ── Open grain for metadata bookkeeping ──
       * Payload data is already there from NIC DMA.
       * OpenGrain just sets _currentIndex and returns
       * the payload pointer (no payload modification). */
      mxlGrainInfo grain_info;
      uint8_t* payload = NULL;

      mxlStatus st =
          mxlFlowWriterOpenGrain(bridge->writer, grain_index, &grain_info, &payload);
      if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[BRIDGE] OpenGrain(slot=%u) failed: %d\n", slot, st);
        atomic_store_explicit(&bridge->grain_slots[slot].in_use, false,
                              memory_order_release);
        /* put_framebuff already called above after dequeue */
        continue;
      }

      /* ── NO MEMCPY — DMA zero-copy path ── */

      /* ── Embed PTP-synced timestamp for e2e measurement ── */
      poc_frame_ts_write(payload, entry.enqueue_ns);

      /* ── Set grain metadata ── */
      if (!entry.complete) grain_info.flags |= MXL_GRAIN_FLAG_INVALID;
      grain_info.validSlices = grain_info.totalSlices;

      /* ── Commit grain ── */
      st = mxlFlowWriterCommitGrain(bridge->writer, &grain_info);
      if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[BRIDGE] CommitGrain(slot=%u) failed: %d\n", slot, st);
      }

      /* ── Latency T2b: just before RDMA post ── */
      uint64_t t2b_ns = poc_now_realtime_ns();

      /* ── Post RDMA write for this grain ── */
      st = mxlFabricsInitiatorTransferGrain(bridge->initiator, grain_index, 0,
                                            grain_info.totalSlices);
      if (st == MXL_ERR_NOT_READY) {
        fprintf(stderr,
                "[BRIDGE] TransferGrain(slot=%u) not ready, "
                "skipping\n",
                (unsigned)slot);
        atomic_store_explicit(&bridge->grain_slots[slot].in_use, false,
                              memory_order_release);
        /* put_framebuff already called above after dequeue */
        continue;
      }
      if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[BRIDGE] TransferGrain(slot=%u) failed: %d\n", (unsigned)slot,
                st);
        atomic_store_explicit(&bridge->grain_slots[slot].in_use, false,
                              memory_order_release);
        /* put_framebuff already called above after dequeue */
        continue;
      }

      /* ── Push to in-flight ring ── */
      ring[ring_tail] = (poc_inflight_entry_t){
          .slot = slot,
          .grain_index = grain_index,
          .t2_ns = t2_ns,
          .t2b_ns = t2b_ns,
          .enqueue_ns = entry.enqueue_ns,
          .queue_lat_us = queue_lat_us,
      };
      ring_tail = (ring_tail + 1) % POC_PIPELINE_DEPTH;
      inflight++;
      bridge->frame_counter++;

      /* ── Interleaved CQ poll: catch completions mid-fill ──
       * If all currently in-flight transfers just completed,
       * release their slots immediately and keep filling. */
      if (inflight > 0) {
        if (try_drain(bridge, ring, &ring_head, &inflight) < 0) break;
      }
    }

    /* ── Step 3: Idle / backpressure handling ──────────────
     * Pipeline full → tight non-blocking spin-poll.
     *   CQ polled at top of next iteration (Step 1), much
     *   faster than the old MakeProgressBlocking(1ms).
     * Nothing in-flight + queue empty → yield CPU.
     * In-flight + queue empty → spin (Step 1 polls next). */
    if (posted_any) idle_spins = 0; /* reset back-off on activity */
    if (inflight >= pipe_depth) {
      /* Tight spin — Step 1 drains on next iteration */
      continue;
    }
    if (!posted_any && inflight == 0) {
      /* Truly idle: drive progress in case of pending
       * ops (e.g. connection management) and yield.
       * 3-tier adaptive back-off (MCM-inspired):
       *   Tier 1 (0-63):   _mm_pause — ~5 ns x86 hint
       *   Tier 2 (64-319): sched_yield — ~1-10 µs
       *   Tier 3 (320+):   usleep(10) — 10 µs sleep   */
      mxlFabricsInitiatorMakeProgressNonBlocking(bridge->initiator);
      if (idle_spins < 64) {
        _mm_pause();
      } else if (idle_spins < 320) {
        sched_yield();
      } else {
        usleep(10);
      }
      idle_spins++;
    }
    /* else: in-flight + queue empty → loop back to Step 1
     * without sleeping, so CQ is polled immediately. */
  }

  /* ── Drain remaining in-flight on shutdown ── */
  if (inflight > 0) {
    printf("[BRIDGE] Draining %d in-flight transfers...\n", inflight);
    uint64_t drain_deadline = now_ms() + 5000;
    while (inflight > 0 && now_ms() < drain_deadline) {
      mxlStatus dst = mxlFabricsInitiatorMakeProgressBlocking(bridge->initiator, 100);
      if (dst == MXL_STATUS_OK) {
        drain_inflight(bridge, ring, &ring_head, &inflight);
        break;
      }
      if (dst != MXL_ERR_NOT_READY) {
        fprintf(stderr,
                "[BRIDGE] Drain: MakeProgress error %d, "
                "leaking %d transfers\n",
                dst, inflight);
        break;
      }
    }
    if (inflight > 0) {
      fprintf(stderr, "[BRIDGE] Timed out draining %d transfers\n", inflight);
    }
  }

  printf(
      "[BRIDGE] Bridge worker stopped (processed %lu frames, "
      "greedy pipeline)\n",
      (unsigned long)bridge->frame_counter);
}

/* ── Cleanup ── */
void poc_mxl_bridge_destroy(poc_mxl_bridge_t* bridge) {
  /* DMA unmap the coalesced region (one mapping per bridge) */
  if (bridge->mtl && bridge->dma_region_mapped) {
    mtl_dma_unmap(bridge->mtl, bridge->dma_region_base, bridge->dma_region_iova,
                  bridge->dma_region_size);
    bridge->dma_region_mapped = false;
    /* Clear per-grain flags */
    for (uint32_t i = 0; i < bridge->grain_count; i++) {
      bridge->grain_slots[i].dma_mapped = false;
    }
  }

  if (bridge->target_info) {
    mxlFabricsFreeTargetInfo(bridge->target_info);
    bridge->target_info = NULL;
  }
  if (bridge->initiator && bridge->fab_instance) {
    mxlFabricsDestroyInitiator(bridge->fab_instance, bridge->initiator);
    bridge->initiator = NULL;
  }
  if (bridge->regions) {
    mxlFabricsRegionsFree(bridge->regions);
    bridge->regions = NULL;
  }
  if (bridge->fab_instance) {
    mxlFabricsDestroyInstance(bridge->fab_instance);
    bridge->fab_instance = NULL;
  }
  if (bridge->writer && bridge->mxl_instance) {
    mxlReleaseFlowWriter(bridge->mxl_instance, bridge->writer);
    bridge->writer = NULL;
  }
  if (bridge->mxl_instance) {
    mxlDestroyInstance(bridge->mxl_instance);
    bridge->mxl_instance = NULL;
  }
}
