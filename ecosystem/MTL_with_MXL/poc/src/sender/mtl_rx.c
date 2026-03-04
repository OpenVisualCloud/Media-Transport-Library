/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host A: MTL ST20 RX session implementation
 *
 * Split into two phases:
 *   Phase 1 (poc_mtl_rx_init_transport): DPDK/MTL init
 *   Phase 2 (poc_mtl_rx_create_session): ST20 RX session with zero-copy ext_frame
 */

#include "poc_mtl_rx.h"
#include "poc_mxl_bridge.h"   /* for poc_grain_slot_t (zero-copy opaque) */
#include "poc_v210_utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── RX callback — runs on MTL lcore, must be non-blocking ── */
static int rx_frame_ready_cb(void *priv, void *frame, struct st20_rx_frame_meta *meta)
{
    poc_mtl_rx_t *rx = (poc_mtl_rx_t *)priv;

    atomic_fetch_add(&rx->stats->rx_frames, 1);

    bool complete = st_is_frame_complete(meta->status);
    if (!complete) {
        atomic_fetch_add(&rx->stats->rx_incomplete, 1);
        if (!rx->accept_incomplete) {
            /* Drop incomplete frame — return buffer immediately.
             * CRITICAL: also release the grain slot that was claimed
             * by query_ext_frame, otherwise it leaks permanently. */
            if (meta->opaque) {
                poc_grain_slot_t *gs = (poc_grain_slot_t *)meta->opaque;
                atomic_store_explicit(&gs->in_use, false, memory_order_release);
            }
            st20_rx_put_framebuff(rx->rx_handle, frame);
            return -1;
        }
    }

    /*
     * In zero-copy mode, meta->opaque is the poc_grain_slot_t pointer
     * set by the query_ext_frame callback.  We store the flow_index
     * from it so the bridge thread knows which grain to commit.
     */
    uint64_t flow_index = 0;
    if (meta->opaque) {
        poc_grain_slot_t *gs = (poc_grain_slot_t *)meta->opaque;
        flow_index = gs->flow_index;
    }

    /* Build queue entry */
    poc_frame_entry_t entry = {
        .frame_addr      = frame,
        .timestamp        = meta->timestamp,
        .flow_index       = flow_index,
        .frame_recv_size  = (uint32_t)meta->frame_recv_size,
        .frame_total_size = (uint32_t)meta->frame_total_size,
        .complete         = complete,
        .enqueue_ns       = poc_now_realtime_ns(),
    };

    /* Enqueue — if queue is full, drop and return buffer */
    if (poc_queue_push(rx->queue, &entry) != 0) {
        atomic_fetch_add(&rx->stats->rx_drops, 1);
        st20_rx_put_framebuff(rx->rx_handle, frame);
        return -1;
    }

    return 0;
}

/* ── Helper: convert fps numerator/denominator to MTL st_fps enum ── */
static enum st_fps fps_to_mtl(uint32_t num, uint32_t den)
{
    /* Common frame rates */
    double fps_val = (double)num / (double)den;
    if (fps_val > 119.5)  return ST_FPS_P120;
    if (fps_val > 59.5)   return ST_FPS_P60;
    if (fps_val > 49.5)   return ST_FPS_P50;
    if (fps_val > 29.5 && den == 1)    return ST_FPS_P30;
    if (fps_val > 29.5)   return ST_FPS_P29_97;
    if (fps_val > 24.5 && den == 1)    return ST_FPS_P25;
    if (fps_val > 23.5)   return ST_FPS_P24;
    return ST_FPS_P30;  /* fallback */
}

/* ── Wrapper: dispatch query_ext_frame through rx→bridge ── */
static int query_ext_frame_wrapper(void *priv, struct st20_ext_frame *ext_frame,
                                   struct st20_rx_frame_meta *meta)
{
    poc_mtl_rx_t *rx = (poc_mtl_rx_t *)priv;
    return rx->ext_frame_fn(rx->ext_frame_ctx, ext_frame, meta);
}

/* ════════════════════════════════════════════
 * Phase 1: MTL transport init (DPDK)
 * ════════════════════════════════════════════ */
int poc_mtl_rx_init_transport(poc_mtl_rx_t *rx, const poc_config_t *cfg)
{
    memset(rx, 0, sizeof(*rx));
    rx->accept_incomplete = cfg->accept_incomplete;

    /* ── MTL init parameters ── */
    struct mtl_init_params param;
    memset(&param, 0, sizeof(param));

    param.num_ports = cfg->redundancy ? 2 : 1;
    snprintf(param.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->mtl_port);

    /* SIP address: local IP for this DPDK port */
    memcpy(param.sip_addr[MTL_PORT_P], cfg->sip, MTL_IP_ADDR_LEN);

    /* ST 2022-7 redundant port */
    if (cfg->redundancy) {
        snprintf(param.port[MTL_PORT_R], MTL_PORT_MAX_LEN, "%s", cfg->mtl_port_r);
        memcpy(param.sip_addr[MTL_PORT_R], cfg->sip_r, MTL_IP_ADDR_LEN);
        printf("[MTL_RX] ST 2022-7 redundancy: P=%s R=%s\n",
               cfg->mtl_port, cfg->mtl_port_r);
    }

    param.flags = MTL_FLAG_DEV_AUTO_START_STOP;
    param.log_level = MTL_LOG_LEVEL_INFO;
    param.rx_sessions_cnt_max = 1;
    param.tx_sessions_cnt_max = 0;

    /* Pin MTL/DPDK lcores to specific CPUs if configured */
    if (cfg->mtl_lcores[0]) {
        param.lcores = (char *)cfg->mtl_lcores;
        printf("[MTL_RX] Pinning MTL lcores to: %s\n", cfg->mtl_lcores);
    }

    rx->mtl = mtl_init(&param);
    if (!rx->mtl) {
        fprintf(stderr, "[MTL_RX] mtl_init failed (port=%s)\n", cfg->mtl_port);
        return -1;
    }
    printf("[MTL_RX] MTL initialised on port %s\n", cfg->mtl_port);
    return 0;
}

