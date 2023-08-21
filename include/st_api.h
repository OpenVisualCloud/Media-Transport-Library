/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_api.h
 *
 * Interfaces to media streaming(st2110) transport.
 *
 */

#include "mtl_api.h"

#ifndef _ST_API_HEAD_H_
#define _ST_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** 90k video sampling rate */
#define ST10_VIDEO_SAMPLING_RATE_90K (90 * 1000)
/** 48k audio sampling rate */
#define ST10_AUDIO_SAMPLING_RATE_48K (48 * 1000)
/** 44.1k audio sampling rate */
#define ST10_AUDIO_SAMPLING_RATE_44K (441 * 100)
/** 96k audio sampling rate */
#define ST10_AUDIO_SAMPLING_RATE_96K (96 * 1000)

/**
 * Timestamp type of st2110-10
 */
enum st10_timestamp_fmt {
  /** the media clock time in nanoseconds since the TAI epoch */
  ST10_TIMESTAMP_FMT_TAI = 0,
  /**
   * the raw media clock value defined in ST2110-10, whose units vary by essence
   * sampling rate(90k for video, 48K/96K for audio).
   */
  ST10_TIMESTAMP_FMT_MEDIA_CLK,
  /** max value of this enum */
  ST10_TIMESTAMP_FMT_MAX,
};

/**
 * FPS type of media streaming, Frame per second or Field per second
 */
enum st_fps {
  ST_FPS_P59_94 = 0, /**< 59.94 fps */
  ST_FPS_P50,        /**< 50 fps */
  ST_FPS_P29_97,     /**< 29.97 fps */
  ST_FPS_P25,        /**< 25 fps */
  ST_FPS_P119_88,    /**< 119.88 fps */
  ST_FPS_P120,       /**< 120 fps */
  ST_FPS_P100,       /**< 100 fps */
  ST_FPS_P60,        /**< 60 fps */
  ST_FPS_P30,        /**< 30 fps */
  ST_FPS_P24,        /**< 24 fps */
  ST_FPS_P23_98,     /**< 23.98 fps */
  ST_FPS_MAX,        /**< max value of this enum */
};

/**
 * Frame status type of rx streaming
 */
enum st_frame_status {
  /** All pixels of the frame were received */
  ST_FRAME_STATUS_COMPLETE = 0,
  /**
   * There was some packet loss, but the complete frame was reconstructed using packets
   * from primary and redundant streams
   */
  ST_FRAME_STATUS_RECONSTRUCTED,
  /** Packets were lost */
  ST_FRAME_STATUS_CORRUPTED,
  /** Max value of this enum */
  ST_FRAME_STATUS_MAX,
};

/**
 * A structure describing rfc3550 rtp header, size: 12
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st_rfc3550_rtp_hdr {
  /** CSRC count(CC) */
  uint8_t csrc_count : 4;
  /** extension(X) */
  uint8_t extension : 1;
  /** padding(P) */
  uint8_t padding : 1;
  /** version(V) */
  uint8_t version : 2;
  /** payload type(PT) */
  uint8_t payload_type : 7;
  /** marker(M) */
  uint8_t marker : 1;
  /** sequence number */
  uint16_t seq_number;
  /** timestamp */
  uint32_t tmstamp;
  /** synchronization source */
  uint32_t ssrc;
});

#else
MTL_PACK(struct st_rfc3550_rtp_hdr {
  /** version(V) */
  uint8_t version : 2;
  /** padding(P) */
  uint8_t padding : 1;
  /** extension(X) */
  uint8_t extension : 1;
  /** CSRC count(CC) */
  uint8_t csrc_count : 4;
  /** marker(M) */
  uint8_t marker : 1;
  /** payload type(PT) */
  uint8_t payload_type : 7;
  /** sequence number */
  uint16_t seq_number;
  /** timestamp */
  uint32_t tmstamp;
  /** synchronization source */
  uint32_t ssrc;
});
#endif

/**
 * The structure describing the destination address(ip addr and port) info for TX.
 * Leave redundant info to zero if the session only has primary port.
 */
struct st_tx_dest_info {
  /** destination IP address of sender */
  uint8_t dip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  /** UDP port number */
  uint16_t udp_port[MTL_PORT_MAX];
};

/**
 * The structure describing the source address(ip addr and port) info for RX.
 * Leave redundant info to zero if the session only has primary port.
 */
struct st_rx_source_info {
  /** source IP address of sender */
  uint8_t sip_addr[MTL_PORT_MAX][MTL_IP_ADDR_LEN];
  /** UDP port number */
  uint16_t udp_port[MTL_PORT_MAX];
};

/**
 * Pcap dump meta data for synchronous st**_rx_pcapng_dump.
 */
struct st_pcap_dump_meta {
  /** file path for the pcap dump file */
  char file_name[MTL_PCAP_FILE_MAX_LEN];
  /** number of packets dumped */
  uint32_t dumped_packets;
};

/**
 * The structure describing queue info attached to one session.
 */
struct st_queue_meta {
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** starting netdev queue id for afxdp */
  uint8_t start_queue[MTL_PORT_MAX];
  /** queue id this session attached to */
  uint8_t queue_id[MTL_PORT_MAX];
};

/**
 * Vsync callback meta data
 */
struct st10_vsync_meta {
  /** current vsync epoch */
  uint64_t epoch;
  /** current ptp time */
  uint64_t ptp;
  /** frame time in ns */
  double frame_time;
};

/**
 * Event type on a media session.
 */
