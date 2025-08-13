/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st30_api.h
 *
 * Interfaces for st2110-30 transport.
 *
 */

#include "st_api.h"

#ifndef _ST30_API_HEAD_H_
#define _ST30_API_HEAD_H_

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
#define ST30_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st30_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST30_TX_FLAG_USER_R_MAC (MTL_BIT32(1))

/**
 * Flag bit in flags of struct st30_tx_ops.
 * User control the frame pacing by pass a timestamp in st30_tx_frame_meta,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST30_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st30_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * st30_tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST30_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st30_tx_ops.
 * Control frame pacing in the build stage also.
 */
#define ST30_TX_FLAG_BUILD_PACING (MTL_BIT32(5))
/**
 * Flag bit in flags of struct st30_tx_ops.
 * If enable the rtcp.
 */
#define ST30_TX_FLAG_ENABLE_RTCP (MTL_BIT32(6))
/**
 * Flag bit in flags of struct st30_tx_ops.
 * If use dedicated queue for TX.
 */
#define ST30_TX_FLAG_DEDICATE_QUEUE (MTL_BIT32(7))
/** Force the numa of the created session, both CPU and memory */
#define ST30_TX_FLAG_FORCE_NUMA (MTL_BIT32(8))

/**
 * Flag bit in flags of struct st30_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st30_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST30_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st30_rx_ops.
 * If enable the rtcp.
 */
#define ST30_RX_FLAG_ENABLE_RTCP (MTL_BIT32(1))
/** Force the numa of the created session, both CPU and memory */
#define ST30_RX_FLAG_FORCE_NUMA (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st30_rx_ops.
 * Enable the timing analyze in the stat dump
 */
#define ST30_RX_FLAG_TIMING_PARSER_STAT (MTL_BIT32(16))
/**
 * Flag bit in flags of struct st30_rx_ops.
 * Enable the timing analyze info by st30_rx_tp_meta of notify_timing_parser_result
 */
#define ST30_RX_FLAG_TIMING_PARSER_META (MTL_BIT32(17))

/** default time in the fifo between packet builder and pacing */
#define ST30_TX_FIFO_DEFAULT_TIME_MS (10)

/**
 * Payload format of st2110-30/31(audio) streaming
 * Note: PCM format is interpreted as big endian.
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
 * st30 tx pacing way
 */
enum st30_tx_pacing_way {
  /** auto detected pacing */
  ST30_TX_PACING_WAY_AUTO = 0,
  /** rate limit based pacing */
  ST30_TX_PACING_WAY_RL,
  /** tsc based pacing */
  ST30_TX_PACING_WAY_TSC,
  /** Max value of this enum */
  ST30_TX_PACING_WAY_MAX,
};

/**
 * A structure describing a AM824 subframe
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st31_am824 {
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
  /** data in am824 */
  uint8_t data[3];
});
#else
MTL_PACK(struct st31_am824 {
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
  /** data in am824 */
  uint8_t data[3];
});
#endif

/**
 * A structure describing a AES3 subframe
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st31_aes3 {
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
});
#else
MTL_PACK(struct st31_aes3 {
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
});
#endif

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
  /** epoch */
  uint64_t epoch;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
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
  /** TAI timestamp measured right after the RTP packet for this frame was received */
  uint64_t timestamp_first_pkt;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /** received data size for current frame */
  size_t frame_recv_size;
};

/**
 * st30 rx timing parser meta for every 200ms
 *
 * dpvr: Delta Packet vs RTP, us.
 * ipt: Inter-packet time, ns
 * tsdf: Timestamped Delay Factor, us.
 */
struct st30_rx_tp_meta {
  /** the max of dpvr(us) for current report period */
  int32_t dpvr_max;
  /** the min of dpvr(us) for current report period */
  int32_t dpvr_min;
  /** the average of dpvr(us) for current report period */
  float dpvr_avg;
  /** the max of ipt(ns) for current report period */
  int32_t ipt_max;
  /** the min of ipt(ns) for current report period */
  int32_t ipt_min;
  /** the average of ipt(ns) for current report period */
  float ipt_avg;
  /** the tsdf(us) for current report period */
  int32_t tsdf;

  /** RX timing parser compliant result */
  enum st_rx_tp_compliant compliant;
  /** the failed cause if compliant is not ST_RX_TP_COMPLIANT_NARROW */
  char failed_cause[64];

  /* packets count in current report period */
  uint32_t pkts_cnt;
};

/**
 * The structure describing how to create a tx st2110-30(audio) session.
 * Include the PCIE port and other required info
 */
