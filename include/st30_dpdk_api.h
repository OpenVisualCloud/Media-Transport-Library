/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st30_dpdk_api.h
 *
 * Interfaces to Media Transport Library for st2110-30 transport.
 *
 */

#include <st_dpdk_api.h>

#ifndef _ST30_DPDK_API_HEAD_H_
#define _ST30_DPDK_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Handle to tx st2110-30(audio) session
 */
typedef struct st_tx_audio_session_handle_impl* st30_tx_handle;
/**
 * Handle to rx st2110-30(audio) session
 */
typedef struct st_rx_audio_session_handle_impl* st30_rx_handle;

/**
 * Flag bit in flags of struct st30_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST30_TX_FLAG_USER_P_MAC (ST_BIT32(0))
/**
 * Flag bit in flags of struct st30_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST30_TX_FLAG_USER_R_MAC (ST_BIT32(1))

/**
 * Flag bit in flags of struct st30_tx_ops.
 * User control the frame timing by pass a timestamp in st30_tx_frame_meta,
 * lib will wait until timestamp is reached.
 */
#define ST30_TX_FLAG_USER_TIMESTAMP (ST_BIT32(3))

/**
 * Flag bit in flags of struct st30_rx_ops, for non ST_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and muticast join/drop.
 * Use st30_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST30_RX_FLAG_DATA_PATH_ONLY (ST_BIT32(0))

/**
 * Payload format of st2110-30/31(audio) streaming
 */
enum st30_fmt {
  ST30_FMT_PCM8 = 0, /**< 8 bits per channel */
  ST30_FMT_PCM16,    /**< 16 bits per channel */
  ST30_FMT_PCM24,    /**< 24 bits per channel */
  ST31_FMT_AM824,    /**< 32 bits per channel */
  ST30_FMT_MAX,      /**< max value of this enum */
};

/**
 * Sampling rate of st2110-30/31(audio) streaming
 */
enum st30_sampling {
  ST30_SAMPLING_48K = 0, /**< sampling rate of 48kHz */
  ST30_SAMPLING_96K,     /**< sampling rate of 96kHz */
  ST31_SAMPLING_44K,     /**< sampling rate of 44.1kHz */
  ST30_SAMPLING_MAX,     /**< max value of this enum */
};

/**
 * Packet time period of st2110-30/31(audio) streaming
 */
enum st30_ptime {
  ST30_PTIME_1MS = 0, /**< packet time of 1ms */
  ST30_PTIME_125US,   /**< packet time of 125us */
  ST30_PTIME_250US,   /**< packet time of 250us */
  ST30_PTIME_333US,   /**< packet time of 333us */
  ST30_PTIME_4MS,     /**< packet time of 4ms */
  ST31_PTIME_80US,    /**< packet time of 80us */
  ST31_PTIME_1_09MS,  /**< packet time of 1.09ms, only for 44.1kHz sample */
  ST31_PTIME_0_14MS,  /**< packet time of 0.14ms, only for 44.1kHz sample */
  ST31_PTIME_0_09MS,  /**< packet time of 0.09ms, only for 44.1kHz sample */
  ST30_PTIME_MAX,     /**< max value of this enum */
};

/**
 * Session type of st2110-30(audio) streaming
 */
enum st30_type {
  ST30_TYPE_FRAME_LEVEL = 0, /**< app interface lib based on frame level */
  ST30_TYPE_RTP_LEVEL,       /**< app interface lib based on RTP level */
  ST30_TYPE_MAX,             /**< max value of this enum */
};

/**
 * A structure describing a AM824 subframe
 */
struct st31_am824 {
#ifdef ST_LITTLE_ENDIAN
  /** v bit in am824 */
  uint8_t v : 1;
  /** u bit in am824 */
  uint8_t u : 1;
  /** c bit in am824 */
  uint8_t c : 1;
  /** p bit in am824 */
  uint8_t p : 1;
  /** f bit in am824 */
  uint8_t f : 1;
  /** b bit in am824 */
  uint8_t b : 1;
  /** unused 2 bits in am824 */
  uint8_t unused : 2;
#else
  /** unused 2 bits in am824 */
  uint8_t unused : 2;
  /** b bit in am824 */
  uint8_t b : 1;
  /** f bit in am824 */
  uint8_t f : 1;
  /** p bit in am824 */
  uint8_t p : 1;
  /** c bit in am824 */
  uint8_t c : 1;
  /** u bit in am824 */
  uint8_t u : 1;
  /** v bit in am824 */
  uint8_t v : 1;
#endif
  /** data in am824 */
  uint8_t data[3];
} __attribute__((__packed__));

/**
 * A structure describing a AES3 subframe
 */
