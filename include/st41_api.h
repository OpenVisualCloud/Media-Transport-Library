/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st41_api.h
 *
 * Interfaces for st2110-41 transport.
 *
 */

#include "st_api.h"

#ifndef _ST41_API_HEAD_H_
#define _ST41_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Handle to tx st2110-41(fast metadata) session
 */
typedef struct st_tx_fastmetadata_session_handle_impl* st41_tx_handle;
/**
 * Handle to rx st2110-41(fast metadata) session
 */
typedef struct st_rx_fastmetadata_session_handle_impl* st41_rx_handle;

/**
 * Flag bit in flags of struct st41_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST41_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st41_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST41_TX_FLAG_USER_R_MAC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st41_tx_ops.
 * User control the frame pacing by pass a timestamp in st41_tx_frame_meta,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST41_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st41_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * st41_tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST41_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st41_tx_ops.
 * If enable the rtcp.
 */
#define ST41_TX_FLAG_ENABLE_RTCP (MTL_BIT32(5))
/**
 * Flag bit in flags of struct st41_tx_ops.
 * If use dedicated queue for TX.
 */
#define ST41_TX_FLAG_DEDICATE_QUEUE (MTL_BIT32(6))

/**
 * Flag bit in flags of struct st30_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st41_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST41_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st41_rx_ops.
 * If enable the rtcp.
 */
#define ST41_RX_FLAG_ENABLE_RTCP (MTL_BIT32(1))

/**
 * Session type of st2110-41(fast metadata) streaming
 */
enum st41_type {
  ST41_TYPE_FRAME_LEVEL = 0, /**< app interface lib based on frame level */
  ST41_TYPE_RTP_LEVEL,       /**< app interface lib based on RTP level */
  ST41_TYPE_MAX,             /**< max value of this enum */
};

/**
 * A structure describing a st2110-41(fast metadata) rtp header
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st41_rtp_hdr {
  /** Rtp rfc3550 base hdr */
  struct st_rfc3550_rtp_hdr base;
  union {
    struct {
      /** Data Item Contents - Number of 32-bit data elements that follow */
      uint32_t data_item_length : 9;
      /** Data Item K-bit */
      uint32_t data_item_k_bit : 1;
      /** Data Item Type */
      uint32_t data_item_type : 22;
    } st41_hdr_chunk;
    /** Handle to make operating on st41_hdr_chunk buffer easier */
    uint32_t swaped_st41_hdr_chunk;
  };
});
#else
MTL_PACK(struct st41_rtp_hdr {
  /** Rtp rfc3550 base hdr */
  struct st_rfc3550_rtp_hdr base;
  union {
    struct {
      /** Data Item Type */
      uint32_t data_item_type : 22;
      /** Data Item K-bit */
      uint32_t data_item_k_bit : 1;
      /** Data Item Contents - Number of 32-bit data elements that follow */
      uint32_t data_item_length : 9;
    } st41_hdr_chunk;
    /** Handle to make operating on st41_hdr_chunk buffer easier */
    uint32_t swaped_st41_hdr_chunk;
  };
});
#endif

/**
 * Structure for ST2110-41(fast metadata) frame
 */
struct st41_frame {
  uint16_t data_item_length_bytes;      /** Size of the User Data Words  */
  uint8_t* data;                        /**<  Handle to data buffer  */
};

/**
 * Frame meta data of st2110-41(fast metadata) tx streaming
 */
struct st41_tx_frame_meta {
  /** Frame fps */
  enum st_fps fps;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
  /** epoch */
  uint64_t epoch;
  /** Second field type indicate for interlaced mode, set by user */
  bool second_field;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
};

/**
 * The structure describing how to create a tx st2110-41(fast metadata) session.
 * Include the PCIE port and other required info.
 */
struct st41_tx_ops {
  /** Mandatory. destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. Session streaming type, frame or RTP */
  enum st41_type type;
  /** Mandatory. Session fps */
  enum st_fps fps;
  /** Mandatory. 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** Mandatory. 22 bits data item type */
  uint32_t fmd_dit;
  /** Mandatory. 1 bit data item K-bit */
  uint8_t fmd_k_bit;

  /** Mandatory. interlaced or not */
  bool interlaced;

  /** Optional. Synchronization source defined in RFC3550, if zero the session will assign
   * a random value */
  uint32_t ssrc;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST41_TX_FLAG_* for possible flags */
  uint32_t flags;

  /**
   * Mandatory for ST41_TYPE_FRAME_LEVEL.
   * the frame buffer count requested for one st41 tx session,
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST41_TYPE_FRAME_LEVEL. callback when lib require a new frame for
   * sending. User should provide the next available frame index to next_frame_idx. It
   * implicit means the frame ownership will be transferred to lib, And only non-block
   * method can be used in this callback as it run from lcore tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st41_tx_frame_meta* meta);
  /**
   * Optional for ST41_TYPE_FRAME_LEVEL. callback when lib finish sending one frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st41_tx_frame_meta* meta);

  /** Optional. UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /**
   * Optional. tx destination mac address.
   * Valid if ST41_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /** Mandatory for ST41_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional for ST41_TYPE_RTP_LEVEL. callback when lib finish the sending of one rtp
   * packet, And only non-block method can be used in this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_rtp_done)(void* priv);
};

/**
 * The structure describing how to create a rx st2110-41(fast metadata) session.
 * Include the PCIE port and other required info
 */
struct st41_rx_ops {
  union {
    /** Mandatory. multicast IP address or sender IP for unicast */
    uint8_t ip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
    /** deprecated, use ip_addr instead, sip_addr is confused */
    uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN] __mtl_deprecated_msg(
        "Use ip_addr instead");
  };
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. UDP dest port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. 7 bits payload type define in RFC3550. Zero means disable the
   * payload_type check on the RX pkt path */
  uint8_t payload_type;
  /** Mandatory. interlaced or not */
  bool interlaced;

  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Optional. Synchronization source defined in RFC3550, RX session will check the
   * incoming RTP packets match the ssrc. Leave to zero to disable the ssrc check */
  uint32_t ssrc;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST41_RX_FLAG_* for possible flags */
  uint32_t flags;

