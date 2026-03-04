/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host A: MXL bridge worker (zero-copy DMA → grain → RDMA)
 *
 * Zero-copy data path:
 *   1. MTL NIC DMAs received packets directly into MXL grain payload buffers
 *      (via query_ext_frame callback providing grain addresses)
 *   2. Bridge thread commits the grain and triggers RDMA transfer
 *   3. No memcpy in the hot path
 */

#ifndef POC_MXL_BRIDGE_H
#define POC_MXL_BRIDGE_H

#include "poc_types.h"
#include "poc_frame_queue.h"
#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/fabrics.h>
#include <mtl_api.h>
#include <st20_api.h>
#include <stdatomic.h>

#define POC_MAX_GRAIN_SLOTS 32

/* Per-grain-slot tracking for zero-copy ext_frame management */
typedef struct {
    uint8_t   *addr;        /* payload virtual address in MXL shared memory */
    mtl_iova_t iova;        /* DMA-mapped IOVA for this payload */
    size_t     buf_len;     /* payload buffer length (grainSize) */
    uint64_t   flow_index;  /* MXL flow index assigned to this slot */
    atomic_bool in_use;     /* true while DMA or RDMA is in progress */
    /* Per-grain DMA mapping (each grain may be in a separate mmap) */
    void      *dma_base;    /* page-aligned base for this grain's DMA map */
    size_t     dma_size;    /* page-aligned size of this grain's DMA map */
    mtl_iova_t dma_iova;    /* IOVA returned by mtl_dma_map for this grain */
    bool       dma_mapped;  /* true if this grain was successfully DMA-mapped */
} poc_grain_slot_t;

typedef struct {
    /* MXL flow objects (owned by bridge) */
    mxlInstance        mxl_instance;
    mxlFlowWriter     writer;
    mxlFlowConfigInfo config_info;

    /* Fabrics initiator */
    mxlFabricsInstance  fab_instance;
    mxlFabricsInitiator initiator;
    mxlFabricsRegions   regions;
    mxlFabricsTargetInfo target_info;  /* parsed from Host B string */

    /* MTL handle (for DMA mapping / put_framebuff) */
    mtl_handle          mtl;

    /* Zero-copy grain slot pool */
    poc_grain_slot_t   grain_slots[POC_MAX_GRAIN_SLOTS];
    uint32_t           grain_count;
    size_t             grain_size;       /* payload size per grain */
    atomic_uint next_slot;     /* round-robin slot index for query_ext_frame */
    atomic_uint_fast64_t next_grain_index; /* monotonically increasing grain index for MXL API */

    /* Coalesced DMA mapping covering all pipeline grains in one region.
     * This avoids exhausting DPDK's memseg list (≈116 entries max)
     * when running many streams. */
    void      *dma_region_base;   /* page-aligned base of coalesced region */
    size_t     dma_region_size;   /* page-aligned total size */
    mtl_iova_t dma_region_iova;   /* IOVA of the coalesced mapping */
    bool       dma_region_mapped; /* true if coalesced region is mapped */

    /* Shared state */
    poc_frame_queue_t  *queue;
    st20_rx_handle      rx_handle;     /* set after st20_rx_create */
    poc_stats_t        *stats;
    volatile bool      *running;

    /* Config */
    uint32_t           pipeline_depth;  /* effective RDMA pipeline depth (≤ grain_count - 3) */
    uint64_t frame_counter;   /* bridge thread's own counter (for verification) */
} poc_mxl_bridge_t;

/* Phase 1: Initialise MXL flow writer, pre-compute grain addresses,
 * DMA-map grain buffers.  Must be called AFTER mtl_init().
 * Does NOT connect fabrics yet. */
int  poc_mxl_bridge_init(poc_mxl_bridge_t *bridge,
                         const poc_config_t *cfg,
                         mtl_handle mtl,
                         poc_frame_queue_t *queue,
                         poc_stats_t *stats,
                         volatile bool *running);

/* Phase 2: Connect fabrics to the remote target.
 * Must be called after st20_rx_create sets bridge->rx_handle. */
int  poc_mxl_bridge_connect(poc_mxl_bridge_t *bridge,
                            const poc_config_t *cfg);

/* MTL query_ext_frame callback — assigns MXL grain buffers as DMA targets.
 * Called from MTL lcore context; must be non-blocking. */
int  poc_mxl_bridge_query_ext_frame(void *priv,
                                    struct st20_ext_frame *ext_frame,
                                    struct st20_rx_frame_meta *meta);

/* Run the bridge worker loop.  Call from a dedicated thread.
 * Blocks until *running becomes false. */
void poc_mxl_bridge_run(poc_mxl_bridge_t *bridge);

/* Destroy bridge resources (MXL + fabrics + DMA unmap). */
void poc_mxl_bridge_destroy(poc_mxl_bridge_t *bridge);

#endif /* POC_MXL_BRIDGE_H */