struct st30_tx_ops {
  /** Mandatory. destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. Session payload format */
  enum st30_fmt fmt;
  /** Mandatory. Session channel number */
  uint16_t channel;
  /** Mandatory. Session sampling rate */
  enum st30_sampling sampling;
  /** Mandatory. Session packet time */
  enum st30_ptime ptime;
  /** Mandatory. Session streaming type, frame or RTP */
  enum st30_type type;
  /** Mandatory. 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /** Optional. The pacing engine */
  enum st30_tx_pacing_way pacing_way;
  /** Optional. Synchronization source defined in RFC3550, if zero the session will assign
   * a random value */
  uint32_t ssrc;
  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. see ST30_TX_FLAG_* for possible flags */
  uint32_t flags;

  /**
   * Mandatory for ST30_TYPE_FRAME_LEVEL.
   * the frame buffer count requested for one st30 tx session,
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST30_TYPE_FRAME_LEVEL.
   * size for each frame buffer, should be multiple of packet size(st30_get_packet_size),
   */
  uint32_t framebuff_size;
  /**
   * Mandatory for ST30_TYPE_FRAME_LEVEL. the callback when lib require a new frame for
   * sending. User should provide the next available frame index to next_frame_idx. It
   * implicit means the frame ownership will be transferred to lib, And only non-block
   * method can be used in this callback as it run from lcore tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st30_tx_frame_meta* meta);
  /**
   * Optional for ST30_TYPE_FRAME_LEVEL. The callback when lib finish sending a frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st30_tx_frame_meta* meta);

  /**
   * Optional. Callback triggered when a frame epoch is omitted/skipped in the lib.
   * This occurs when the transmission timing falls behind schedule and an epoch
   * must be skipped to maintain synchronization. (or in the user pacing mode
   * when the user time is behind the lib sending time).
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);

  /*
   * Optional. The size of fifo ring which used between the packet builder and pacing.
   * Leave to zero to use default value: the packet number within
   * ST30_TX_FIFO_DEFAULT_TIME_MS.
   */
  uint16_t fifo_size;
  /** Optional. UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /**
   * Optional. tx destination mac address.
   * Valid if ST30_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /**
   * Optional. The rtp timestamp delta(us) to the start time of frame.
   * Zero means the rtp timestamp at the start of the frame.
   */
  int32_t rtp_timestamp_delta_us;

  /** Mandatory for ST30_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional for ST30_TYPE_RTP_LEVEL. the callback when lib finish the sending of one rtp
   * packet. And only non-block method can be used in this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_rtp_done)(void* priv);
  /** Optional for ST30_TX_PACING_WAY_RL, the required accuracy for warmup check point */
  uint32_t rl_accuracy_ns;
  /** Optional for ST30_TX_PACING_WAY_RL, the offset time(us) for warmup check point */
  int32_t rl_offset_ns;
  /**  Use this socket if ST30_TX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;

  /**
   * size for each sample group,
   * use st30_get_sample_size to get the size for different format.
   */
  uint16_t sample_size __mtl_deprecated_msg("Not use anymore, plan to remove");
  /**
   * number of samples for single channel in packet,
   * use st30_get_sample_num to get the number from different ptime and sampling rate.
   */
  uint16_t sample_num __mtl_deprecated_msg("Not use anymore, plan to remove");
};

/**
 * The structure describing how to create a rx st2110-30(audio) session
 * Include the PCIE port and other required info
 */
struct st30_rx_ops {
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

  /** Mandatory. Session PCM format */
  enum st30_fmt fmt;
  /** Mandatory. Session channel number */
  uint16_t channel;
  /** Mandatory. Session sampling rate */
  enum st30_sampling sampling;
  /** Mandatory. Session packet time */
  enum st30_ptime ptime;
  /** Mandatory. Session streaming type, frame or RTP */
  enum st30_type type;
  /** Mandatory. 7 bits payload type define in RFC3550. Zero means disable the
   * payload_type check on the RX pkt path */
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
  /** Optional. see ST30_RX_FLAG_* for possible flags */
  uint32_t flags;