  /** Mandatory. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional. the callback when lib finish the sending of one rtp packet. And only
   * non-block method can be used in this callback as it run from lcore tasklet routine.
   */
  int (*notify_rtp_ready)(void* priv);
};

/**
 * Create one tx st2110-41(fast metadata) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-41(fast metadata)
 * session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-41(fast metadata) session.
 */
st41_tx_handle st41_tx_create(mtl_handle mt, struct st41_tx_ops* ops);

/**
 * Free the tx st2110-41(fast metadata) session.
 *
 * @param handle
 *   The handle to the tx st2110-41(fast metadata) session.
 * @return
 *   - 0: Success, tx st2110-41(fast metadata) session freed.
 *   - <0: Error code of the tx st2110-41(fast metadata) session free.
 */
int st41_tx_free(st41_tx_handle handle);

/**
 * Online update the destination info for the tx st2110-41(fast metadata) session.
 *
 * @param handle
 *   The handle to the tx st2110-41(fast metadata) session.
 * @param dst
 *   The pointer to the tx st2110-41(fast metadata) destination info.
 * @return
 *   - 0: Success, tx st2110-41(fast metadata) session destination update succ.
 *   - <0: Error code of the rx st2110-41(fast metadata) session destination update.
 */
int st41_tx_update_destination(st41_tx_handle handle, struct st_tx_dest_info* dst);

/**
 * Get the framebuffer pointer from the tx st2110-41(fast metadata) session.
 * For ST41_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-41(fast metadata) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st41_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st41_tx_get_framebuffer(st41_tx_handle handle, uint16_t idx);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the tx st2110-41(fast metadata) session.
 * For ST41_TYPE_RTP_LEVEL.
 * Must call st41_tx_put_mbuf to return the mbuf after rtp pack done.
 *
 * @param handle
 *   The handle to the tx st2110-41(fast metadata) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st41_tx_get_mbuf(st41_tx_handle handle, void** usrptr);

/**
 * Put back the mbuf which get by st41_tx_get_mbuf to the tx st2110-41(fast metadata) session.
 * For ST41_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-41(fast metadata) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st41_tx_get_mbuf.
 * @param len
 *   the rtp package length, include both header and payload.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st41_tx_put_mbuf(st41_tx_handle handle, void* mbuf, uint16_t len);

/**
 * Create one rx st2110-41(fast metadata) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-41(fast metadata) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-41(fast metadata) session.
 */
st41_rx_handle st41_rx_create(mtl_handle mt, struct st41_rx_ops* ops);

/**
 * Online update the source info for the rx st2110-41(fast metadata) session.
 *
 * @param handle
 *   The handle to the rx st2110-41(fast metadata) session.
 * @param src
 *   The pointer to the rx st2110-41(fast metadata) source info.
 * @return
 *   - 0: Success, rx st2110-41(fast metadata) session source update succ.
 *   - <0: Error code of the rx st2110-41(fast metadata) session source update.
 */
int st41_rx_update_source(st41_rx_handle handle, struct st_rx_source_info* src);

/**
 * Free the rx st2110-41(fast metadata) session.
 *
 * @param handle
 *   The handle to the rx st2110-41(fast metadata) session.
 * @return
 *   - 0: Success, rx st2110-41(fast metadata) session freed.
 *   - <0: Error code of the rx st2110-41(fast metadata) session free.
 */
int st41_rx_free(st41_rx_handle handle);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the rx st2110-41(fast metadata) session.
 * For ST41_TYPE_RTP_LEVEL.
 * Must call st41_rx_put_mbuf to return the mbuf after consume it.
 *
 * @param handle
 *   The handle to the tx st2110-41(fast metadata) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @param len
 *   The length of the rtp packet, include both the header and payload.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st41_rx_get_mbuf(st41_rx_handle handle, void** usrptr, uint16_t* len);

/**
 * Put back the mbuf which get by st41_rx_get_mbuf to the rx st2110-41(fast metadata) session.
 * For ST41_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-41(fast metadata) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st41_rx_get_mbuf.
 */
void st41_rx_put_mbuf(st41_rx_handle handle, void* mbuf);

/**
 * Get the queue meta attached to rx st2110-41(fast metadata) session.
 *
 * @param handle
 *   The handle to the rx st2110-41(fast metadata) session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st41_rx_get_queue_meta(st41_rx_handle handle, struct st_queue_meta* meta);

#if defined(__cplusplus)
}
#endif

#endif
