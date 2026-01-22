/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2025 Intel Corporation
 */

#ifndef __ST40_PIPELINE_API_H__
#define __ST40_PIPELINE_API_H__

#include "st40_api.h"
#include "st_pipeline_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

/** Handle to tx st2110-40 pipeline session of lib */
typedef struct st40p_tx_ctx* st40p_tx_handle;

/** Handle to rx st2110-40 pipeline session of lib */
typedef struct st40p_rx_ctx* st40p_rx_handle;

/** The structure info for st40 frame meta. */
struct st40_frame_info {
  /** Pointer to the metadata array for this frame */
  struct st40_meta* meta;
  /** Pointer to the number of metadata entries in the frame */
  uint32_t meta_num;
  /** user data words buffer address */
  uint8_t* udw_buff_addr;
  /** user data words buffer size */
  size_t udw_buffer_size;
  /** user data words fill of the buffer */
  uint32_t udw_buffer_fill;
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

  /** Packet loss per session port based on per-port sequence tracking. */
  uint32_t port_seq_lost[MTL_SESSION_PORT_MAX];
  /** True when a per-port sequence discontinuity was detected in this frame. */
  bool port_seq_discont[MTL_SESSION_PORT_MAX];

  /** Whether a marker bit was seen on any RTP packet in this frame. */
  bool rtp_marker;
  /** True if a sequence number discontinuity was observed within this frame. */
  bool seq_discont;
  /** Number of missing RTP sequence numbers observed while assembling this frame. */
  uint32_t seq_lost;

  /** TAI timestamp measured right after the RTP packet for this frame was received */
  uint64_t receive_timestamp;