enum st_event {
  /**
   * vsync(ptp come to a new epoch) event on each frame, freq is fps,
   * args point to struct st10_vsync_meta.
   */
  ST_EVENT_VSYNC = 0,
  /** the error occurred and session recovery successfully */
  ST_EVENT_RECOVERY_ERROR,
  /** fatal error and session can't recovery, app should free the session then */
  ST_EVENT_FATAL_ERROR,
  /** max value of this enum */
  ST_EVENT_MAX,
};

/**
 * A structure used to retrieve varied info for an media instance.
 */
struct st_var_info {
  /** st20 tx session count */
  uint16_t st20_tx_sessions_cnt;
  /** st22 tx session count */
  uint16_t st22_tx_sessions_cnt;
  /** st30 tx session count */
  uint16_t st30_tx_sessions_cnt;
  /** st40 tx session count */
  uint16_t st40_tx_sessions_cnt;
  /** st20 rx session count */
  uint16_t st20_rx_sessions_cnt;
  /** st22 rx session count */
  uint16_t st22_rx_sessions_cnt;
  /** st30 rx session count */
  uint16_t st30_rx_sessions_cnt;
  /** st40 rx session count */
  uint16_t st40_rx_sessions_cnt;
};

/**
 * Retrieve the varied info of the media transport device context.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param info
 *   A pointer to info structure.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_var_info(mtl_handle mt, struct st_var_info* info);

/**
 * Inline function to check the  rx frame is a completed frame.
 * @param status
 *   The input frame status.
 * @return
 *     Complete or not.
 */
static inline bool st_is_frame_complete(enum st_frame_status status) {
  if ((status == ST_FRAME_STATUS_COMPLETE) || (status == ST_FRAME_STATUS_RECONSTRUCTED))
    return true;
  else
    return false;
}

/**
 * Helper function returning accurate frame rate from enum st_fps
 * @param fps
 *   enum st_fps fps.
 * @return
 *   frame rate number
 */
double st_frame_rate(enum st_fps fps);

/**
 * Helper function returning enum st_fps from frame rate
 * @param framerate
 *   frame rate number
 * @return
 *   enum st_fps fps.
 */
enum st_fps st_frame_rate_to_st_fps(double framerate);

/**
 * Helper function to convert ST10_TIMESTAMP_FMT_TAI to ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * @param tai_ns
 *   time in nanoseconds since the TAI epoch.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate
 */
uint32_t st10_tai_to_media_clk(uint64_t tai_ns, uint32_t sampling_rate);

/**
 * Helper function to convert ST10_TIMESTAMP_FMT_MEDIA_CLK to nanoseconds.
 *
 * @param media_ts
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   time in nanoseconds.
 */
uint64_t st10_media_clk_to_ns(uint32_t media_ts, uint32_t sampling_rate);

/**
 * Helper function to get tai for both ST10_TIMESTAMP_FMT_TAI and
 * ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * @param tfmt
 *   timestamp fmt.
 * @param timestamp
 *   timestamp value with tfmt.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   time in nanoseconds since the TAI epoch.
 */
static inline uint64_t st10_get_tai(enum st10_timestamp_fmt tfmt, uint64_t timestamp,
                                    uint32_t sampling_rate) {
  if (tfmt == ST10_TIMESTAMP_FMT_TAI) return timestamp;
  return st10_media_clk_to_ns((uint32_t)timestamp, sampling_rate);
}

/**
 * Helper function to get media clock value defined in ST2110-10 for both
 * ST10_TIMESTAMP_FMT_TAI and ST10_TIMESTAMP_FMT_MEDIA_CLK.
 *
 * @param tfmt
 *   timestamp fmt.
 * @param timestamp
 *   timestamp value with tfmt.
 * @param sampling_rate
 *   sampling rate(90k for video, 48K/96K for audio).
 * @return
 *   the raw media clock value defined in ST2110-10, whose units vary by sampling_rate.
 */
static inline uint32_t st10_get_media_clk(enum st10_timestamp_fmt tfmt,
                                          uint64_t timestamp, uint32_t sampling_rate) {
  if (tfmt == ST10_TIMESTAMP_FMT_MEDIA_CLK) return (uint32_t)timestamp;
  return st10_tai_to_media_clk(timestamp, sampling_rate);
}

/**
 * Helper function to get tx queues cnt for st sessions.
 *
 * @param st20_sessions
 * st20 tx sessions count.
 * @param st30_sessions
 * st30 tx sessions count.
 * @param st30_sessions
 * st30 sessions count.
 * @return
 *   queues count.
 */
static inline uint16_t st_tx_sessions_queue_cnt(uint16_t st20_sessions,
                                                uint16_t st30_sessions,
                                                uint16_t st40_sessions) {
  uint16_t queues = st20_sessions;
  if (st30_sessions) queues++;
  if (st40_sessions) queues++;
  return queues;
}

/**
 * Helper function to get tx queues cnt for st sessions.
 *
 * @param st20_sessions
 * st20 tx sessions count.
 * @param st30_sessions
 * st30 tx sessions count.
 * @param st30_sessions
 * st30 sessions count.
 * @return
 *   queues count.
 */
static inline uint16_t st_rx_sessions_queue_cnt(uint16_t st20_sessions,
                                                uint16_t st30_sessions,
                                                uint16_t st40_sessions) {
  return st20_sessions + st30_sessions + st40_sessions;
}

#if defined(__cplusplus)
}
#endif

#endif
