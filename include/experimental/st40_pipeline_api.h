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

/** The structure info for st40 frame meta. */
struct st40_frame_info {
  /** frame buffer address */
  struct st40_frame* anc_frame;
  /** user data words buffer address */
  void* udw_buff_addr;
  /** user data words buffer size */
  size_t udw_buffer_size;
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
   * NOT SUPPORTED YET
   * Force the numa of the created session, both CPU and memory
   */
  ST40P_TX_FLAG_FORCE_NUMA = (MTL_BIT32(8)),
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
  /**
   * Optional. Callback when frame available.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Optional. Callback when frame done.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, struct st40_frame_info* frame_info);

  /**
   * Optional. tx destination mac address.
   * Valid if ST40P_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];
};

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

#if defined(__cplusplus)
}
#endif

#endif /* __ST40_PIPELINE_API_H__ */