  /** True if this frame represents the second interlaced field (F=0b11). */
  bool second_field;
  /** True if the frame was flagged as interlaced (F bits indicate field 1/2). */
  bool interlaced;

  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** Bit define for flags of struct st40p_tx_ops. */
enum st40p_tx_flag {
  /**
   * Flag bit in flags of struct st40_tx_ops.
   * P TX destination mac assigned by user
   */
  ST40P_TX_FLAG_USER_P_MAC = (MTL_BIT32(0)),
  /**
   * Flag bit in flags of struct st40_tx_ops.
   * R TX destination mac assigned by user
   */
  ST40P_TX_FLAG_USER_R_MAC = (MTL_BIT32(1)),
  /**
   * Flag bit in flags of struct st40_tx_ops.
   * User control the frame pacing by pass a timestamp in st40_tx_frame_meta,
   * lib will wait until timestamp is reached for each frame.
   */
  ST40P_TX_FLAG_USER_PACING = (MTL_BIT32(3)),
  /**
   * Flag bit in flags of struct st40_tx_ops.
   * If enabled, lib will assign the rtp timestamp to the value in
   * st40_tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
   */
  ST40P_TX_FLAG_USER_TIMESTAMP = (MTL_BIT32(4)),
  /**
   * Flag bit in flags of struct st40_tx_ops.
   * If enable the rtcp.
   */
  ST40P_TX_FLAG_ENABLE_RTCP = (MTL_BIT32(5)),
  /**
   * Flag bit in flags of struct st40_tx_ops.
   * If use dedicated queue for TX.
   */
  ST40P_TX_FLAG_DEDICATE_QUEUE = (MTL_BIT32(6)),
  /**
   * Drop frames when the mtl reports late frames (transport can't keep up).
   * When late frame is detected, next frame from pipeline is ommited.
   * Untill we resume normal frame sending.
   */
  ST40P_TX_FLAG_DROP_WHEN_LATE = (MTL_BIT32(7)),
  /**
   * NOT SUPPORTED YET
   * Force the numa of the created session, both CPU and memory
   */
  ST40P_TX_FLAG_FORCE_NUMA = (MTL_BIT32(8)),
  /**
   * Works together with ST40P_TX_FLAG_USER_PACING and makes the first packet of the
   * frame leave exactly at the user provided timestamp instead of aligning to epochs.
   */
  ST40P_TX_FLAG_EXACT_USER_PACING = (MTL_BIT32(9)),
  /** Force one ANC packet per RTP and allow splitting multi-ANC frames. */
  ST40P_TX_FLAG_SPLIT_ANC_BY_PKT = (MTL_BIT32(10)),
  /** Enable the st40p_tx_get_frame block behavior to wait until a frame becomes
   available or timeout(default: 1s, use st40p_tx_set_block_timeout to customize)*/
  ST40P_TX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
};

/**
 * The structure describing how to create a tx st2110-40(ancillary) pipeline session.
 * Include the PCIE port and other required info
 */
struct st40p_tx_ops {
  /** Mandatory. tx port info */
  struct st_tx_port port;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. the frame buffer count. */
  uint16_t framebuff_cnt;
  /** Maximum combined size of all user data words to send in single st40p frame */
  uint32_t max_udw_buff_size;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST40P_TX_FLAG_* for possible flags */
  uint32_t flags;
  /** Optional. test-only mutation config; ignored when pattern is NONE. */
  struct st40_tx_test_config test;
  /**
   * Optional. Callback when frame available.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Optional. Callback when frame done. If TX_FLAG_DROP_WHEN_LATE is enabled
   * this will be called only when the notify_frame_late is not triggered.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, struct st40_frame_info* frame_info);

  /**
   * Optional. Callback when frame timing issues occur.
   * If ST40P_TX_FLAG_DROP_WHEN_LATE is enabled: triggered when a frame is dropped
   * from the pipeline due to late transmission.
   * If ST40P_TX_FLAG_DROP_WHEN_LATE is disabled: triggered when the transport
   * layer reports late frame delivery.
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);

  /**
   * Optional. tx destination mac address.
   * Valid if ST40P_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];
};

/** Bit define for flags of struct st40p_rx_ops. */
enum st40p_rx_flag {
  /**
   * Flag bit in flags of struct st40_rx_ops, for non MTL_PMD_DPDK_USER.
   * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
   * Use st40p_rx_get_queue_meta to get the queue meta(queue number etc) info.
   */
  ST40P_RX_FLAG_DATA_PATH_ONLY = (MTL_BIT32(0)),
  /**
   * Flag bit in flags of struct st40_rx_ops.
   * If enable the rtcp.
   */
  ST40P_RX_FLAG_ENABLE_RTCP = (MTL_BIT32(1)),
  /**
   * NOT SUPPORTED YET
   * Force the numa of the created session, both CPU and memory
   */
  ST40P_RX_FLAG_FORCE_NUMA = (MTL_BIT32(2)),
  /**
   * If set, lib will auto-detect progressive vs interlaced using RTP F bits. The
   * st40p_rx_ops.interlaced field becomes optional and will be updated after
   * detection.
   */
  ST40P_RX_FLAG_AUTO_DETECT_INTERLACED = (MTL_BIT32(3)),
  /** Enable the st40p_rx_get_frame block behavior to wait until a frame becomes
   available or timeout(default: 1s, use st40p_rx_set_block_timeout to customize)*/
  ST40P_RX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
};

/**
 * The structure describing how to create a rx st2110-40(ancillary) pipeline session.
 * Include the PCIE port and other required info
 */
struct st40p_rx_ops {
  /** Mandatory. rx port info */
  struct st_rx_port port;
  /** Mandatory unless ST40P_RX_FLAG_AUTO_DETECT_INTERLACED is set. interlaced or not */
  bool interlaced;
  /** Mandatory. the frame buffer count. */
  uint16_t framebuff_cnt;
  /** Maximum combined size of all user data words to receive in single st40p frame */
  uint32_t max_udw_buff_size;
  /** Mandatory. RTP ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST40P_RX_FLAG_* for possible flags */
  uint32_t flags;
  /**
   * Optional. Callback when frame available.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
};

/**
 * Retrieve the general statistics(I/O) for one rx st2110-40(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(pipeline) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st40p_tx_get_session_stats(st40p_tx_handle handle, struct st40_tx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-40(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st40p_tx_reset_session_stats(st40p_tx_handle handle);

/** Create one tx st2110-40 pipeline session */
st40p_tx_handle st40p_tx_create(mtl_handle mt, struct st40p_tx_ops* ops);

/**
 * Get one tx frame from the tx st2110-40 pipeline session.
 * Call st40p_tx_put_frame to return the frame to session.
 */
struct st40_frame_info* st40p_tx_get_frame(st40p_tx_handle handle);

/** Return the frame that was requested by st40p_tx_get_frame. */
int st40p_tx_put_frame(st40p_tx_handle handle, struct st40_frame_info* frame_info);

/** Free the tx st2110-40 pipeline session. */
int st40p_tx_free(st40p_tx_handle handle);

/** Update the destination for the tx st2110-40 pipeline session. */
int st40p_tx_update_destination(st40p_tx_handle handle, struct st_tx_dest_info* dst);

/** Wake up the block for the tx st2110-40 pipeline session. */
int st40p_tx_wake_block(st40p_tx_handle handle);

/** Set the block timeout for the tx st2110-40 pipeline session. */
int st40p_tx_set_block_timeout(st40p_tx_handle handle, uint64_t timedwait_ns);

/** Get the maximum user data words buffer size for the tx st2110-40 pipeline session. */
size_t st40p_tx_max_udw_buff_size(st40p_tx_handle handle);

/** Get the user data words buffer address for the tx st2110-40 pipeline session. */
void* st40p_tx_get_udw_buff_addr(st40p_tx_handle handle, uint16_t idx);

/** Get the framebuffer address for the tx st2110-40 pipeline session. */
void* st40p_tx_get_fb_addr(st40p_tx_handle handle, uint16_t idx);

/** Create one rx st2110-40 pipeline session */
st40p_rx_handle st40p_rx_create(mtl_handle mt, struct st40p_rx_ops* ops);

/**
 * Get one rx frame from the rx st2110-40 pipeline session.
 * Call st40p_rx_put_frame to return the frame to session.
 */
struct st40_frame_info* st40p_rx_get_frame(st40p_rx_handle handle);

/** Return the frame that was requested by st40p_rx_get_frame. */
int st40p_rx_put_frame(st40p_rx_handle handle, struct st40_frame_info* frame_info);

/** Free the rx st2110-40 pipeline session. */
int st40p_rx_free(st40p_rx_handle handle);

/**
 * Get the queue meta attached to rx st2110-40 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-40 pipeline session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st40p_rx_get_queue_meta(st40p_rx_handle handle, struct st_queue_meta* meta);

/**
 * Retrieve the general statistics(I/O) for one rx st2110-40(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(pipeline) session.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st40p_rx_get_session_stats(st40p_rx_handle handle, struct st40_rx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-40(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(pipeline) session.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st40p_rx_reset_session_stats(st40p_rx_handle handle);

/** Update the source for the rx st2110-40 pipeline session. */
int st40p_rx_update_source(st40p_rx_handle handle, struct st_rx_source_info* src);

/** Wake up the block for the rx st2110-40 pipeline session. */
int st40p_rx_wake_block(st40p_rx_handle handle);

/** Set the block timeout for the rx st2110-40 pipeline session. */
int st40p_rx_set_block_timeout(st40p_rx_handle handle, uint64_t timedwait_ns);

/** Get the maximum user data words buffer size for the rx st2110-40 pipeline session. */
size_t st40p_rx_max_udw_buff_size(st40p_rx_handle handle);

/** Get the user data words buffer address for the rx st2110-40 pipeline session. */
void* st40p_rx_get_udw_buff_addr(st40p_rx_handle handle, uint16_t idx);

#if defined(__cplusplus)
}
#endif

#endif /* __ST40_PIPELINE_API_H__ */