struct st31_aes3 {
#ifdef ST_LITTLE_ENDIAN
  /** preamble in aes3 */
  uint8_t preamble : 4;
  /** data_0 in aes3 */
  uint8_t data_0 : 4;
  /** data_1 in aes3 */
  uint16_t data_1;
  /** data_2 in aes3 */
  uint8_t data_2 : 4;
  /** v bit in aes3 */
  uint8_t v : 1;
  /** u bit in aes3 */
  uint8_t u : 1;
  /** c bit in aes3 */
  uint8_t c : 1;
  /** p bit in aes3 */
  uint8_t p : 1;
#else
  /** data_0 in aes3 */
  uint8_t data_0 : 4;
  /** preamble in aes3 */
  uint8_t preamble : 4;
  /** data_1 in aes3 */
  uint16_t data_1;
  /** p bit in aes3 */
  uint8_t p : 1;
  /** c bit in aes3 */
  uint8_t c : 1;
  /** u bit in aes3 */
  uint8_t u : 1;
  /** v bit in aes3 */
  uint8_t v : 1;
  /** data_2 in aes3 */
  uint8_t data_2 : 4;
#endif
} __attribute__((__packed__));

/**
 * Frame meta data of st2110-30(audio) tx streaming
 */
struct st30_tx_frame_meta {
  /** Session payload format */
  enum st30_fmt fmt;
  /** Session channel number */
  uint16_t channel;
  /** Session sampling rate */
  enum st30_sampling sampling;
  /** Session packet time */
  enum st30_ptime ptime;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
};

/**
 * Frame meta data of st2110-30(audio) rx streaming
 */
struct st30_rx_frame_meta {
  /** Frame format */
  enum st30_fmt fmt;
  /** Frame sampling type */
  enum st30_sampling sampling;
  /** Frame channel number */
  uint16_t channel;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
};

/**
 * The structure describing how to create a tx st2110-30(audio) session.
 * Include the PCIE port and other required info
 */
struct st30_tx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** destination IP address */
  uint8_t dip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];

  /** Session payload format */
  enum st30_fmt fmt;
  /** Session channel number */
  uint16_t channel;
  /** Session sampling rate */
  enum st30_sampling sampling;
  /** Session packet time */
  enum st30_ptime ptime;
  /** Session streaming type, frame or RTP */
  enum st30_type type;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /**
   * size for each sample group,
   * use st30_get_sample_size to get the size for different format.
   */
  uint16_t sample_size;
  /**
   * number of samples for single channel in packet,
   * use st30_get_sample_num to get the number from different ptime and sampling rate.
   */
  uint16_t sample_num;
  /** flags, value in ST30_TX_FLAG_* */
  uint32_t flags;
  /**
   * tx destination mac address.
   * Valid if ST30_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[ST_PORT_MAX][6];

  /**
   * the frame buffer count requested for one st30 tx session,
   * only for ST30_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * size for each frame buffer, should be multiple of sample_size,
   * only for ST30_TYPE_FRAME_LEVEL.
   */
  uint32_t framebuff_size;
  /**
   * ST30_TYPE_FRAME_LEVEL callback when lib require a new frame.
   * User should provide the next avaiable frame index to next_frame_idx.
   * It implicit means the frame ownership will be transferred to lib,
   * only for ST30_TYPE_FRAME_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st30_tx_frame_meta* meta);
  /**
   * ST30_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * only for ST30_TYPE_FRAME_LEVEL,
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st30_tx_frame_meta* meta);

  /**
   * rtp ring size, must be power of 2.
   * only for ST30_TYPE_RTP_LEVEL
   */
  uint32_t rtp_ring_size;
  /**
   * ST30_TYPE_RTP_LEVEL callback when lib consume one rtp packet,
   * only for ST30_TYPE_RTP_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_done)(void* priv);
};

/**
 * The structure describing how to create a rx st2110-30(audio) session
 * Include the PCIE port and other required info
 */
struct st30_rx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** source IP address of sender */
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];
  /** flags, value in ST30_RX_FLAG_* */
  uint32_t flags;

  /** Session PCM format */
  enum st30_fmt fmt;
  /** Session channel number */
  uint16_t channel;
  /** Session sampling rate */
  enum st30_sampling sampling;
  /** Session packet time */
  enum st30_ptime ptime;
  /** Session streaming type, frame or RTP */
  enum st30_type type;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /**
   * size for each sample group,
   * use st30_get_sample_size to get the size for different format.
   */
  uint16_t sample_size;
  /**
   * number of samples for single channel in packet,
   * use st30_get_sample_num to get the number from different ptime and sampling rate.
   */
  uint16_t sample_num;

  /**
   * the frame buffer count requested for one st30 rx session,
   * only for ST30_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * size for each frame buffer, should be multiple of sample_size,
   * only for ST30_TX_TYPE_FRAME_LEVEL.
   */
  uint32_t framebuff_size;
  /**
   * ST30_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st30_rx_put_framebuff
   * to return the frame when it finish the handling
   *   < 0: the error code if app cann't handle, lib will free the frame then
   * the consume of frame.
   * only for ST30_TX_TYPE_FRAME_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void* priv, void* frame, struct st30_rx_frame_meta* meta);

  /**
   * rtp ring size, must be power of 2,
   * only for ST30_TYPE_RTP_LEVEL
   */
  uint32_t rtp_ring_size;
  /**
   * ST30_TYPE_RTP_LEVEL callback when lib receive one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
};

/**
 * Create one tx st2110-30(audio) session.
 *
 * @param st
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-30(audio) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-30(audio) session.
 */