  /**
   * Mandatory for ST30_TYPE_FRAME_LEVEL.
   * the frame buffer count requested for one st30 rx session,
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST30_TYPE_FRAME_LEVEL.
   * size for each frame buffer, should be multiple of packet size(st30_get_packet_size),
   */
  uint32_t framebuff_size;
  /**
   * Mandatory for ST30_TYPE_FRAME_LEVEL. callback when lib receive one full frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st30_rx_put_framebuff
   * to return the frame when it finish the handling
   *   < 0: the error code if app can't handle, lib will call st30_rx_put_framebuff then.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void* priv, void* frame, struct st30_rx_frame_meta* meta);

  /** Mandatory for ST30_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional for ST20_TYPE_RTP_LEVEL. The callback when lib receive one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
  /**
   * Mandatory for ST30_RX_FLAG_TIMING_PARSER_META. The callback to notify the rx timing
   * parser result for every 200ms . And only non-block method can be used in this
   * callback as it run from lcore tasklet routine.
   */
  int (*notify_timing_parser_result)(void* priv, enum mtl_session_port port,
                                     struct st30_rx_tp_meta* tp);
  /**  Use this socket if ST30_RX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;

  /**
   * size for each sample group,
   * use st30_get_sample_size to get the size for different format.
   */
  uint16_t sample_size __mtl_deprecated_msg("Not use anymore, plan to remove");
  /**
   * number of samples for single channel in packet,
   * use st30_get_sample_num to get the number from different ptime and sampling rate.
   */
  uint16_t sample_num __mtl_deprecated_msg("Not use anymore, plan to remove");
};

/**
 * A structure used to retrieve general statistics(I/O) for a st30 tx session.
 */
struct st30_tx_user_stats {
  struct st_tx_user_stats common;
  uint64_t stat_epoch_mismatch;
  uint64_t stat_epoch_late;
  uint64_t stat_recoverable_error;
  uint64_t stat_unrecoverable_error;
  uint64_t stat_pkts_burst;
  uint64_t stat_pad_pkts_burst;
  uint64_t stat_warmup_pkts_burst;
  uint64_t stat_mismatch_sync_point;
  uint64_t stat_recalculate_warmup;
  uint64_t stat_hit_backup_cp;
};

/**
 * A structure used to retrieve general statistics(I/O) for a st30 rx session.
 */
struct st30_rx_user_stats {
  struct st_rx_user_stats common;
  uint64_t stat_pkts_redundant;
  uint64_t stat_pkts_dropped;
  uint64_t stat_pkts_len_mismatch_dropped;
  uint64_t stat_slot_get_frame_fail;
};

/**
 * Retrieve the general statistics(I/O) for one tx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30_tx_get_session_stats(st30_tx_handle handle, struct st30_tx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one tx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30_tx_reset_session_stats(st30_tx_handle handle);

/**
 * Retrieve the general statistics(I/O) for one rx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30_rx_get_session_stats(st30_rx_handle handle, struct st30_rx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the rx st2110-30(audio) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st30_rx_reset_session_stats(st30_rx_handle handle);

/**
 * Create one tx st2110-30(audio) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-30(audio) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-30(audio) session.
 */
st30_tx_handle st30_tx_create(mtl_handle mt, struct st30_tx_ops* ops);

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
 * Online update the destination info for the tx st2110-30(audio) session.
 *
 * @param handle
 *   The handle to the tx st2110-30(audio) session.
 * @param dst
 *   The pointer to the tx st2110-30(audio) destination info.
 * @return
 *   - 0: Success, tx st2110-30(audio) session destination update succ.
 *   - <0: Error code of the rx st2110-30(audio) session destination update.
 */
int st30_tx_update_destination(st30_tx_handle handle, struct st_tx_dest_info* dst);

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
 *   - NULL if no available mbuf in the ring.
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
 * Retrieve the size for each pkt.
 *
 * @param fmt
 *   The st2110-30(audio) format.
 * @param ptime
 *   The st2110-30(audio) packet time.
 * @param sampling
 *   The st2110-30(audio) sampling rate.
 * @param channel
 *   The st2110-30(audio) channel.
 * @return
 *   - >0 the clock rate.
 *   - <0: Error code if fail.
 */
int st30_get_packet_size(enum st30_fmt fmt, enum st30_ptime ptime,
                         enum st30_sampling sampling, uint16_t channel);

/**
 * Helper tp calculate the frame buffer size for one desired frame time.
 *
 * @param fmt
 *   The st2110-30(audio) format.
 * @param ptime
 *   The st2110-30(audio) packet time.
 * @param sampling
 *   The st2110-30(audio) sampling rate.
 * @param channel
 *   The st2110-30(audio) channel.
 * @param desired_frame_time_ns
 *   The desired frame time in ns.
 * @param fps
 *   The fps return for current frame rate based on desired_frame_time_ns. Leave to NULL
 * if not want to know the fps.
 * @return
 *   - >0 the frame buffer size returned.
 *   - <0: Error code if fail.
 */
int st30_calculate_framebuff_size(enum st30_fmt fmt, enum st30_ptime ptime,
                                  enum st30_sampling sampling, uint16_t channel,
                                  uint64_t desired_frame_time_ns, double* fps);

/**
 * Create one rx st2110-30(audio) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx st2110-30(audio) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-30(audio) session.
 */
st30_rx_handle st30_rx_create(mtl_handle mt, struct st30_rx_ops* ops);

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
 *   - NULL if no available mbuf in the ring.
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
