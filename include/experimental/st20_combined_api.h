/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st20_redundant_combined.h
 *
 * Interfaces for st2110-20 combined redundant transport, experimental feature only.
 *
 */

#include "../st20_api.h"

#ifndef _ST20_COMBINED_API_HEAD_H_
/** Marco for re-include protect */
#define _ST20_COMBINED_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Handle to rx st2110-22 pipeline session of lib */
typedef struct st20rc_rx_ctx *st20rc_rx_handle;

/**
 * Flag bit in flags of struct st20rc_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st20p_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST20RC_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st20rc_rx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST20RC_RX_FLAG_ENABLE_VSYNC (MTL_BIT32(1))

/**
 * Flag bit in flags of struct st20rc_rx_ops.
 * If set, lib will pass the incomplete frame to app also.
 * User can check st_frame_status data for the frame integrity
 */
#define ST20RC_RX_FLAG_RECEIVE_INCOMPLETE_FRAME (MTL_BIT32(16))
/**
 * Flag bit in flags of struct st20rc_rx_ops.
 * If set, lib will try to allocate DMA memory copy offload from
 * dma_dev_port(mtl_init_params) list.
 * Pls note it could fallback to CPU if no DMA device is available.
 */
#define ST20RC_RX_FLAG_DMA_OFFLOAD (MTL_BIT32(17))
/**
 * Flag bit in flags of struct st20rc_rx_ops.
 * Only ST20_PACKING_BPM stream can enable this offload as software limit
 * Try to enable header split offload feature.
 */
#define ST20RC_RX_FLAG_HDR_SPLIT (MTL_BIT32(19))

/**
 * The structure describing how to create a rx st2110-20(redundant) session.
 * Frame based.
 */
struct st20rc_rx_ops {
  /** name */
  const char *name;
  /** private data to the callback function */
  void *priv;
  union {
    /** Mandatory. multicast IP address or sender IP for unicast */
    uint8_t ip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
    /** deprecated, use ip_addr instead, sip_addr is confused */
    uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN] __mtl_deprecated_msg(
        "Use ip_addr instead");
  };
  /** num of ports this session attached to, must be 2 */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session packing mode */
  enum st20_packing packing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** interlace or not, false: non-interlaced: true: interlaced */
  bool interlaced;
  /** 7 bits payload type define in RFC3550. Zero means disable the
   * payload_type check on the RX pkt path */
  uint8_t payload_type;
  /** Optional. Synchronization source defined in RFC3550, RX session will check the
   * incoming RTP packets match the ssrc. Leave to zero to disable the ssrc check */
  uint32_t ssrc;
  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];

  /** flags, value in ST20RC_RX_FLAG_* */
  uint32_t flags;
  /**
   * the ST20_TYPE_FRAME_LEVEL frame buffer count requested,
   * should be in range [2, ST20_FB_MAX_COUNT].
   */
  uint16_t framebuff_cnt;
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib receive one frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st20rc_rx_put_frame
   * to return the frame when it finish the handling
   *   < 0: the error code if app can't handle, lib will call st20rc_rx_put_frame then.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void *priv, void *frame, struct st20_rx_frame_meta *meta);
  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void *priv, enum st_event event, void *args);
};

/**
 * Create one rx st2110-20(redundant) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx st2110-20(video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-20(redundant) session.
 */
st20rc_rx_handle st20rc_rx_create(mtl_handle mt, struct st20rc_rx_ops *ops);

/**
 * Free the rx st2110-20(redundant) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(redundant) session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20rc_rx_free(st20rc_rx_handle handle);

/**
 * Put back the received buff get from notify_frame_ready.
 *
 * @param handle
 *   The handle to the rx st2110-20(redundant) session.
 * @param frame
 *   The framebuffer pointer.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20rc_rx_put_frame(st20rc_rx_handle handle, void *frame);

/**
 * Get the framebuffer size for the rx st2110-20(redundant) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - size.
 */
size_t st20rc_rx_get_framebuffer_size(st20rc_rx_handle handle);

/**
 * Get the framebuffer count for the rx st2110-20(redundant) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - count.
 */
int st20rc_rx_get_framebuffer_count(st20rc_rx_handle handle);

/**
 * Dump st2110-20(redundant) packets to pcapng file.
 *
 * @param handle
 *   The handle to the rx st2110-20(redundant) session.
 * @param max_dump_packets
 *   The max number of packets to be dumped.
 * @param sync
 *   synchronous or asynchronous, true means this func will return after dump
 * progress is finished.
 * @param meta
 *   The meta data returned, only for synchronous, leave to NULL if not need the meta.
 * @return
 *   - 0: Success, rx st2110-20(redundant) session pcapng dump succ.
 *   - <0: Error code of the rx st2110-20(redundant) session pcapng dump.
 */
int st20rc_rx_pcapng_dump(st20rc_rx_handle handle, uint32_t max_dump_packets, bool sync,
                          struct st_pcap_dump_meta *meta);

#if defined(__cplusplus)
}
#endif

#endif
