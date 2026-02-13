/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file st30_pipeline_api.h
 *
 * Interfaces for st2110-30 pipeline transport.
 *
 */

#include "st30_api.h"
#include "st_pipeline_api.h"

#ifndef _ST30_PIPELINE_API_HEAD_H_
#define _ST30_PIPELINE_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Handle to tx st2110-30 pipeline session of lib */
typedef struct st30p_tx_ctx* st30p_tx_handle;
/** Handle to rx st2110-30 pipeline session of lib */
typedef struct st30p_rx_ctx* st30p_rx_handle;

/** Bit define for flags of struct st20p_tx_ops. */
enum st30p_tx_flag {
  /**
   * Flag bit in flags of struct st30p_tx_ops.
   * P TX destination mac assigned by user
   */
  ST30P_TX_FLAG_USER_P_MAC = (MTL_BIT32(0)),
  /**
   * Flag bit in flags of struct st30p_tx_ops.
   * R TX destination mac assigned by user
   */
  ST30P_TX_FLAG_USER_R_MAC = (MTL_BIT32(1)),
  /**
   * User control the frame pacing by pass a timestamp in st30_tx_frame_meta,
   * lib will wait until timestamp is reached for each frame.
   */
  ST30P_TX_FLAG_USER_PACING = (MTL_BIT32(3)),
  /**
   * Flag bit in flags of struct st30p_tx_ops.
   * If use dedicated queue for TX.
   */
  ST30P_TX_FLAG_DEDICATE_QUEUE = (MTL_BIT32(7)),
  /** Force the numa of the created session, both CPU and memory */
  ST30P_TX_FLAG_FORCE_NUMA = (MTL_BIT32(8)),

  /** Enable the st30p_tx_get_frame block behavior to wait until a frame becomes
   available or timeout(default: 1s, use st30p_tx_set_block_timeout to customize)*/
  ST30P_TX_FLAG_BLOCK_GET = (MTL_BIT32(15)),

  /**
   * Drop frames when the mtl reports late frames (transport can't keep up).
   * When late frame is detected, next frame from pipeline is ommited.
   * Untill we resume normal frame sending.
   */
  ST30P_TX_FLAG_DROP_WHEN_LATE = (MTL_BIT32(16)),
};

/** The structure info for st30 frame meta. */
struct st30_frame {
  /** frame buffer address */
  void* addr;
  /** frame format */
  enum st30_fmt fmt;
  /** channels number */
  uint16_t channel;
  /** sampling rate */
  enum st30_sampling sampling;
  /** packet time */
  enum st30_ptime ptime;
  /** frame buffer size */
  size_t buffer_size;
  /** frame valid data size, may <= buffer_size */
  size_t data_size;
  /** frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** frame timestamp value */
  uint64_t timestamp;
  /** epoch info for the done frame */
  uint64_t epoch;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /** the total packets received, not include the redundant packets */
  uint32_t pkts_total;
  /** the valid packets received on each session port. For each session port, the validity
   * of received packets can be assessed by comparing 'pkts_recv[s_port]' with
   * 'pkts_total,' which serves as an indicator of signal quality.  */
  uint32_t pkts_recv[MTL_SESSION_PORT_MAX];

  /** TAI timestamp measured right after the first packet of the frame was received */
  uint64_t receive_timestamp;

  /** priv pointer for lib, do not touch this */
  void* priv;
};

/**
 * The structure describing how to create a tx st2110-30(audio) pipeline session.
 * Include the PCIE port and other required info
 */
struct st30p_tx_ops {
  /** Mandatory. tx port info */
  struct st_tx_port port;

  /** Mandatory. Session payload format */
  enum st30_fmt fmt;
  /** Mandatory. Session channel number */
  uint16_t channel;
  /** Mandatory. Session sampling rate */
  enum st30_sampling sampling;
  /** Mandatory. Session packet time */
  enum st30_ptime ptime;
  /** Optional. The pacing engine */
  enum st30_tx_pacing_way pacing_way;
  /** Mandatory. the frame buffer count. */
  uint16_t framebuff_cnt;
  /** size for each frame buffer, must be multiple of packet size(st30_get_packet_size) */
  uint32_t framebuff_size;

  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST30P_TX_FLAG_* for possible flags */
  uint32_t flags;

  /**
   * Optional. Callback when frame available.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Optional. Callback when frame done.
   * If TX_FLAG_DROP_WHEN_LATE is enabled AND notify_frame_late is set,
   * then this will be called only when notify_frame_late is NOT called.
   */
  int (*notify_frame_done)(void* priv, struct st30_frame* frame);
  /**
   * Optional. Callback when frame timing issues occur.
   * If ST30P_TX_FLAG_DROP_WHEN_LATE is enabled: triggered when a frame is dropped
   * from the pipeline due to late transmission.
   * If ST30P_TX_FLAG_DROP_WHEN_LATE is disabled: triggered when the transport
   * layer reports late frame delivery.
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);

  /**
   * Optional. The rtp timestamp delta(us) to the start time of frame.
   * Zero means the rtp timestamp at the start of the frame.
   */
  int32_t rtp_timestamp_delta_us;