/* ════════════════════════════════════════════
 * Phase 2: Create ST20 RX session with zero-copy ext_frame
 * ════════════════════════════════════════════ */
int poc_mtl_rx_create_session(poc_mtl_rx_t *rx, const poc_config_t *cfg,
                              poc_frame_queue_t *queue, poc_stats_t *stats,
                              void *ext_frame_priv,
                              int (*query_ext_frame)(void *, struct st20_ext_frame *,
                                                     struct st20_rx_frame_meta *))
{
    rx->queue = queue;
    rx->stats = stats;

    /* ── ST20 RX session ── */
    struct st20_rx_ops ops;
    memset(&ops, 0, sizeof(ops));

    ops.name = "mtl_mxl_poc_rx";
    ops.priv = rx;
    ops.num_port = cfg->redundancy ? 2 : 1;

    /* Multicast/unicast IP — P path */
    memcpy(ops.ip_addr[MTL_SESSION_PORT_P], cfg->rx_ip, MTL_IP_ADDR_LEN);
    snprintf(ops.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", cfg->mtl_port);
    ops.udp_port[MTL_SESSION_PORT_P] = cfg->rx_udp_port;

    /* ST 2022-7 — R path */
    if (cfg->redundancy) {
        memcpy(ops.ip_addr[MTL_SESSION_PORT_R], cfg->rx_ip_r, MTL_IP_ADDR_LEN);
        snprintf(ops.port[MTL_SESSION_PORT_R], MTL_PORT_MAX_LEN, "%s", cfg->mtl_port_r);
        ops.udp_port[MTL_SESSION_PORT_R] = cfg->rx_udp_port_r;
    }

    ops.type = ST20_TYPE_FRAME_LEVEL;
    ops.width = cfg->video.width;
    ops.height = cfg->video.height;
    ops.fps = fps_to_mtl(cfg->video.fps_num, cfg->video.fps_den);
    ops.interlaced = false;
    ops.fmt = ST20_FMT_YUV_422_10BIT;
    ops.payload_type = cfg->payload_type;

    ops.framebuff_cnt = (uint16_t)cfg->framebuff_cnt;
    if (ops.framebuff_cnt < 2) ops.framebuff_cnt = 4;
    if (ops.framebuff_cnt > ST20_FB_MAX_COUNT) ops.framebuff_cnt = ST20_FB_MAX_COUNT;

    ops.notify_frame_ready = rx_frame_ready_cb;

    /* Zero-copy: use dynamic ext_frame callback.
     * ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME is required for dynamic ext_frame
     * mode (the lib checks query_ext_frame != NULL). */
    ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    if (query_ext_frame) {
        /* Store the external callback+context; ops.priv = rx so we use
         * a local wrapper that dispatches to the real function. */
        rx->ext_frame_ctx = ext_frame_priv;
        rx->ext_frame_fn  = query_ext_frame;
        ops.query_ext_frame = query_ext_frame_wrapper;
        printf("[MTL_RX] Zero-copy mode: using dynamic ext_frame from MXL grain buffers\n");
    }

    /* Also accept incomplete frames if user configured it */
    if (cfg->accept_incomplete) {
        ops.flags |= ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME;
    }

    rx->rx_handle = st20_rx_create(rx->mtl, &ops);
    if (!rx->rx_handle) {
        fprintf(stderr, "[MTL_RX] st20_rx_create failed\n");
        return -1;
    }

    printf("[MTL_RX] ST20 RX session created: %ux%u %u/%u fps, YUV422_10BIT, fb_cnt=%u%s%s\n",
           cfg->video.width, cfg->video.height,
           cfg->video.fps_num, cfg->video.fps_den,
           ops.framebuff_cnt,
           query_ext_frame ? " [ZERO-COPY]" : "",
           cfg->redundancy ? " [ST2022-7]" : "");

    return 0;
}

void poc_mtl_rx_destroy(poc_mtl_rx_t *rx)
{
    if (rx->rx_handle) {
        st20_rx_free(rx->rx_handle);
        rx->rx_handle = NULL;
    }
    if (rx->mtl) {
        mtl_uninit(rx->mtl);
        rx->mtl = NULL;
    }
}
