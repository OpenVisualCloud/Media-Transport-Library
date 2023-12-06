/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st40_api.h
 *
 * Interfaces for st2110-40 transport.
 *
 */

#include "st_api.h"

#ifndef _ST40_API_HEAD_H_
#define _ST40_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Handle to tx st2110-40(ancillary) session
 */
typedef struct st_tx_ancillary_session_handle_impl* st40_tx_handle;
/**
 * Handle to rx st2110-40(ancillary) session
 */
typedef struct st_rx_ancillary_session_handle_impl* st40_rx_handle;

/**
 * Flag bit in flags of struct st40_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST40_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st40_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST40_TX_FLAG_USER_R_MAC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st40_tx_ops.
 * User control the frame pacing by pass a timestamp in st40_tx_frame_meta,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST40_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st40_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * st40_tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST40_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st40_tx_ops.
 * If enable the rtcp.
 */
#define ST40_TX_FLAG_ENABLE_RTCP (MTL_BIT32(5))

/**
 * Flag bit in flags of struct st30_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st40_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST40_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st40_rx_ops.
 * If enable the rtcp.
 */
#define ST40_RX_FLAG_ENABLE_RTCP (MTL_BIT32(1))

/**
 * Session type of st2110-40(ancillary) streaming
 */
enum st40_type {
  ST40_TYPE_FRAME_LEVEL = 0, /**< app interface lib based on frame level */
  ST40_TYPE_RTP_LEVEL,       /**< app interface lib based on RTP level */
  ST40_TYPE_MAX,             /**< max value of this enum */
};

/**
 * A structure describing a st2110-40(ancillary) rfc8331 rtp header
 */
MTL_PACK(struct st40_rfc8331_rtp_hdr {
  /** Rtp rfc3550 base hdr */
  struct st_rfc3550_rtp_hdr base;
  /** Extended Sequence Number */
  uint16_t seq_number_ext;
  /** Number of octets of the ANC data RTP payload */
  uint16_t length;

  struct {
    /** the count of the total number of ANC data packets carried in the RTP payload */
    uint32_t anc_count : 8;
    /** signaling the field specified by the RTP timestamp in an interlaced SDI raster */
    uint32_t f : 2;
    /** reserved */
    uint32_t reserved : 22;
  };
});

/**
 * A structure describing a st2110-40(ancillary) rfc8331 payload header
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st40_rfc8331_payload_hdr {
  union {
    struct {
      /** the source data stream number of the ANC data packet */
      uint32_t stream_num : 7;
      /** whether the data stream number of a multi-stream data mapping */
      uint32_t s : 1;
      /** the location of the ANC data packet in the SDI raster */
      uint32_t horizontal_offset : 12;
      /** line number corresponds to the location (vertical) of the ANC data packet */
      uint32_t line_number : 11;
      /** the ANC data uses luma (Y) data channel */
      uint32_t c : 1;
    } first_hdr_chunk;
    /** Handle to make operating on first_hdr_chunk buffer easier */
    uint32_t swaped_first_hdr_chunk;
  };
  union {
    struct {
      /** Starting point of the UDW (user data words) */
      uint32_t rsvd_for_udw : 2;
      /** Data Count */
      uint32_t data_count : 10;
      /** Secondary Data Identification Word */
      uint32_t sdid : 10;
      /** Data Identification Word */
      uint32_t did : 10;
    } second_hdr_chunk;
    /** Handle to make operating on second_hdr_chunk buffer easier */
    uint32_t swaped_second_hdr_chunk;
  };
});
#else
MTL_PACK(struct st40_rfc8331_payload_hdr {
  union {
    struct {
      /** the ANC data uses luma (Y) data channel */
      uint32_t c : 1;
      /** line number corresponds to the location (vertical) of the ANC data packet */
      uint32_t line_number : 11;
      /** the location of the ANC data packet in the SDI raster */
      uint32_t horizontal_offset : 12;
      /** whether the data stream number of a multi-stream data mapping */
      uint32_t s : 1;
      /** the source data stream number of the ANC data packet */
      uint32_t stream_num : 7;
    } first_hdr_chunk;
    /** Handle to make operating on first_hdr_chunk buffer easier */
    uint32_t swaped_first_hdr_chunk;
  };
  union {
    struct {
      /** Data Identification Word */
      uint32_t did : 10;
      /** Secondary Data Identification Word */
      uint32_t sdid : 10;
      /** Data Count */
      uint32_t data_count : 10;
      /** Starting point of the UDW (user data words) */
      uint32_t rsvd_for_udw : 2;
    } second_hdr_chunk;
    /** Handle to make operating on second_hdr_chunk buffer easier */
    uint32_t swaped_second_hdr_chunk;
  };
});
#endif