  /*
   * Optional. The size of fifo ring which used between the packet builder and pacing.
   * Leave to zero to use default value: the packet number within
   * ST30_TX_FIFO_DEFAULT_TIME_MS.
   */
  uint16_t fifo_size;
  /**
   * Optional. tx destination mac address.
   * Valid if ST30P_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /** Optional for ST30_TX_PACING_WAY_RL, the required accuracy for warmup check point */
  uint32_t rl_accuracy_ns;
  /** Optional for ST30_TX_PACING_WAY_RL, the offset time(us) for warmup check point */
  int32_t rl_offset_ns;
  /**  Use this socket if ST30P_TX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/**
 * Retrieve the general statistics(I/O) for one rx st2110-30(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(pipeline) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30p_tx_get_session_stats(st30p_tx_handle handle, struct st30_tx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-30(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30p_tx_reset_session_stats(st30p_tx_handle handle);

/**
 * Get one tx frame from the tx st2110-30 pipeline session.
 * Call st30p_tx_put_frame to return the frame to session.
 */
struct st30_frame* st30p_tx_get_frame(st30p_tx_handle handle);
/** Put back the frame which get by st30p_tx_get_frame. */
int st30p_tx_put_frame(st30p_tx_handle handle, struct st30_frame* frame);
/** Free the tx st2110-30 pipeline session. */
int st30p_tx_free(st30p_tx_handle handle);
/** Create one tx st2110-30 pipeline session */
st30p_tx_handle st30p_tx_create(mtl_handle mt, struct st30p_tx_ops* ops);
/** Online update the destination info for the tx st2110-30(pipeline) session. */
int st30p_tx_update_destination(st30p_tx_handle handle, struct st_tx_dest_info* dst);
/** Wake up the block wait on st30p_tx_get_frame if ST30P_TX_FLAG_BLOCK_GET is enabled.*/
int st30p_tx_wake_block(st30p_tx_handle handle);
/* get framebuff size */
size_t st30p_tx_frame_size(st30p_tx_handle handle);
/* get framebuff pointer */
void* st30p_tx_get_fb_addr(st30p_tx_handle handle, uint16_t idx);
/** Set the block timeout time on st30p_tx_get_frame if ST30P_TX_FLAG_BLOCK_GET is
 * enabled. */
int st30p_tx_set_block_timeout(st30p_tx_handle handle, uint64_t timedwait_ns);

/** Bit define for flags of struct st20p_rx_ops. */
enum st30p_rx_flag {
  /**
   * Flag bit in flags of struct st30p_rx_ops, for non MTL_PMD_DPDK_USER.
   * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
   * Use st30_rx_get_queue_meta to get the queue meta(queue number etc) info.
   */
  ST30P_RX_FLAG_DATA_PATH_ONLY = (MTL_BIT32(0)),
  /** Force the numa of the created session, both CPU and memory */
  ST30P_RX_FLAG_FORCE_NUMA = (MTL_BIT32(2)),

  /** Enable the st30p_rx_get_frame block behavior to wait until a frame becomes
   available or timeout(default: 1s, use st30p_rx_set_block_timeout to customize) */
  ST30P_RX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
};

/**
 * The structure describing how to create a rx st2110-30(audio) pipeline session.
 * Include the PCIE port and other required info
 */
struct st30p_rx_ops {
  /** Mandatory. rx port info */
  struct st_rx_port port;

  /** Mandatory. Session payload format */
  enum st30_fmt fmt;
  /** Mandatory. Session channel number */
  uint16_t channel;
  /** Mandatory. Session sampling rate */
  enum st30_sampling sampling;
  /** Mandatory. Session packet time */
  enum st30_ptime ptime;
  /** Mandatory. the frame buffer count. */
  uint16_t framebuff_cnt;
  /** size for each frame buffer, must be multiple of packet size(st30_get_packet_size) */
  uint32_t framebuff_size;

  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST30P_RX_FLAG_* for possible flags */
  uint32_t flags;

  /**
   * Optional. Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**  Use this socket if ST30P_RX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/**
 * Retrieve the general statistics(I/O) for one rx st2110-30(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(pipeline) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30p_rx_get_session_stats(st30p_rx_handle handle, struct st30_rx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-30(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30p_rx_reset_session_stats(st30p_rx_handle handle);

/**
 * Get one rx frame from the rx st2110-30 pipeline session.
 * Call st30p_rx_put_frame to return the frame to session.
 */
struct st30_frame* st30p_rx_get_frame(st30p_rx_handle handle);
/** Put back the frame which get by st30p_rx_get_frame. */
int st30p_rx_put_frame(st30p_rx_handle handle, struct st30_frame* frame);
/** Free the rx st2110-30 pipeline session. */
int st30p_rx_free(st30p_rx_handle handle);
/** Create one rx st2110-30 pipeline session */
st30p_rx_handle st30p_rx_create(mtl_handle mt, struct st30p_rx_ops* ops);
/** Wake up the block wait on st30p_rx_get_frame if ST30P_RX_FLAG_BLOCK_GET is enabled.*/
int st30p_rx_wake_block(st30p_rx_handle handle);
/** Set the block timeout time on st30p_rx_get_frame if ST30P_RX_FLAG_BLOCK_GET is
 * enabled. */
int st30p_rx_set_block_timeout(st30p_rx_handle handle, uint64_t timedwait_ns);
/* get framebuff size */
size_t st30p_rx_frame_size(st30p_rx_handle handle);

#if defined(__cplusplus)
}
#endif

#endif