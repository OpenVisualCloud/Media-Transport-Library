#ifndef __ST40_PIPELINE_TX_H__
#define __ST40_PIPELINE_TX_H__

#include <experimental/st40_pipeline_api.h>

#include "../st_main.h"

#if defined(__cplusplus)
extern "C" {
#endif

enum st40p_tx_frame_status {
  ST40P_TX_FRAME_FREE = 0,
  ST40P_TX_FRAME_IN_USER,         /* in user */
  ST40P_TX_FRAME_READY,           /* ready to transport */
  ST40P_TX_FRAME_IN_TRANSMITTING, /* for transport */
  ST40P_TX_FRAME_STATUS_MAX,
};

struct st40p_tx_ctx {
  struct mtl_main_impl* impl;
  int idx;
  int socket_id;
  enum mt_handle_type type; /* for sanity check */

  char ops_name[ST_MAX_NAME_LEN];
  struct st40p_tx_ops ops;

  st40_tx_handle transport;
  uint16_t framebuff_cnt;
  uint16_t framebuff_producer_idx;
  uint16_t framebuff_consumer_idx;
  struct st40p_tx_frame* framebuffs;
  pthread_mutex_t lock;
  bool ready;

  int frames_per_sec;

  /* for ST40P_TX_FLAG_BLOCK_GET */
  bool block_get;
  pthread_cond_t block_wake_cond;
  pthread_mutex_t block_wake_mutex;
  uint64_t block_timeout_ns;

  /* get frame stat */
  int stat_get_frame_try;
  int stat_get_frame_succ;
  int stat_put_frame;
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
  /** Force the numa of the created session, both CPU and memory */
  ST40P_TX_FLAG_FORCE_NUMA = (MTL_BIT32(8)),
  /** Enable the st40p_tx_get_frame block behavior to wait until a frame becomes
   available or timeout(default: 1s, use st40p_tx_set_block_timeout to customize)*/
  ST40P_TX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
};

struct st40p_tx_frame {
  enum st40p_tx_frame_status stat;
  struct st40_frame_info frame_info;
  uint16_t idx;
};

#if defined(__cplusplus)
}
#endif

#endif /* __ST40_PIPELINE_TX_H__ */