/**
 * Structure for ST2110-40(ancillary) meta
 */
struct st40_meta {
  /** the ANC data uses luma (Y) data channel */
  uint16_t c;
  /** line number corresponds to the location (vertical) of the ANC data packet */
  uint16_t line_number;
  /** the location of the ANC data packet in the SDI raster */
  uint16_t hori_offset;
  /** whether the data stream number of a multi-stream data mapping */
  uint16_t s;
  /** the source data stream number of the ANC data packet */
  uint16_t stream_num;
  /** Data Identification Word */
  uint16_t did;
  /** Secondary Data Identification Word */
  uint16_t sdid;
  /** Size of the User Data Words  */
  uint16_t udw_size;
  /** Offset of the User Data Words  */
  uint16_t udw_offset;
};

/**
 * Max number of meta in one ST2110-40(ancillary) frame
 */
#define ST40_MAX_META (20)

/**
 * Structure for ST2110-40(ancillary) frame
 */
struct st40_frame {
  struct st40_meta meta[ST40_MAX_META]; /**<  Meta data  */
  uint8_t* data;                        /**<  Handle to data buffer  */
  uint32_t data_size;                   /**<  Size of content data  */
  uint32_t meta_num;                    /**<  number of meta data  */
};

/**
 * Frame meta data of st2110-40(ancillary) tx streaming
 */
struct st40_tx_frame_meta {
  /** Frame fps */
  enum st_fps fps;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
  /** epoch */
  uint64_t epoch;
};

/**
 * The structure describing how to create a tx st2110-40(ancillary) session.
 * Include the PCIE port and other required info.
 */
struct st40_tx_ops {
  /** Mandatory. destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. Session streaming type, frame or RTP */
  enum st40_type type;
  /** Mandatory. Session fps */
  enum st_fps fps;
  /** Mandatory. 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /** Optional. Synchronization source defined in RFC3550, if zero the session will assign
   * a random value */
  uint32_t ssrc;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST40_TX_FLAG_* for possible flags */
  uint32_t flags;