st30_tx_handle st30_tx_create(st_handle st, struct st30_tx_ops* ops);

/**
 * Free the tx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @return
 *   - 0: Success, tx st2110-30(audio) session freed.
 *   - <0: Error code of the tx st2110-30(audio) session free.
 */
int st30_tx_free(st30_tx_handle handle);

/**
 * Get the framebuffer pointer from the tx st2110-30(audio) session.
 * For ST30_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st30_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st30_tx_get_framebuffer(st30_tx_handle handle, uint16_t idx);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the tx st2110-30(audio) session.
 * For ST30_TYPE_RTP_LEVEL.
 * Must call st30_tx_put_mbuf to return the mbuf after rtp pack done.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @return
 *   - NULL if no avaiable mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st30_tx_get_mbuf(st30_tx_handle handle, void** usrptr);

/**
 * Put back the mbuf which get by st30_tx_get_mbuf to the tx st2110-30(audio) session.
 * For ST30_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st30_tx_get_mbuf.
 * @param len
 *   the rtp package length, include both header and payload.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st30_tx_put_mbuf(st30_tx_handle handle, void* mbuf, uint16_t len);

/**
 * Retrieve the packet time in nanoseconds from st2110-30(audio) ptime.
 *
 * @param ptime
 *   The st2110-30(audio) ptime.
 * @return
 *   - >0 the packet time in nanoseconds.
 *   - <0: Error code if fail.
 */
double st30_get_packet_time(enum st30_ptime ptime);

/**
 * Retrieve the sample data size from from st2110-30(audio) format.
 *
 * @param fmt
 *   The st2110-30(audio) format.
 * @return
 *   - >0 the sample data size.
 *   - <0: Error code if fail.
 */
int st30_get_sample_size(enum st30_fmt fmt);

/**
 * Retrieve the number of samples in packet from
 * st2110-30(audio) packet time and sampling rate.
 *
 * @param ptime
 *   The st2110-30(audio) packet time.
 * @param sampling
 *   The st2110-30(audio) sampling rate.
 * @return
 *   - >0 the sample num in packet.
 *   - <0: Error code if fail.
 */
int st30_get_sample_num(enum st30_ptime ptime, enum st30_sampling sampling);

/**
 * Retrieve the sampling clock rate.
 *
 * @param sampling
 *   The st2110-30(audio) sampling rate.
 * @return
 *   - >0 the clock rate.
 *   - <0: Error code if fail.
 */
int st30_get_sample_rate(enum st30_sampling sampling);

/**
 * Create one rx st2110-30(audio) session.
 *
 * @param st
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx st2110-30(audio) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-30(audio) session.
 */
st30_rx_handle st30_rx_create(st_handle st, struct st30_rx_ops* ops);

/**
 * Online update the source info for the rx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @param src
 *   The pointer to the rx st2110-30(audio) source info.
 * @return
 *   - 0: Success, rx st2110-30(audio) session source update succ.
 *   - <0: Error code of the rx st2110-30(audio) session source update.
 */
int st30_rx_update_source(st30_rx_handle handle, struct st_rx_source_info* src);

/**
 * Free the rx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @return
 *   - 0: Success, rx st2110-30(audio) session freed.
 *   - <0: Error code of the rx st2110-30(audio) session free.
 */
int st30_rx_free(st30_rx_handle handle);

/**
 * Put back the received buff get from notify_frame_ready.
 * For ST30_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @param frame
 *   The framebuffer pointer.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st30_rx_put_framebuff(st30_rx_handle handle, void* frame);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the rx st2110-30(audio) session.
 * For ST30_TYPE_RTP_LEVEL.
 * Must call st30_rx_put_mbuf to return the mbuf after consume it.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @param len
 *   The length of the rtp packet, include both the header and payload.
 * @return
 *   - NULL if no avaiable mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st30_rx_get_mbuf(st30_rx_handle handle, void** usrptr, uint16_t* len);

/**
 * Put back the mbuf which get by st30_rx_get_mbuf to the rx st2110-30(audio) session.
 * For ST30_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st30_rx_get_mbuf.
 */
void st30_rx_put_mbuf(st30_rx_handle handle, void* mbuf);

/**
 * Get the queue meta attached to rx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st30_rx_get_queue_meta(st30_rx_handle handle, struct st_queue_meta* meta);

#if defined(__cplusplus)
}
#endif

#endif