  /**
   * Mandatory for ST40_TYPE_FRAME_LEVEL.
   * the frame buffer count requested for one st40 tx session,
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST40_TYPE_FRAME_LEVEL. callback when lib require a new frame for
   * sending. User should provide the next available frame index to next_frame_idx. It
   * implicit means the frame ownership will be transferred to lib, And only non-block
   * method can be used in this callback as it run from lcore tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st40_tx_frame_meta* meta);
  /**
   * Optional for ST40_TYPE_FRAME_LEVEL. callback when lib finish sending one frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st40_tx_frame_meta* meta);

  /** Optional. UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /**
   * Optional. tx destination mac address.
   * Valid if ST40_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /** Mandatory for ST40_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional for ST40_TYPE_RTP_LEVEL. callback when lib finish the sending of one rtp
   * packet, And only non-block method can be used in this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_rtp_done)(void* priv);
};

/**
 * The structure describing how to create a rx st2110-40(ancillary) session.
 * Include the PCIE port and other required info
 */
struct st40_rx_ops {
  /** Mandatory. source IP address of sender or multicast IP address */
  uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. UDP dest port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Optional. Synchronization source defined in RFC3550, RX session will check the
   * incoming RTP packets match the ssrc. Leave to zero to disable the ssrc check */
  uint32_t ssrc;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST40_RX_FLAG_* for possible flags */
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
 * Create one tx st2110-40(ancillary) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-40(ancillary)
 * session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-40(ancillary) session.
 */
st40_tx_handle st40_tx_create(mtl_handle mt, struct st40_tx_ops* ops);

/**
 * Free the tx st2110-40(ancillary) session.
 *
 * @param handle
 *   The handle to the tx st2110-40(ancillary) session.
 * @return
 *   - 0: Success, tx st2110-40(ancillary) session freed.
 *   - <0: Error code of the tx st2110-40(ancillary) session free.
 */
int st40_tx_free(st40_tx_handle handle);

/**
 * Online update the destination info for the tx st2110-40(ancillary) session.
 *
 * @param handle
 *   The handle to the tx st2110-40(ancillary) session.
 * @param dst
 *   The pointer to the tx st2110-40(ancillary) destination info.
 * @return
 *   - 0: Success, tx st2110-40(ancillary) session destination update succ.
 *   - <0: Error code of the rx st2110-40(ancillary) session destination update.
 */
int st40_tx_update_destination(st40_tx_handle handle, struct st_tx_dest_info* dst);

/**
 * Get the framebuffer pointer from the tx st2110-40(ancillary) session.
 * For ST40_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-40(ancillary) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st40_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st40_tx_get_framebuffer(st40_tx_handle handle, uint16_t idx);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the tx st2110-40(ancillary) session.
 * For ST40_TYPE_RTP_LEVEL.
 * Must call st40_tx_put_mbuf to return the mbuf after rtp pack done.
 *
 * @param handle
 *   The handle to the tx st2110-40(ancillary) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st40_tx_get_mbuf(st40_tx_handle handle, void** usrptr);

/**
 * Put back the mbuf which get by st40_tx_get_mbuf to the tx st2110-40(ancillary) session.
 * For ST40_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-40(ancillary) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st40_tx_get_mbuf.
 * @param len
 *   the rtp package length, include both header and payload.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st40_tx_put_mbuf(st40_tx_handle handle, void* mbuf, uint16_t len);

/**
 * Create one rx st2110-40(ancillary) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-40(ancillary) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-40(ancillary) session.
 */
st40_rx_handle st40_rx_create(mtl_handle mt, struct st40_rx_ops* ops);

/**
 * Online update the source info for the rx st2110-40(ancillary) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(ancillary) session.
 * @param src
 *   The pointer to the rx st2110-40(ancillary) source info.
 * @return
 *   - 0: Success, rx st2110-40(ancillary) session source update succ.
 *   - <0: Error code of the rx st2110-40(ancillary) session source update.
 */
int st40_rx_update_source(st40_rx_handle handle, struct st_rx_source_info* src);

/**
 * Free the rx st2110-40(ancillary) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(ancillary) session.
 * @return
 *   - 0: Success, rx st2110-40(ancillary) session freed.
 *   - <0: Error code of the rx st2110-40(ancillary) session free.
 */
int st40_rx_free(st40_rx_handle handle);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the rx st2110-40(ancillary) session.
 * For ST40_TYPE_RTP_LEVEL.
 * Must call st40_rx_put_mbuf to return the mbuf after consume it.
 *
 * @param handle
 *   The handle to the tx st2110-40(ancillary) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @param len
 *   The length of the rtp packet, include both the header and payload.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st40_rx_get_mbuf(st40_rx_handle handle, void** usrptr, uint16_t* len);

/**
 * Put back the mbuf which get by st40_rx_get_mbuf to the rx st2110-40(ancillary) session.
 * For ST40_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-40(ancillary) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st40_rx_get_mbuf.
 */
void st40_rx_put_mbuf(st40_rx_handle handle, void* mbuf);

/**
 * Get the queue meta attached to rx st2110-40(ancillary) session.
 *
 * @param handle
 *   The handle to the rx st2110-40(ancillary) session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st40_rx_get_queue_meta(st40_rx_handle handle, struct st_queue_meta* meta);

/**
 * Get udw from from st2110-40(ancillary) payload.
 *
 * @param idx
 *   Index.
 * @param data
 *   The pointer to st2110-40 payload.
 * @return
 *   - udw
 */
uint16_t st40_get_udw(uint32_t idx, uint8_t* data);

/**
 * Set udw from for st2110-40(ancillary) payload.
 *
 * @param idx
 *   Index.
 * @param udw
 *   udw value to set.
 * @param data
 *   The pointer to st2110-40 payload.
 */
void st40_set_udw(uint32_t idx, uint16_t udw, uint8_t* data);

/**
 * Calculate checksum from st2110-40(ancillary) payload.
 *
 * @param data_num
 *   Index.
 * @param data
 *   The pointer to st2110-40 payload.
 * @return
 *   - checksum
 */
uint16_t st40_calc_checksum(uint32_t data_num, uint8_t* data);

/**
 * Add parity from st2110-40(ancillary) payload.
 *
 * @param val
 *   value.
 * @return
 *   - parity
 */
uint16_t st40_add_parity_bits(uint16_t val);

/**
 * Check parity for st2110-40(ancillary) payload.
 *
 * @param val
 *   value.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st40_check_parity_bits(uint16_t val);

#if defined(__cplusplus)
}
#endif

#endif
