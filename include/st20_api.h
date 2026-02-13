/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_api.h
 *
 * Interfaces for st2110-20/22 transport.
 *
 */

#include "st_api.h"

#ifndef _ST20_API_HEAD_H_
#define _ST20_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Max allowed number of video(st20) frame buffers
 */
#define ST20_FB_MAX_COUNT (8)

/**
 * Max allowed number of video(st22) frame buffers
 */
#define ST22_FB_MAX_COUNT (8)

/**
 * Flag bit in flags of struct st20_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST20_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST20_TX_FLAG_USER_R_MAC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * Frame addr set by user for zero-copy, for ST20_TYPE_FRAME_LEVEL
 */
#define ST20_TX_FLAG_EXT_FRAME (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * User control the frame transmission time by passing a timestamp in
 * st20_tx_frame_meta.timestamp, lib will wait until timestamp is reached for each frame.
 * The time of sending is aligned with virtual receiver read schedule.
 */
#define ST20_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value of
 * t20_tx_frame_meta.timestamp (if needed the value will be converted to
 * ST10_TIMESTAMP_FMT_MEDIA_CLK)
 */
#define ST20_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST20_TX_FLAG_ENABLE_VSYNC (MTL_BIT32(5))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enable the static RL pad interval profiling.
 * Static padding is trained only for e810, it is not recommended to use this flag
 * for other NICs.
 */
#define ST20_TX_FLAG_ENABLE_STATIC_PAD_P (MTL_BIT32(6))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enable the rtcp.
 */
#define ST20_TX_FLAG_ENABLE_RTCP (MTL_BIT32(7))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * It changes how ST20_TX_FLAG_USER_PACING works. if enabled, it does not align the
 * transmission time to the virtual receiver read schedule. The first
 * packet of the frame will be sent exactly at the time specified by the user.
 */
#define ST20_TX_FLAG_EXACT_USER_PACING (MTL_BIT32(8))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enabled the RTP timestamp will be set exactly to epoch + N *
 * frame_time, omitting TR_offset.
 */
#define ST20_TX_FLAG_RTP_TIMESTAMP_EPOCH (MTL_BIT32(9))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * Set this flag to the bulk operation on all internal buffer rings. It may degrade the
 * performance since the object enqueue/dequeue will be acted one by one.
 */
#define ST20_TX_FLAG_DISABLE_BULK (MTL_BIT32(10))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * Force the numa of the created session, both CPU and memory.
 */
#define ST20_TX_FLAG_FORCE_NUMA (MTL_BIT32(11))

/**
 * Flag bit in flags of struct st22_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST22_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST22_TX_FLAG_USER_R_MAC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * Disable ST22 boxes, for ST22_TYPE_FRAME_LEVEL
 */
#define ST22_TX_FLAG_DISABLE_BOXES (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * User control the frame pacing by pass a timestamp in st22_tx_frame_meta,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST22_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST22_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST22_TX_FLAG_ENABLE_VSYNC (MTL_BIT32(5))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * If enable the rtcp.
 */
#define ST22_TX_FLAG_ENABLE_RTCP (MTL_BIT32(6))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * Set this flag to the bulk operation on all internal buffer rings. It may degrade the
 * performance since the object enqueue/dequeue will be acted one by one.
 */
#define ST22_TX_FLAG_DISABLE_BULK (MTL_BIT32(7))
/**
 * Flag bit in flags of struct st22_tx_ops.
 * Force the numa of the created session, both CPU and memory.
 */
#define ST22_TX_FLAG_FORCE_NUMA (MTL_BIT32(8))

/**
 * Flag bit in flags of struct st20_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st20_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST20_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST20_RX_FLAG_ENABLE_VSYNC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * If enable the rtcp.
 */
#define ST20_RX_FLAG_ENABLE_RTCP (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * If enabled, simulate random packet loss, test usage only.
 */
#define ST20_RX_FLAG_SIMULATE_PKT_LOSS (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Force the numa of the created session, both CPU and memory.
 */
#define ST20_RX_FLAG_FORCE_NUMA (MTL_BIT32(4))

/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
 * If set, lib will pass the incomplete frame to app also by notify_frame_ready.
 * User can check st20_rx_frame_meta data for the frame integrity
 */
#define ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME (MTL_BIT32(16))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
 * If set, lib will try to allocate DMA memory copy offload from
 * dma_dev_port(mtl_init_params) list.
 * Pls note it could fallback to CPU if no DMA device is available.
 */
#define ST20_RX_FLAG_DMA_OFFLOAD (MTL_BIT32(17))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
 * If set, lib will automatically detect video format.
 * Width, height and fps set by app will be invalid.
 */
#define ST20_RX_FLAG_AUTO_DETECT (MTL_BIT32(18))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
 * Only ST20_PACKING_BPM stream can enable this offload as software limit
 * Try to enable header split offload feature.
 */
#define ST20_RX_FLAG_HDR_SPLIT (MTL_BIT32(19))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL and MTL_FLAG_RX_VIDEO_MIGRATE
 * is enabled.
 * Always disable MIGRATE for this session.
 */
#define ST20_RX_FLAG_DISABLE_MIGRATE (MTL_BIT32(20))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Enable the timing analyze info in the stat dump
 */
#define ST20_RX_FLAG_TIMING_PARSER_STAT (MTL_BIT32(21))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Enable the timing analyze info in the st20_rx_frame_meta
 */
#define ST20_RX_FLAG_TIMING_PARSER_META (MTL_BIT32(22))
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL.
 * Force to use multi(only two now) threads for the rx packet processing
 */
#define ST20_RX_FLAG_USE_MULTI_THREADS (MTL_BIT32(23))

/**
 * Flag bit in flags of struct st22_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st22_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST22_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st22_rx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST22_RX_FLAG_ENABLE_VSYNC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st22_rx_ops.
 * Disable ST22 boxes, for ST22_TYPE_FRAME_LEVEL
 */
#define ST22_RX_FLAG_DISABLE_BOXES (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st22_rx_ops.
 * If enable the rtcp.
 */
#define ST22_RX_FLAG_ENABLE_RTCP (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st22_rx_ops.
 * If enabled, simulate random packet loss, test usage only.
 */
#define ST22_RX_FLAG_SIMULATE_PKT_LOSS (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st22_rx_ops.
 * Force the numa of the created session, both CPU and memory.
 */
#define ST22_RX_FLAG_FORCE_NUMA (MTL_BIT32(5))

/**
 * Flag bit in flags of struct st22_rx_ops.
 * Only for ST22_TYPE_FRAME_LEVEL.
 * If set, lib will pass the incomplete frame to app also by notify_frame_ready.
 * User can check st22_rx_frame_meta data for the frame integrity
 */
#define ST22_RX_FLAG_RECEIVE_INCOMPLETE_FRAME (MTL_BIT32(16))

/**
 * Handle to tx st2110-20(video) session
 */
typedef struct st_tx_video_session_handle_impl* st20_tx_handle;
/**
 * Handle to tx st2110-22(compressed video) session
 */
typedef struct st22_tx_video_session_handle_impl* st22_tx_handle;
/**
 * Handle to rx st2110-20(video) session
 */
typedef struct st_rx_video_session_handle_impl* st20_rx_handle;
/**
 * Handle to rx st2110-22(compressed video) session
 */
typedef struct st22_rx_video_session_handle_impl* st22_rx_handle;

/**
 * Pacing type of st2110-20(video) sender
 */
enum st21_pacing {
  ST21_PACING_NARROW = 0, /**< narrow gapped sender */
  ST21_PACING_WIDE,       /**< wide sender */
  ST21_PACING_LINEAR,     /**< narrow linear sender */
  ST21_PACING_MAX,        /**< max value of this enum */
};

/**
 * Format type of st2110-20(video) streaming
 */
enum st20_fmt {
  ST20_FMT_YUV_422_10BIT = 0, /**< 10-bit YUV 4:2:2 */
  ST20_FMT_YUV_422_8BIT,      /**< 8-bit YUV 4:2:2 */
  ST20_FMT_YUV_422_12BIT,     /**< 12-bit YUV 4:2:2 */
  ST20_FMT_YUV_422_16BIT,     /**< 16-bit YUV 4:2:2 */
  ST20_FMT_YUV_420_8BIT,      /**< 8-bit YUV 4:2:0 */
  ST20_FMT_YUV_420_10BIT,     /**< 10-bit YUV 4:2:0 */
  ST20_FMT_YUV_420_12BIT,     /**< 12-bit YUV 4:2:0 */
  ST20_FMT_YUV_420_16BIT,     /**< 16-bit YUV 4:2:0 */
  ST20_FMT_RGB_8BIT,          /**< 8-bit RGB */
  ST20_FMT_RGB_10BIT,         /**< 10-bit RGB */
  ST20_FMT_RGB_12BIT,         /**< 12-bit RGB */
  ST20_FMT_RGB_16BIT,         /**< 16-bit RGB */
  ST20_FMT_YUV_444_8BIT,      /**< 8-bit YUV 4:4:4 */
  ST20_FMT_YUV_444_10BIT,     /**< 10-bit YUV 4:4:4 */
  ST20_FMT_YUV_444_12BIT,     /**< 12-bit YUV 4:4:4 */
  ST20_FMT_YUV_444_16BIT,     /**< 16-bit YUV 4:4:4 */
  /*
   * Below are the formats which not compatible with st2110 rfc4175.
   * Ex, user want to transport ST_FRAME_FMT_YUV422PLANAR10LE directly with padding on the
   * network and no color convert required.
   */
  ST20_FMT_YUV_422_PLANAR10LE, /**< 10-bit YUV 4:2:2 planar little endian. Experimental
                                  now, how to support ext frame? */
  ST20_FMT_V210,               /**< 10-bit YUV 422 V210 */
  ST20_FMT_MAX,                /**< max value of this enum */
};

/**
 * Session type of st2110-20(video) streaming
 */
enum st20_type {
  /** app interface lib based on frame level */
  ST20_TYPE_FRAME_LEVEL = 0,
  /** app interface lib based on RTP level */
  ST20_TYPE_RTP_LEVEL,
  /**
   * similar to ST20_TYPE_FRAME_LEVEL but with slice control,
   * latency reduce to slice(lines) level.
   * BTW, pls always enable ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME then app can get the
   * notify for incomplete frame
   */
  ST20_TYPE_SLICE_LEVEL,
  /** max value of this enum */
  ST20_TYPE_MAX,
};

/**
 * Session type of st2110-22(compressed video) streaming
 */
enum st22_type {
  /** app interface lib based on frame level */
  ST22_TYPE_FRAME_LEVEL = 0,
  /** app interface lib based on RTP level, same to ST20_TYPE_RTP_LEVEL */
  ST22_TYPE_RTP_LEVEL,
  /** max value of this enum */
  ST22_TYPE_MAX,
};

/**
 * Packetization mode of st2110-22(compressed video) streaming
 */
enum st22_pack_type {
  /** Codestream packetization mode */
  ST22_PACK_CODESTREAM = 0,
  /** Slice packetization mode, not support now */
  ST22_PACK_SLICE,
  /** max value of this enum */
  ST22_PACK_MAX,
};

/**
 * Session packing mode of st2110-20(video) streaming
 */
enum st20_packing {
  ST20_PACKING_BPM = 0, /**< block packing mode */
  ST20_PACKING_GPM,     /**< general packing mode */
  ST20_PACKING_GPM_SL,  /**< general packing mode, single scan line */
  ST20_PACKING_MAX,     /**< max value of this enum */
};

/**
 * A structure describing a st2110-20(video) pixel group
 */
struct st20_pgroup {
  /** video format of current pixel group */
  enum st20_fmt fmt;
  /** pixel group size(octets), e.g. 5 for YUV422 10 bit */
  uint32_t size;
  /** pixel group coverage(pixels), e.g. 2 for YUV422 10 bit */
  uint32_t coverage;
  /** name */
  char* name;
};

/**
 * Frame meta data of st2110-20(video) tx streaming
 */
struct st20_tx_frame_meta {
  /** Frame resolution width */
  uint32_t width;
  /** Frame resolution height */
  uint32_t height;
  /** Frame resolution fps */
  enum st_fps fps;
  /** Frame resolution format */
  enum st20_fmt fmt;
  /** Second field type indicate for interlaced mode, set by user */
  bool second_field;
  /** Timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Timestamp value */
  uint64_t timestamp;
  /** epoch */
  uint64_t epoch;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /**
   * The user meta data buffer for current frame, the size must smaller than
   * MTL_PKT_MAX_RTP_BYTES. This data will be transported to RX with video data and passed
   * to user by user_meta in st20_rx_frame_meta.
   */
  const void* user_meta;
  /** size for meta data buffer */
  size_t user_meta_size;
};

/**
 * Slice meta data of st2110-20(video) tx streaming
 */
struct st20_tx_slice_meta {
  /** Ready lines */
  uint16_t lines_ready;
};

/**
 * st20 rx timing parser meta for each frame as defined in SMPTE ST2110-21.
 * Referenced from
 * `https://github.com/ebu/pi-list/blob/master/docs/video_timing_analysis.md`.
 *
 * cinst: Instantaneous value of the Retwork Compatibility model C.
 * vrx: Measured level of the Virtual Receive Buffer.
 * ipt: Inter-packet time, ns
 * fpt: First Packet Time measured between frame/field reference time and the first
 *      captured packet of a frame/field. Unit: ns
 * latency: TPA0(Actual measured arrival time of a packet) - RTP Timestamp. Unit: ns
 * rtp_offset: RTP OFFSET = RTP Timestamp - N x Tframe, unit: timestamp ticks
 * rtp_ts_delta: Delta between RTP timestamps of 2 consecutive frames/fields,
 *               unit: timestamp ticks.
 */
struct st20_rx_tp_meta {
  /** the max of cinst for current frame */
  int32_t cinst_max;
  /** the min of cinst for current frame */
  int32_t cinst_min;
  /** the average of cinst for current frame */
  float cinst_avg;
  /** the max of vrx for current frame */
  int32_t vrx_max;
  /** the min of vrx for current frame */
  int32_t vrx_min;
  /** the average of vrx for current frame */
  float vrx_avg;
  /** the max of ipt(Inter-packet time, ns) for current frame */
  int32_t ipt_max;
  /** the min of ipt(Inter-packet time, ns) for current frame */
  int32_t ipt_min;
  /** the average of ipt(Inter-packet time, ns) for current frame */
  float ipt_avg;

  /** fpt(ns) for current frame */
  int32_t fpt;
  /** latency(ns) for current frame */
  int32_t latency;
  /** rtp_offset(ticks) for current frame */
  int32_t rtp_offset;
  /** rtp_ts_delta(ticks) for current frame */
  int32_t rtp_ts_delta;
  /** RX timing parser compliant result */
  enum st_rx_tp_compliant compliant;
  /** the failed cause if compliant is not ST_RX_TP_COMPLIANT_NARROW */
  char failed_cause[64];
  /** TAI timestamp measured right after first packet of the frame was received */
  uint64_t receive_timestamp;

  /* packets count in current report meta */
  uint32_t pkts_cnt;
};

/** st20 rx timing parser pass critical */
struct st20_rx_tp_pass {
  /** The max allowed cinst for narrow */
  int32_t cinst_max_narrow;
  /** The max allowed cinst for wide */
  int32_t cinst_max_wide;
  /** The min allowed cinst: 0 */
  int32_t cinst_min;
  /** The max allowed vrx full for narrow */
  int32_t vrx_max_narrow;
  /** The max allowed vrx wide for narrow */
  int32_t vrx_max_wide;
  /** The min allowed vrx: 0 */
  int32_t vrx_min;
  /** tr_offset, in ns, pass if fpt < tr_offset */
  int32_t tr_offset;
  /** The max allowed latency: 1000 us */
  int32_t latency_max;
  /** The min allowed latency: 0 */
  int32_t latency_min;
  /** The max allowed rtp_offset */
  int32_t rtp_offset_max;
  /** The min allowed rtp_offset: -1 */
  int32_t rtp_offset_min;
  /** The max allowed rtp_ts_delta */
  int32_t rtp_ts_delta_max;
  /** The min allowed rtp_ts_delta */
  int32_t rtp_ts_delta_min;
};

/**
 * Frame meta data of st2110-20(video) rx streaming
 */
struct st20_rx_frame_meta {
  /** Frame resolution width */
  uint32_t width;
  /** Frame resolution height */
  uint32_t height;
  /** Frame resolution fps */
  enum st_fps fps;
  /** Frame resolution format */
  enum st20_fmt fmt;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
  /** Frame status, complete or not */
  enum st_frame_status status;
  /** Frame total size */
  size_t frame_total_size;
  /** The total size for user frame */
  size_t uframe_total_size;
  /** Second field type indicate, for interlaced mode */
  bool second_field;
  /**
   * The actual received size for current frame, user can inspect frame_recv_size
   * against frame_total_size to check the signal integrity for incomplete frame.
   */
  size_t frame_recv_size;
  /** Private data for user, get from query_ext_frame callback */
  void* opaque;
  /** timestamp(ST10_TIMESTAMP_FMT_TAI in ns, PTP) value for the first pkt */
  uint64_t timestamp_first_pkt;
  /** timestamp(ST10_TIMESTAMP_FMT_TAI in ns, PTP) value for the first pkt */
  uint64_t timestamp_last_pkt;
  /** first packet time in ns to the start of current epoch */
  int64_t fpt;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /**
   * The received user meta data buffer for current frame.
   */
  const void* user_meta;
  /** size for meta data buffer */
  size_t user_meta_size;
  /** the total packets received, not include the redundant packets */
  uint32_t pkts_total;
  /** the valid packets received on each session port. For each session port, the validity
   * of received packets can be assessed by comparing 'pkts_recv[s_port]' with
   * 'pkts_total,' which serves as an indicator of signal quality.  */
  uint32_t pkts_recv[MTL_SESSION_PORT_MAX];
  /** st20 rx timing parser meta, only active if ST20_RX_FLAG_TIMING_PARSER_META */
  struct st20_rx_tp_meta* tp[MTL_SESSION_PORT_MAX];
};

/**
 * Slice meta data of st2110-20(video) rx streaming
 */
struct st20_rx_slice_meta {
  /** Frame resolution width */
  uint32_t width;
  /** Frame resolution height */
  uint32_t height;
  /** Frame resolution fps */
  enum st_fps fps;
  /** Frame resolution format */
  enum st20_fmt fmt;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame total size */
  size_t frame_total_size;
  /** The total size for user frame */
  size_t uframe_total_size;
  /** Second field type indicate, for interlaced mode */
  bool second_field;
  /** Frame timestamp value */
  uint64_t timestamp;
  /** The received size for current frame */
  size_t frame_recv_size;
  /** The received lines for current frame */
  uint32_t frame_recv_lines;
};

/**
 * Pixel group meta data for user frame st2110-20(video) rx streaming.
 */
struct st20_rx_uframe_pg_meta {
  /** Frame resolution width */
  uint32_t width;
  /** Frame resolution height */
  uint32_t height;
  /** Frame resolution fps */
  enum st_fps fps;
  /** Frame resolution format */
  enum st20_fmt fmt;
  /** The total size for raw frame */
  size_t frame_total_size;
  /** The total size for user frame */
  size_t uframe_total_size;
  /** Point to current pixel groups data */
  void* payload;
  /** Number of octets of data included from current pixel groups data */
  uint16_t row_length;
  /** Scan line number */
  uint16_t row_number;
  /** Offset of the first pixel of the payload data within current pixel groups data */
  uint16_t row_offset;
  /** How many pixel groups in current meta */
  uint32_t pg_cnt;
  /** Frame timestamp */
  uint64_t timestamp;
};

/**
 * Frame meta data of st2110-22(video) tx streaming
 */
struct st22_tx_frame_meta {
  /** Frame resolution width, set by lib */
  uint32_t width;
  /** Frame resolution height, set by lib */
  uint32_t height;
  /** Frame resolution fps, set by lib */
  enum st_fps fps;
  /** Second field type indicate, for interlaced mode */
  bool second_field;
  /** codestream_size for next_frame_idx, set by user */
  size_t codestream_size;
  /** Timestamp format, user can customize it if ST22_TX_FLAG_USER_PACING */
  enum st10_timestamp_fmt tfmt;
  /** Timestamp value, user can customize it if ST22_TX_FLAG_USER_PACING */
  uint64_t timestamp;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /** epoch */
  uint64_t epoch;
};

/**
 * Frame meta data of st2110-22(video) rx streaming
 */
struct st22_rx_frame_meta {
  /** Second field type indicate, for interlaced mode */
  bool second_field;
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /** Frame total size */
  size_t frame_total_size;
  /** Frame status, complete or not */
  enum st_frame_status status;
  /** the total packets received, not include the redundant packets */
  uint32_t pkts_total;
  /** the valid packets received on each session port. For each session port, the validity
   * of received packets can be assessed by comparing 'pkts_recv[s_port]' with
   * 'pkts_total,' which serves as an indicator of signal quality.  */
  uint32_t pkts_recv[MTL_SESSION_PORT_MAX];
};

/**
 * The Continuation bit shall in row_offset be set to 1 if an additional Sample Row Data.
 * Header follows the current Sample Row Data Header in the RTP Payload
 * Header, which signals that the RTP packet is carrying data for more than one
 * sample row. The Continuation bit shall be set to 0 otherwise.
 */
#define ST20_SRD_OFFSET_CONTINUATION (0x1 << 15)
/**
 * The field identification bit in row_number shall be set to 1 if the payload comes from
 * second field.The field identification bit shall be set to 0 otherwise.
 */
#define ST20_SECOND_FIELD (0x1 << 15)
/**
 * The retransmit bit in row_length shall be set to 1 if it is a retransmit packet.
 * Do not use it when row length can be larger than 16383.
 */
#define ST20_RETRANSMIT (0x1 << 14)

#ifndef __MTL_PYTHON_BUILD__
/**
 * A structure describing a st2110-20(video) rfc4175 rtp header, size: 20
 */
MTL_PACK(struct st20_rfc4175_rtp_hdr {
  /** Rtp rfc3550 base hdr */
  struct st_rfc3550_rtp_hdr base;
  /** Extended Sequence Number */
  uint16_t seq_number_ext;
  /** Number of octets of data included from this scan line */
  uint16_t row_length;
  /** Scan line number */
  uint16_t row_number;
  /** Offset of the first pixel of the payload data within the scan line */
  uint16_t row_offset;
});

/**
 * A structure describing a st2110-22(video) rfc9134 rtp header, size: 16
 */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st22_rfc9134_rtp_hdr {
  /** Rtp rfc3550 base hdr */
  struct st_rfc3550_rtp_hdr base;
  /** F counter high part */
  uint8_t f_counter_hi : 3;
  /** Interlaced information, 0b00: progressively, 0b10: first field, 0b11: second field.
   */
  uint8_t interlaced : 2;
  /** Last */
  uint8_t last_packet : 1;
  /** pacKetization mode */
  uint8_t kmode : 1;
  /** Transmission mode */
  uint8_t trans_order : 1;

  /** Sep counter high part */
  uint8_t sep_counter_hi : 6;
  /** F counter low part */
  uint8_t f_counter_lo : 2;

  /** P counter high part */
  uint8_t p_counter_hi : 3;
  /** F counter low part */
  uint8_t sep_counter_lo : 5;

  /** P counter low part */
  uint8_t p_counter_lo;
});
#else
MTL_PACK(struct st22_rfc9134_rtp_hdr {
  /** Rtp rfc3550 base hdr */
  struct st_rfc3550_rtp_hdr base;
  /** Transmission mode */
  uint8_t trans_order : 1;
  /** pacKetization mode */
  uint8_t kmode : 1;
  /** Last */
  uint8_t last_packet : 1;
  /** Interlaced information */
  uint8_t interlaced : 2;
  /** F counter high part */
  uint8_t f_counter_hi : 3;

  /** F counter low part */
  uint8_t f_counter_lo : 2;
  /** Sep counter high part */
  uint8_t sep_counter_hi : 6;

  /** F counter low part */
  uint8_t sep_counter_lo : 5;
  /** P counter high part */
  uint8_t p_counter_hi : 3;

  /** P counter low part */
  uint8_t p_counter_lo;
});
#endif

/**
 * A structure describing a st2110-20(video) rfc4175 rtp additional header.
 * if Continuation bit is set in struct st20_rfc4175_rtp_hdr. size: 6
 */
MTL_PACK(struct st20_rfc4175_extra_rtp_hdr {
  /** Number of octets of data included from this scan line */
  uint16_t row_length;
  /** Scan line number */
  uint16_t row_number;
  /** Offset of the first pixel of the payload data within the scan line */
  uint16_t row_offset;
});

/** Pixel Group describing two image pixels in YUV 4:4:4 or RGB 12-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_444_12_pg2_be {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Y_G00 : 4;   /**< First 4 bits for Y/Green 0 */
  uint8_t Cb_R00_ : 4; /**< Second 4 bits for Cb/Red 0 */
  uint8_t Y_G00_;      /**< Second 8 bits for Y/Green 0 */
  uint8_t Cr_B00;      /**< First 8 bits for Cr/Blue 0 */
  uint8_t Cb_R01 : 4;  /**< First 4 bits for Cb/Red 1 */
  uint8_t Cr_B00_ : 4; /**< Second 4 bits for Cr/Blue 0 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Cr_B01 : 4;  /**< First 4 bits for Cr/Blue 1 */
  uint8_t Y_G01_ : 4;  /**< Second 4 bits for Y/Green 1 */
  uint8_t Cr_B01_;     /**< Second 8 bits for Cr/Blue 1 */
});
#else
MTL_PACK(struct st20_rfc4175_444_12_pg2_be {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Cb_R00_ : 4; /**< Second 4 bits for Cb/Red 0 */
  uint8_t Y_G00 : 4;   /**< First 4 bits for Y/Green 0 */
  uint8_t Y_G00_;      /**< Second 8 bits for Y/Green 0 */
  uint8_t Cr_B00;      /**< First 8 bits for Cr/Blue 0 */
  uint8_t Cr_B00_ : 4; /**< Second 4 bits for Cr/Blue 0 */
  uint8_t Cb_R01 : 4;  /**< First 4 bits for Cb/Red 1 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Y_G01_ : 4;  /**< Second 4 bits for Y/Green 1 */
  uint8_t Cr_B01 : 4;  /**< First 4 bits for Cr/Blue 1 */
  uint8_t Cr_B01_;     /**< Second 8 bits for Cr/Blue 1 */
});
#endif

/** Pixel Group describing two image pixels in YUV 4:4:4 or RGB 12-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_444_12_pg2_le {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Cb_R00_ : 4; /**< Second 4 bits for Cb/Red 0 */
  uint8_t Y_G00 : 4;   /**< First 4 bits for Y/Green 0 */
  uint8_t Y_G00_;      /**< Second 8 bits for Y/Green 0 */
  uint8_t Cr_B00;      /**< First 8 bits for Cr/Blue 0 */
  uint8_t Cr_B00_ : 4; /**< Second 4 bits for Cr/Blue 0 */
  uint8_t Cb_R01 : 4;  /**< First 4 bits for Cb/Red 1 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Y_G01_ : 4;  /**< Second 4 bits for Y/Green 1 */
  uint8_t Cr_B01 : 4;  /**< First 4 bits for Cr/Blue 1 */
  uint8_t Cr_B01_;     /**< Second 8 bits for Cr/Blue 1 */
});
#else
MTL_PACK(struct st20_rfc4175_444_12_pg2_le {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Y_G00 : 4;   /**< First 4 bits for Y/Green 0 */
  uint8_t Cb_R00_ : 4; /**< Second 4 bits for Cb/Red 0 */
  uint8_t Y_G00_;      /**< Second 8 bits for Y/Green 0 */
  uint8_t Cr_B00;      /**< First 8 bits for Cr/Blue 0 */
  uint8_t Cb_R01 : 4;  /**< First 4 bits for Cb/Red 1 */
  uint8_t Cr_B00_ : 4; /**< Second 4 bits for Cr/Blue 0 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Cr_B01 : 4;  /**< First 4 bits for Cr/Blue 1 */
  uint8_t Y_G01_ : 4;  /**< Second 4 bits for Y/Green 1 */
  uint8_t Cr_B01_;     /**< Second 8 bits for Cr/Blue 1 */
});
#endif

/** Pixel Group describing four image pixels in YUV 4:4:4 or RGB 10-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_444_10_pg4_be {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Y_G00 : 6;   /**< First 6 bits for Y/Green 0 */
  uint8_t Cb_R00_ : 2; /**< Second 2 bits for Cb/Red 0 */
  uint8_t Cr_B00 : 4;  /**< First 4 bits for Cr/Blue 0 */
  uint8_t Y_G00_ : 4;  /**< Second 4 bits for Y/Green 0 */
  uint8_t Cb_R01 : 2;  /**< First 2 bits for Cb/Red 1 */
  uint8_t Cr_B00_ : 6; /**< Second 6 bits for Cr/Blue 0 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Cr_B01 : 6;  /**< First 6 bits for Cr/Blue 1 */
  uint8_t Y_G01_ : 2;  /**< Second 2 bits for Y/Green 1 */
  uint8_t Cb_R02 : 4;  /**< First 4 bits for Cb/Red 2 */
  uint8_t Cr_B01_ : 4; /**< Second 2 bits for Cr/Blue 1 */
  uint8_t Y_G02 : 2;   /**< First 2 bits for Y/Green 2 */
  uint8_t Cb_R02_ : 6; /**< Second 6 bits for Cb/Red 2 */
  uint8_t Y_G02_;      /**< Second 8 bits for Y/Green 2 */
  uint8_t Cr_B02;      /**< First 8 bits for Cr/Blue 2 */
  uint8_t Cb_R03 : 6;  /**< First 6 bits for Cb/Red 3 */
  uint8_t Cr_B02_ : 2; /**< Second 2 bits for Cr/Blue 2 */
  uint8_t Y_G03 : 4;   /**< First 4 bits for Y/Green 3 */
  uint8_t Cb_R03_ : 4; /**< Second 4 bits for Cb/Red 3 */
  uint8_t Cr_B03 : 2;  /**< First 2 bits for Cr/Blue 3 */
  uint8_t Y_G03_ : 6;  /**< Second 6 bits for Y/Green 3 */
  uint8_t Cr_B03_;     /**< Second 8 bits for Cr/Blue 1 */
});
#else
MTL_PACK(struct st20_rfc4175_444_10_pg4_be {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Cb_R00_ : 2; /**< Second 2 bits for Cb/Red 0 */
  uint8_t Y_G00 : 6;   /**< First 6 bits for Y/Green 0 */
  uint8_t Y_G00_ : 4;  /**< Second 4 bits for Y/Green 0 */
  uint8_t Cr_B00 : 4;  /**< First 4 bits for Cr/Blue 0 */
  uint8_t Cr_B00_ : 6; /**< Second 6 bits for Cr/Blue 0 */
  uint8_t Cb_R01 : 2;  /**< First 2 bits for Cb/Red 1 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Y_G01_ : 2;  /**< Second 2 bits for Y/Green 1 */
  uint8_t Cr_B01 : 6;  /**< First 6 bits for Cr/Blue 1 */
  uint8_t Cr_B01_ : 4; /**< Second 2 bits for Cr/Blue 1 */
  uint8_t Cb_R02 : 4;  /**< First 4 bits for Cb/Red 2 */
  uint8_t Cb_R02_ : 6; /**< Second 6 bits for Cb/Red 2 */
  uint8_t Y_G02 : 2;   /**< First 2 bits for Y/Green 2 */
  uint8_t Y_G02_;      /**< Second 8 bits for Y/Green 2 */
  uint8_t Cr_B02;      /**< First 8 bits for Cr/Blue 2 */
  uint8_t Cr_B02_ : 2; /**< Second 2 bits for Cr/Blue 2 */
  uint8_t Cb_R03 : 6;  /**< First 6 bits for Cb/Red 3 */
  uint8_t Cb_R03_ : 4; /**< Second 4 bits for Cb/Red 3 */
  uint8_t Y_G03 : 4;   /**< First 4 bits for Y/Green 3 */
  uint8_t Y_G03_ : 6;  /**< Second 6 bits for Y/Green 3 */
  uint8_t Cr_B03 : 2;  /**< First 2 bits for Cr/Blue 3 */
  uint8_t Cr_B03_;     /**< Second 8 bits for Cr/Blue 1 */
});
#endif

/** Pixel Group describing four image pixels in YUV 4:4:4 or RGB 10-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_444_10_pg4_le {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Cb_R00_ : 2; /**< Second 2 bits for Cb/Red 0 */
  uint8_t Y_G00 : 6;   /**< First 6 bits for Y/Green 0 */
  uint8_t Y_G00_ : 4;  /**< Second 4 bits for Y/Green 0 */
  uint8_t Cr_B00 : 4;  /**< First 4 bits for Cr/Blue 0 */
  uint8_t Cr_B00_ : 6; /**< Second 6 bits for Cr/Blue 0 */
  uint8_t Cb_R01 : 2;  /**< First 2 bits for Cb/Red 1 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Y_G01_ : 2;  /**< Second 2 bits for Y/Green 1 */
  uint8_t Cr_B01 : 6;  /**< First 6 bits for Cr/Blue 1 */
  uint8_t Cr_B01_ : 4; /**< Second 2 bits for Cr/Blue 1 */
  uint8_t Cb_R02 : 4;  /**< First 4 bits for Cb/Red 2 */
  uint8_t Cb_R02_ : 6; /**< Second 6 bits for Cb/Red 2 */
  uint8_t Y_G02 : 2;   /**< First 2 bits for Y/Green 2 */
  uint8_t Y_G02_;      /**< Second 8 bits for Y/Green 2 */
  uint8_t Cr_B02;      /**< First 8 bits for Cr/Blue 2 */
  uint8_t Cr_B02_ : 2; /**< Second 2 bits for Cr/Blue 2 */
  uint8_t Cb_R03 : 6;  /**< First 6 bits for Cb/Red 3 */
  uint8_t Cb_R03_ : 4; /**< Second 4 bits for Cb/Red 3 */
  uint8_t Y_G03 : 4;   /**< First 4 bits for Y/Green 3 */
  uint8_t Y_G03_ : 6;  /**< Second 6 bits for Y/Green 3 */
  uint8_t Cr_B03 : 2;  /**< First 2 bits for Cr/Blue 3 */
  uint8_t Cr_B03_;     /**< Second 8 bits for Cr/Blue 1 */
});
#else
MTL_PACK(struct st20_rfc4175_444_10_pg4_le {
  uint8_t Cb_R00;      /**< First 8 bits for Cb/Red 0 */
  uint8_t Y_G00 : 6;   /**< First 6 bits for Y/Green 0 */
  uint8_t Cb_R00_ : 2; /**< Second 2 bits for Cb/Red 0 */
  uint8_t Cr_B00 : 4;  /**< First 4 bits for Cr/Blue 0 */
  uint8_t Y_G00_ : 4;  /**< Second 4 bits for Y/Green 0 */
  uint8_t Cb_R01 : 2;  /**< First 2 bits for Cb/Red 1 */
  uint8_t Cr_B00_ : 6; /**< Second 6 bits for Cr/Blue 0 */
  uint8_t Cb_R01_;     /**< Second 8 bits for Cb/Red 1 */
  uint8_t Y_G01;       /**< First 8 bits for Y/Green 1 */
  uint8_t Cr_B01 : 6;  /**< First 6 bits for Cr/Blue 1 */
  uint8_t Y_G01_ : 2;  /**< Second 2 bits for Y/Green 1 */
  uint8_t Cb_R02 : 4;  /**< First 4 bits for Cb/Red 2 */
  uint8_t Cr_B01_ : 4; /**< Second 2 bits for Cr/Blue 1 */
  uint8_t Y_G02 : 2;   /**< First 2 bits for Y/Green 2 */
  uint8_t Cb_R02_ : 6; /**< Second 6 bits for Cb/Red 2 */
  uint8_t Y_G02_;      /**< Second 8 bits for Y/Green 2 */
  uint8_t Cr_B02;      /**< First 8 bits for Cr/Blue 2 */
  uint8_t Cb_R03 : 6;  /**< First 6 bits for Cb/Red 3 */
  uint8_t Cr_B02_ : 2; /**< Second 2 bits for Cr/Blue 2 */
  uint8_t Y_G03 : 4;   /**< First 4 bits for Y/Green 3 */
  uint8_t Cb_R03_ : 4; /**< Second 4 bits for Cb/Red 3 */
  uint8_t Cr_B03 : 2;  /**< First 2 bits for Cr/Blue 3 */
  uint8_t Y_G03_ : 6;  /**< Second 6 bits for Y/Green 3 */
  uint8_t Cr_B03_;     /**< Second 8 bits for Cr/Blue 1 */
});
#endif

/** Pixel Group describing two image pixels in YUV 4:2:2 12-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_422_12_pg2_be {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Y00 : 4;   /**< First 4 bits Luminance for Y0 */
  uint8_t Cb00_ : 4; /**< Second 4 bit Blue */
  uint8_t Y00_;      /**< Second 8 bits Luminance for Y0 */
  uint8_t Cr00;      /**< First 8 bit Red */
  uint8_t Y01 : 4;   /**< First 4 bits Luminance for Y1 */
  uint8_t Cr00_ : 4; /**< Second 4 bit Red */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#else
MTL_PACK(struct st20_rfc4175_422_12_pg2_be {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Cb00_ : 4; /**< Second 4 bit Blue */
  uint8_t Y00 : 4;   /**< First 4 bits Luminance for Y0 */
  uint8_t Y00_;      /**< Second 8 bits Luminance for Y0 */
  uint8_t Cr00;      /**< First 8 bit Red */
  uint8_t Cr00_ : 4; /**< Second 4 bit Red */
  uint8_t Y01 : 4;   /**< First 4 bits Luminance for Y1 */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#endif

/** Pixel Group describing two image pixels in YUV 4:2:2 12-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_422_12_pg2_le {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Cb00_ : 4; /**< Second 4 bit Blue */
  uint8_t Y00 : 4;   /**< First 4 bits Luminance for Y0 */
  uint8_t Y00_;      /**< Second 8 bits Luminance for Y0 */
  uint8_t Cr00;      /**< First 8 bit Red */
  uint8_t Cr00_ : 4; /**< Second 4 bit Red */
  uint8_t Y01 : 4;   /**< First 4 bits Luminance for Y1 */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#else
MTL_PACK(struct st20_rfc4175_422_12_pg2_le {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Y00 : 4;   /**< First 4 bits Luminance for Y0 */
  uint8_t Cb00_ : 4; /**< Second 4 bit Blue */
  uint8_t Y00_;      /**< Second 8 bits Luminance for Y0 */
  uint8_t Cr00;      /**< First 8 bit Red */
  uint8_t Y01 : 4;   /**< First 4 bits Luminance for Y1 */
  uint8_t Cr00_ : 4; /**< Second 4 bit Red */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#endif

/** Pixel Group describing two image pixels in YUV 4:2:2 10-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_422_10_pg2_be {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#else
MTL_PACK(struct st20_rfc4175_422_10_pg2_be {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#endif

/** Pixel Group describing two image pixels in YUV 4:2:2 10-bit format */
#ifdef MTL_LITTLE_ENDIAN
MTL_PACK(struct st20_rfc4175_422_10_pg2_le {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#else
MTL_PACK(struct st20_rfc4175_422_10_pg2_le {
  uint8_t Cb00;      /**< First 8 bit Blue */
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
  uint8_t Y01_;      /**< Second 8 bits Luminance for Y1 */
});
#endif

/** Pixel Group describing two image pixels in YUV 4:2:2 8-bit format */
MTL_PACK(struct st20_rfc4175_422_8_pg2_le {
  uint8_t Cb00; /**< 8 bit Blue */
  uint8_t Y00;  /**< 8 bit Y0 */
  uint8_t Cr00; /**< 8 bit Red */
  uint8_t Y01;  /**< 8 bit Y1 */
});
#endif

/** External framebuffer */
struct st20_ext_frame {
  /** Virtual address of external framebuffer */
  void* buf_addr;
  /** DMA mapped IOVA of external framebuffer */
  mtl_iova_t buf_iova;
  /** Length of external framebuffer */
  size_t buf_len;
  /** Private data for user, will be retrieved with st_frame or st20_rx_frame_meta */
  void* opaque;
};

/**
 * The RTCP info for tx st2110-20/22 session.
 */
struct st_tx_rtcp_ops {
  /**
   * The size of the packets buffer for RTCP, should be power of two.
   * This value should not be less than nb_tx_desc.
   * Only used when ST20(P)/22(P)_TX_FLAG_ENABLE_RTCP flag set.
   * If leave it to 0 the lib will use ST_TX_VIDEO_RTCP_RING_SIZE.
   */
  uint16_t buffer_size;
};

/**
 * The structure describing how to create a tx st2110-20(video) session.
 * Include the PCIE port and other required info.
 */
struct st20_tx_ops {
  /** Mandatory. Destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDFs of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. UDP destination port number for this tx session */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. Sender pacing type, default is narrow */
  enum st21_pacing pacing;
  /** Mandatory. Session streaming type, frame(default) or RTP */
  enum st20_type type;
  /** Mandatory. Session packing mode, default is BPM */
  enum st20_packing packing;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session resolution format */
  enum st20_fmt fmt;
  /** Mandatory. 7 bits payload type defined in RFC3550 */
  uint8_t payload_type;

  /** Optional. Synchronization source defined in RFC3550, if zero the session will assign
   * a random value */
  uint32_t ssrc;
  /** Optional. Name */
  const char* name;
  /** Optional. Private data to the cb functions(get_next_frame and others) */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST20_TX_FLAG_* for possible value
   */
  uint32_t flags;

  /**
   * Mandatory for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * the frame buffer count requested for one st20 tx session,
   * should be in range [2, ST20_FB_MAX_COUNT].
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * The callback when lib require a new frame for sending, user should provide the next
   * available frame index to next_frame_idx. It implicit means the frame ownership will
   * be transferred to lib. And only non-block method can be used within this callback, as
   * it run from lcore tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta);
  /**
   * Optional for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * The callback when lib finish the sending of one frame, frame_idx indicate the
   * done frame. It implicit means the frame ownership is transferred to app. And only
   * non-block method can be used within this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st20_tx_frame_meta* meta);

  /**
   * Optional. Callback triggered when a frame epoch is omitted/skipped in the lib.
   * This occurs when the transmission timing falls behind schedule and an epoch
   * must be skipped to maintain synchronization. (or in the user pacing mode
   * when the user time is behind the lib sending time).
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);
  /**
   * Optional. The event callback when there is some event(vsync or others) happened for
   * this session. Only non-block method can be used in this callback as it run from lcore
   * routine. Args point to the meta data of each event. Ex, cast to struct
   * st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /** Optional for ST20_TX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_tx_rtcp_ops rtcp;
  /**
   * Optional. Session linesize(stride) in bytes, leave to zero if no padding for each
   * line. Valid linesize should be wider than width size.
   */
  uint32_t linesize;
  /** Optional. UDP source port number, leave as 0 to use same port as destination port */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /** Optional. The tx destination mac address if ST20_TX_FLAG_USER_P(R)_MAC is enabled */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];
  /**
   * Optional. The start vrx buffer.
   * Leave to zero if not know detail, lib will assign a start value of vrx(narrow) based
   * on resolution and timing. Refer to st21 spec for the possible vrx value and also fine
   * tune is required since network setup difference and RL burst.
   */
  uint16_t start_vrx;
  /**
   * Optional. Manually assigned padding pkt interval(pkts level) for RL pacing.
   * Leave to zero if not know detail, lib will train the interval in the initial routine.
   */
  uint16_t pad_interval;
  /**
   * Optional. The rtp timestamp delta(us) to the start time of frame.
   * Zero means the rtp timestamp at the start of the frame.
   */
  int32_t rtp_timestamp_delta_us;
  /**
   * Optional. The time for lib to detect the hang on the tx queue and try to recovery
   * Leave to zero system will use the default value(1s).
   */
  uint32_t tx_hang_detect_ms;

  /**
   * Mandatory for ST20_TYPE_SLICE_LEVEL.
   * The callback when lib requires new ready lines, user should provide the ready lines
   * number by the meta.
   */
  int (*query_frame_lines_ready)(void* priv, uint16_t frame_idx,
                                 struct st20_tx_slice_meta* meta);

  /** Mandatory for ST20_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Mandatory for ST20_TYPE_RTP_LEVEL. Total pkts in one frame, ex: 4320 for 1080p.
   */
  uint32_t rtp_frame_total_pkts;
  /**
   * Mandatory for ST20_TYPE_RTP_LEVEL.
   * Size for each rtp pkt, include both the payload data and rtp header, must small than
   * MTL_PKT_MAX_RTP_BYTES. Used by MTL to calculate the total bandwidth of each frame.
   * App still can customize the size of each pkt by the `len` parameter of
   * `st20_tx_put_mbuf`
   */
  uint16_t rtp_pkt_size;
  /**
   * Optional for ST20_TYPE_RTP_LEVEL.
   * The callback when lib finish the sending of one rtp packet.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_rtp_done)(void* priv);
  /**  Use this socket if ST20_TX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/**
 * The structure describing how to create a tx st2110-22(compressed video) session.
 * Include the PCIE port and other required info.
 */
struct st22_tx_ops {
  /** Mandatory. Destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDFs of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. UDP destination port number for this tx session */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Mandatory. Sender pacing type, default is narrow */
  enum st21_pacing pacing;
  /** Mandatory. Session streaming type, frame(default) or RTP */
  enum st22_type type;
  /** Mandatory. Packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session resolution format */
  enum st20_fmt fmt;
  /** Mandatory. 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /** Optional. Synchronization source defined in RFC3550, if zero the session will assign
   * a random value */
  uint32_t ssrc;
  /** Optional. Name */
  const char* name;
  /** Optional. Private data to the cb functions(get_next_frame and others) */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST22_TX_FLAG_* for possible value
   */
  uint32_t flags;

  /**
   * Mandatory for ST22_TYPE_FRAME_LEVEL.
   * the frame buffer count requested for one st22 tx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST22_TYPE_FRAME_LEVEL.
   * max framebuffer size for one st22 tx session codestream,
   * usually ST22 use constant bitrate (CBR) mode.
   * lib will allocate all frame buffer with this size,
   * app can indicate the real codestream size later in get_next_frame query.
   */
  size_t framebuff_max_size;
  /**
   * Mandatory for ST22_TYPE_FRAME_LEVEL. The callback when lib require a new frame for
   * sending. User should provide the next available frame index to next_frame_idx. It
   * implicit means the frame ownership will be transferred to lib. And only non-block
   * method can be used within this callback, as it run from lcore tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st22_tx_frame_meta* meta);
  /**
   * Optional for ST22_TYPE_FRAME_LEVEL.
   * The callback when lib finish the sending of current frame, frame_idx indicate the
   * done frame. It implicit means the frame ownership is transferred to app. And only
   * non-block method can be used within this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st22_tx_frame_meta* meta);

  /**
   * Optional. Callback triggered when a frame epoch is omitted/skipped in the lib.
   * This occurs when the transmission timing falls behind schedule and an epoch
   * must be skipped to maintain synchronization. (or in the user pacing mode
   * when the user time is behind the lib sending time).
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);

  /**
   * Optional. The event callback when there is some event(vsync or others) happened for
   * this session. Only non-block method can be used in this callback as it run from lcore
   * routine. Args point to the meta data of each event. Ex, cast to struct
   * st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /** Optional. UDP source port number, leave as 0 to use same port as destination port */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /** Optional for ST20_TX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_tx_rtcp_ops rtcp;
  /** Optional. The tx destination mac address if ST20_TX_FLAG_USER_P(R)_MAC is enabled */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /** Mandatory for ST22_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Mandatory for ST22_TYPE_RTP_LEVEL. total pkts in one rtp frame. Used by MTL to
   * calculate the total bandwidth of each frame, and user must build exactly the same
   * number packets of each frame by `st20_tx_put_mbuf` */
  uint32_t rtp_frame_total_pkts;
  /**
   * Mandatory for ST22_TYPE_RTP_LEVEL.
   * Size for each rtp pkt, include both the payload data and rtp header, must small than
   * MTL_PKT_MAX_RTP_BYTES. Used by MTL to calculate the total bandwidth of each frame.
   * App still can customize the size of each pkt by the `len` parameter of
   * `st20_tx_put_mbuf`
   */
  uint16_t rtp_pkt_size;
  /**
   * Optional for ST22_TYPE_RTP_LEVEL.
   * The callback when lib finish the sending of one rtp packet.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_rtp_done)(void* priv);
  /**  Use this socket if ST22_TX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/**
 * A structure used to pass detect metadata to app.
 */
struct st20_detect_meta {
  /** Stream resolution width */
  int width;
  /** Stream resolution height */
  int height;
  /** Stream FPS */
  enum st_fps fps;
  /** Packet packing mode */
  enum st20_packing packing;
  /** Interlaced scan, otherwise progressive */
  bool interlaced;
};

/**
 * A structure used to pass detect reply to lib.
 */
struct st20_detect_reply {
  /**
   * Only for ST20_TYPE_SLICE_LEVEL.
   * App replied slice lines when slice used.
   */
  uint32_t slice_lines;
  /**
   * Only used when user frame set.
   * App replied user frame size.
   */
  size_t uframe_size;
};

/**
 * The RTCP info for rx st2110-20/22 session.
 */
struct st_rx_rtcp_ops {
  /**
   * RTCP NACK send interval in us.
   * Only used when ST20/22_RX_FLAG_ENABLE_RTCP flag set.
   */
  uint32_t nack_interval_us;
  /**
   * RTCP seq bitmap size, window size is bitmap size * 8.
   * Only used when ST20/22_RX_FLAG_ENABLE_RTCP flag set.
   */
  uint16_t seq_bitmap_size;
  /**
   * RTCP seq skip window, missing within skip window will be ignored.
   * Only used when ST20/22_RX_FLAG_ENABLE_RTCP flag set.
   */
  uint16_t seq_skip_window;
  /**
   * Optional. max burst of simulated packet loss.
   * Only used when ST2*_RX_FLAG_SIMULATE_PKT_LOSS enabled.
   */
  uint16_t burst_loss_max;
  /**
   * Optional. simulated packet loss rate
   * Only used when ST2*_RX_FLAG_SIMULATE_PKT_LOSS enabled.
   */
  float sim_loss_rate;
};

/**
 * The structure describing how to create a rx st2110-20(video) session.
 * Include the PCIE port and other required info.
 */
struct st20_rx_ops {
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

  /** Mandatory. Session streaming type, frame(default) or RTP */
  enum st20_type type;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session resolution format */
  enum st20_fmt fmt;
  /** Mandatory. 7 bits payload type define in RFC3550. Zero means disable the
   * payload_type check on the RX pkt path */
  uint8_t payload_type;

  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Optional. Synchronization source defined in RFC3550, RX session will check the
   * incoming RTP packets match the ssrc. Leave to zero to disable the ssrc check */
  uint32_t ssrc;
  /** Optional. Not in use now as RX support all pacing type, reserved for future */
  enum st21_pacing pacing;
  /** Optional. Not in use now as RX support all packing type, reserved for future */
  enum st20_packing packing;

  /** Optional. Name */
  const char* name;
  /** Optional. Private data to the cb functions(notify_frame_ready and others) */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST20_RX_FLAG_* for possible value
   */
  uint32_t flags;
  /* Optional, the size for each mt_rxq_burst, leave to zero to let system select a
   * default value */
  uint16_t rx_burst_size;

  /**
   * Mandatory for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * the frame buffer count requested for one st20 rx session,
   * should be in range [2, ST20_FB_MAX_COUNT].
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * The callback when lib receive a new frame. It implicit means the frame ownership is
   * transferred to app.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st20_rx_put_framebuff
   * to return the frame when it finish the handling
   *   < 0: the error code if app can't handle, lib will call st20_rx_put_framebuff then.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void* priv, void* frame, struct st20_rx_frame_meta* meta);

  /**
   * Optional. the ST20_TYPE_FRAME_LEVEL external frame buffer info array,
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   */
  struct st20_ext_frame* ext_frames;
  /**
   * Optional. The event callback when there is some event(vsync or others) happened for
   * this session. Only non-block method can be used in this callback as it run from lcore
   * routine. Args point to the meta data of each event. Ex, cast to struct
   * st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /** Optional for ST20_RX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_rx_rtcp_ops rtcp;
  /**
   * Optional. Session linesize(stride) in bytes, leave to zero if no padding for each
   * line. Valid linesize should be wider than width size.
   */
  uint32_t linesize;

  /**
   * Optional for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * Total size for user frame, lib will allocate frame with this value. When lib receive
   * a payload from network, it will call uframe_pg_callback to let user to handle the
   * pixel group data in the payload, the callback should convert the pixel group data to
   * the data format app required.
   * Zero means the user frame mode is disabled.
   */
  size_t uframe_size;
  /**
   * Optional for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * User frame callback when lib receive pixel group data from network.
   * frame: point to the address of the user frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the pixel group successfully.
   *   < 0: the error code if app can't handle.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*uframe_pg_callback)(void* priv, void* frame, struct st20_rx_uframe_pg_meta* meta);
  /**
   * Optional for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL with
   * ST20_RX_FLAG_AUTO_DETECT. The callback when lib detected video format. And only
   * non-block method can be used in this callback as it run from lcore tasklet routine.
   */
  int (*notify_detected)(void* priv, const struct st20_detect_meta* meta,
                         struct st20_detect_reply* reply);
  /**
   * Optional only for ST20_TYPE_FRAME_LEVEL with ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*query_ext_frame)(void* priv, struct st20_ext_frame* ext_frame,
                         struct st20_rx_frame_meta* meta);

  /** Mandatory for ST20_TYPE_SLICE_LEVEL, lines in one slice */
  uint32_t slice_lines;
  /**
   * Mandatory for ST20_TYPE_SLICE_LEVEL.
   * the callback when lib received one more full slice info for a frame.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_slice_ready)(void* priv, void* frame, struct st20_rx_slice_meta* meta);

  /** Mandatory for ST20_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional for ST20_TYPE_RTP_LEVEL. The callback when lib receive one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
  /**  Use this socket if ST20_RX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;

  /* use to store framebuffers on vram */
  bool gpu_direct_framebuffer_in_vram_device_address;
  void* gpu_context;
};

/**
 * The structure describing how to create a rx st2110-22(compressed video) session.
 * Include the PCIE port and other required info.
 */
struct st22_rx_ops {
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

  /** Mandatory. Sender pacing type, default is narrow */
  enum st21_pacing pacing;
  /** Mandatory. Session streaming type, frame(default) or RTP */
  enum st22_type type;
  /** Mandatory. packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session resolution format */
  enum st20_fmt fmt;
  /** Mandatory. 7 bits payload type define in RFC3550. Zero means disable the
   * payload_type check on the RX pkt path */
  uint8_t payload_type;

  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Optional. Synchronization source defined in RFC3550, RX session will check the
   * incoming RTP packets match the ssrc. Leave to zero to disable the ssrc check */
  uint32_t ssrc;
  /** Optional. Name */
  const char* name;
  /** Optional. Private data to the cb functions(notify_frame_ready and others) */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST22_RX_FLAG_* for possible value
   */
  uint32_t flags;

  /**
   * Mandatory for ST22_TYPE_FRAME_LEVEL.
   * the frame buffer count requested for one st22 rx session,
   * should be in range [2, ST20_FB_MAX_COUNT].
   */
  uint16_t framebuff_cnt;
  /**
   * Mandatory for ST22_TYPE_FRAME_LEVEL. max framebuffer size for one st22 rx session,
   * usually ST22 use constant bitrate (CBR) mode.
   * lib will allocate all frame buffer with this size,
   * app can get the real codestream size later in notify_frame_ready callback.
   */
  size_t framebuff_max_size;
  /**
   * Mandatory for ST22_TYPE_FRAME_LEVEL. the callback when lib receive one frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st22_rx_put_framebuff
   * to return the frame when it finish the handling
   *   < 0: the error code if app can't handle, lib will free the frame then.
   * Only for ST22_TYPE_FRAME_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void* priv, void* frame, struct st22_rx_frame_meta* meta);

  /**
   * Optional. The event callback when there is some event(vsync or others) happened for
   * this session. Only non-block method can be used in this callback as it run from lcore
   * routine. Args point to the meta data of each event. Ex, cast to struct
   * st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /** Optional for ST22_RX_FLAG_ENABLE_RTCP, RTCP info */
  struct st_rx_rtcp_ops rtcp;

  /** Mandatory for ST22_TYPE_RTP_LEVEL. rtp ring queue size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * Optional for ST22_TYPE_RTP_LEVEL. The callback when lib receive one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
  /**  Use this socket if ST22_RX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/**
 * A structure used to retrieve general statistics(I/O) for a st20 tx session.
 */
struct st20_tx_user_stats {
  struct st_tx_user_stats common;
  uint64_t stat_pkts_dummy;
  uint64_t stat_epoch_troffset_mismatch;
  uint64_t stat_trans_troffset_mismatch;
  uint64_t stat_trans_recalculate_warmup;
  uint64_t stat_user_busy;
  uint64_t stat_lines_not_ready;
  uint64_t stat_vsync_mismatch;
  uint64_t stat_pkts_chain_realloc_fail;
  uint64_t stat_user_meta_cnt;
  uint64_t stat_user_meta_pkt_cnt;
  uint64_t stat_recoverable_error;
  uint64_t stat_unrecoverable_error;
  uint64_t stat_interlace_first_field;
  uint64_t stat_interlace_second_field;
};

/**
 * A structure used to retrieve general statistics(I/O) for a st20 rx session.
 */
struct st20_rx_user_stats {
  struct st_rx_user_stats common;
  uint64_t stat_bytes_received;
  uint64_t stat_slices_received;
  uint64_t stat_pkts_idx_dropped;
  uint64_t stat_pkts_offset_dropped;
  uint64_t stat_frames_dropped;
  uint64_t stat_pkts_idx_oo_bitmap;
  uint64_t stat_frames_pks_missed;
  uint64_t stat_pkts_rtp_ring_full;
  uint64_t stat_pkts_no_slot;
  uint64_t stat_pkts_redundant_dropped;
  uint64_t stat_pkts_wrong_interlace_dropped;
  uint64_t stat_pkts_wrong_len_dropped;
  uint64_t stat_pkts_enqueue_fallback;
  uint64_t stat_pkts_dma;
  uint64_t stat_pkts_slice_fail;
  uint64_t stat_pkts_slice_merged;
  uint64_t stat_pkts_multi_segments_received;
  uint64_t stat_pkts_not_bpm;
  uint64_t stat_pkts_wrong_payload_hdr_split;
  uint64_t stat_mismatch_hdr_split_frame;
  uint64_t stat_pkts_copy_hdr_split;
  uint64_t stat_vsync_mismatch;
  uint64_t stat_slot_get_frame_fail;
  uint64_t stat_slot_query_ext_fail;
  uint64_t stat_pkts_simulate_loss;
  uint64_t stat_pkts_user_meta;
  uint64_t stat_pkts_user_meta_err;
  uint64_t stat_pkts_retransmit;
  uint64_t stat_interlace_first_field;
  uint64_t stat_interlace_second_field;
  uint64_t stat_st22_boxes;
  uint64_t stat_burst_pkts_max;
  uint64_t stat_burst_succ_cnt;
  uint64_t stat_burst_pkts_sum;
  uint64_t incomplete_frames_cnt;
  uint64_t stat_pkts_wrong_kmod_dropped;
};

/**
 * Create one tx st2110-20(video) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-20(video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-20(video) session.
 */
st20_tx_handle st20_tx_create(mtl_handle mt, struct st20_tx_ops* ops);

/**
 * Free the tx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - 0: Success, tx st2110-20(video) session freed.
 *   - <0: Error code of the tx st2110-20(video) session free.
 */
int st20_tx_free(st20_tx_handle handle);

/**
 * Online update the destination info for the tx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param dst
 *   The pointer to the tx st2110-20(video) destination info.
 * @return
 *   - 0: Success, tx st2110-20(video) session destination update succ.
 *   - <0: Error code of the rx st2110-20(video) session destination update.
 */
int st20_tx_update_destination(st20_tx_handle handle, struct st_tx_dest_info* dst);

/**
 * Set the frame virtual address and iova from user for the tx st2110-20(video) session.
 * For ST20_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st20_tx_ops].
 * @param ext_frame
 *   The pointer to the structure describing external framebuffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if set fail.
 */
int st20_tx_set_ext_frame(st20_tx_handle handle, uint16_t idx,
                          struct st20_ext_frame* ext_frame);

/**
 * Get the framebuffer pointer from the tx st2110-20(video) session.
 * For ST20_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st20_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st20_tx_get_framebuffer(st20_tx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size for the tx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - size.
 */
size_t st20_tx_get_framebuffer_size(st20_tx_handle handle);

/**
 * Get the framebuffer count for the tx st2110-20(compressed video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(compressed video) session.
 * @return
 *   - count.
 */
int st20_tx_get_framebuffer_count(st20_tx_handle handle);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the tx st2110-20(video) session.
 * For ST20_TYPE_RTP_LEVEL.
 * Must call st20_tx_put_mbuf to return the mbuf after rtp pack done.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st20_tx_get_mbuf(st20_tx_handle handle, void** usrptr);

/**
 * Put back the mbuf which get by st20_tx_get_mbuf to the tx st2110-20(video) session.
 * For ST20_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st20_tx_get_mbuf.
 * @param len
 *   the rtp package length, include both header and payload.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st20_tx_put_mbuf(st20_tx_handle handle, void* mbuf, uint16_t len);

/**
 * Get the scheduler index for the tx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - >=0 the scheduler index.
 *   - <0: Error code.
 */
int st20_tx_get_sch_idx(st20_tx_handle handle);

/**
 * Retrieve pacing parameters for a tx st2110-20 session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param tr_offset_ns
 *   Optional output for the tr offset value in nanoseconds.
 * @param trs_ns
 *   Optional output for the packet spacing (TRS) value in nanoseconds.
 * @param vrx_pkts
 *   Optional output for the VRX packet count.
 *
 * @return
 *    0 on success, negative value otherwise.
 */
int st20_tx_get_pacing_params(st20_tx_handle handle, double* tr_offset_ns, double* trs_ns,
                              uint32_t* vrx_pkts);

/**
 * Retrieve the general statistics(I/O) for one tx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20_tx_get_session_stats(st20_tx_handle handle, struct st20_tx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one tx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20_tx_reset_session_stats(st20_tx_handle handle);

/**
 * Retrieve the pixel group info from st2110-20(video) format.
 *
 * @param fmt
 *   The st2110-20(video) format.
 * @param pg
 *   A pointer to a structure of type *st20_pgroup* to be filled.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st20_get_pgroup(enum st20_fmt fmt, struct st20_pgroup* pg);

/**
 * Retrieve the frame size from st2110-20(video) format.
 *
 * @param fmt
 *   The st2110-20(video) format.
 * @param width
 *   width.
 * @param height
 *   height.
 * @return
 *   - frame size
 */
size_t st20_frame_size(enum st20_fmt fmt, uint32_t width, uint32_t height);

/**
 * Retrieve bit rate(bit per second) for given st2110-20(video) format.
 *
 * @param width
 *   The st2110-20(video) width.
 * @param height
 *   The st2110-20(video) height.
 * @param fmt
 *   The st2110-20(video) format.
 * @param fps
 *   The st2110-20(video) fps.
 * @param interlaced
 *   If interlaced.
 * @param bps
 *   A pointer to the return bit rate.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st20_get_bandwidth_bps(int width, int height, enum st20_fmt fmt, enum st_fps fps,
                           bool interlaced, uint64_t* bps);

/**
 * Inline function returning bandwidth(mega per second) for 1080 p59 yuv422 10bit
 * @return
 *     Bandwidth(mega per second)
 */
static inline uint64_t st20_1080p59_yuv422_10bit_bandwidth_mps(void) {
  uint64_t bps;
  st20_get_bandwidth_bps(1920, 1080, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94, false, &bps);
  return bps / 1000 / 1000;
}

/**
 * Create one tx st2110-22(compressed video) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 * st2110-22(compressed video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-22(compressed video) session.
 */
st22_tx_handle st22_tx_create(mtl_handle mt, struct st22_tx_ops* ops);

/**
 * Online update the destination info for the tx st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param dst
 *   The pointer to the tx st2110-22(compressed video) destination info.
 * @return
 *   - 0: Success, tx st2110-22(compressed video) session destination update succ.
 *   - <0: Error code of the rx st2110-22(compressed video) session destination update.
 */
int st22_tx_update_destination(st22_tx_handle handle, struct st_tx_dest_info* dst);

/**
 * Free the tx st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @return
 *   - 0: Success, tx st2110-22(compressed video) session freed.
 *   - <0: Error code of the tx st2110-22(compressed video) session free.
 */
int st22_tx_free(st22_tx_handle handle);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the tx
 * st2110-22(compressed video) session.
 * Must call st22_tx_put_mbuf to return the mbuf after rtp pack done.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st22_tx_get_mbuf(st22_tx_handle handle, void** usrptr);

/**
 * Put back the mbuf which get by st22_tx_get_mbuf to the tx
 * st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st22_tx_get_mbuf.
 * @param len
 *   the rtp package length, include both header and payload.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22_tx_put_mbuf(st22_tx_handle handle, void* mbuf, uint16_t len);

/**
 * Get the scheduler index for the tx st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @return
 *   - >=0 the scheduler index.
 *   - <0: Error code.
 */
int st22_tx_get_sch_idx(st22_tx_handle handle);

/**
 * Get the framebuffer pointer from the tx st2110-22(video) session.
 * For ST22_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the tx st2110-22(video) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st22_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st22_tx_get_fb_addr(st22_tx_handle handle, uint16_t idx);

/**
 * Create one rx st2110-20(video) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx st2110-20(video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-20(video) session.
 */
st20_rx_handle st20_rx_create(mtl_handle mt, struct st20_rx_ops* ops);

/**
 * Online update the source info for the rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param src
 *   The pointer to the rx st2110-20(video) source info.
 * @return
 *   - 0: Success, rx st2110-20(video) session source update succ.
 *   - <0: Error code of the rx st2110-20(video) session source update.
 */
int st20_rx_update_source(st20_rx_handle handle, struct st_rx_source_info* src);

/**
 * Get the scheduler index for the rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @return
 *   - >=0 the scheduler index.
 *   - <0: Error code.
 */
int st20_rx_get_sch_idx(st20_rx_handle handle);

/**
 * Dump st2110-20 packets to pcapng file.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param max_dump_packets
 *   The max number of packets to be dumped.
 * @param sync
 *   synchronous or asynchronous, true means this func will return after dump
 * progress is finished.
 * @param meta
 *   The meta data returned, only for synchronous, leave to NULL if not need the meta.
 * @return
 *   - 0: Success, rx st2110-20(video) session pcapng dump succ.
 *   - <0: Error code of the rx st2110-20(video) session pcapng dump.
 */
int st20_rx_pcapng_dump(st20_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta);

/**
 * Free the rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @return
 *   - 0: Success, rx st2110-20(video) session freed.
 *   - <0: Error code of the rx st2110-20(video) session free.
 */
int st20_rx_free(st20_rx_handle handle);

/**
 * Get the framebuffer size for the rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - size.
 */
size_t st20_rx_get_framebuffer_size(st20_rx_handle handle);

/**
 * Get the framebuffer count for the rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @return
 *   - count.
 */
int st20_rx_get_framebuffer_count(st20_rx_handle handle);

/**
 * Put back the received buff get from notify_frame_ready.
 * For ST20_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param frame
 *   The framebuffer pointer.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20_rx_put_framebuff(st20_rx_handle handle, void* frame);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the rx st2110-20(video) session.
 * For ST20_TYPE_RTP_LEVEL.
 * Must call st20_rx_put_mbuf to return the mbuf after consume it.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @param len
 *   The length of the rtp packet, include both the header and payload.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st20_rx_get_mbuf(st20_rx_handle handle, void** usrptr, uint16_t* len);

/**
 * Put back the mbuf which get by st20_rx_get_mbuf to the rx st2110-20(video) session.
 * For ST20_TYPE_RTP_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st20_rx_get_mbuf.
 */
void st20_rx_put_mbuf(st20_rx_handle handle, void* mbuf);

/**
 * Get the queue meta attached to rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20_rx_get_queue_meta(st20_rx_handle handle, struct st_queue_meta* meta);

/**
 * Check if dma is enabled or not.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @return
 *   - true: dma enabled.
 *   - false: no dma.
 */
bool st20_rx_dma_enabled(st20_rx_handle handle);

/**
 * Get the timing parser pass critical to rx st2110-20(video) session.
 * Only avaiable if ST20_RX_FLAG_TIMING_PARSER_META is enabled.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param pass
 *   the pointer to save the timing parser pass critical.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20_rx_timing_parser_critical(st20_rx_handle handle, struct st20_rx_tp_pass* pass);

/**
 * Retrieve the general statistics(I/O) for one rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20_rx_get_session_stats(st20_rx_handle handle, struct st20_rx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-20(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20_rx_reset_session_stats(st20_rx_handle handle);

/**
 * Create one rx st2110-22(compressed video) session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-22(compressed video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-22(compressed video) session.
 */
st22_rx_handle st22_rx_create(mtl_handle mt, struct st22_rx_ops* ops);

/**
 * Online update the source info for the rx st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the rx st2110-22(compressed video) session.
 * @param src
 *   The pointer to the rx st2110-22(compressed video) source info.
 * @return
 *   - 0: Success, rx st2110-22(video) session source update succ.
 *   - <0: Error code of the rx st2110-22(compressed video) session source update.
 */
int st22_rx_update_source(st22_rx_handle handle, struct st_rx_source_info* src);

/**
 * Get the scheduler index for the rx st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the rx st2110-22(compressed video) session.
 * @return
 *   - >=0 the scheduler index.
 *   - <0: Error code.
 */
int st22_rx_get_sch_idx(st22_rx_handle handle);

/**
 * Dump st2110-22 packets to pcapng file.
 *
 * @param handle
 *   The handle to the rx st2110-22(compressed video) session.
 * @param max_dump_packets
 *   The max number of packets to be dumped.
 * @param sync
 *   synchronous or asynchronous, true means this func will return after dump
 * progress is finished.
 * @param meta
 *   The meta data returned, only for synchronous, leave to NULL if not need the meta.
 * @return
 *   - 0: Success, rx st2110-22 session pcapng dump succ.
 *   - <0: Error code of the rx st2110-22 session pcapng dump.
 */
int st22_rx_pcapng_dump(st22_rx_handle handle, uint32_t max_dump_packets, bool sync,
                        struct st_pcap_dump_meta* meta);

/**
 * Free the rx st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the rx st2110-22(compressed video) session.
 * @return
 *   - 0: Success, rx st2110-22(compressed video) session freed.
 *   - <0: Error code of the rx st2110-22(compressed video) session free.
 */
int st22_rx_free(st22_rx_handle handle);

/**
 * Get the mbuf pointer and usrptr of the mbuf from the
 * rx st2110-22(compressed video) session.
 * Must call st22_rx_put_mbuf to return the mbuf after consume it.
 *
 * @param handle
 *   The handle to the tx st2110-22(compressed video) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @param len
 *   The length of the rtp packet, include both the header and payload.
 * @return
 *   - NULL if no available mbuf in the ring.
 *   - Otherwise, the dpdk mbuf pointer.
 */
void* st22_rx_get_mbuf(st22_rx_handle handle, void** usrptr, uint16_t* len);

/**
 * Put back the mbuf which get by st22_rx_get_mbuf to the rx
 * st2110-22(compressed video) session.
 *
 * @param handle
 *   The handle to the rx st2110-22(compressed video) session.
 * @param mbuf
 *   the dpdk mbuf pointer by st22_rx_get_mbuf.
 */
void st22_rx_put_mbuf(st22_rx_handle handle, void* mbuf);

/**
 * Put back the received buff get from notify_frame_ready.
 * For ST22_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-22(video) session.
 * @param frame
 *   The framebuffer pointer.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22_rx_put_framebuff(st22_rx_handle handle, void* frame);

/**
 * Get the framebuffer pointer from the rx st2110-22(video) session.
 * For ST22_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-22(video) session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st22_rx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st22_rx_get_fb_addr(st22_rx_handle handle, uint16_t idx);

/**
 * Get the queue meta attached to rx st2110-22(video) session.
 *
 * @param handle
 *   The handle to the rx st2110-22(video) session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22_rx_get_queue_meta(st22_rx_handle handle, struct st_queue_meta* meta);

/**
 * Get the name of st20_fmt
 *
 * @param fmt
 *   format.
 * @return
 *   The pointer to name.
 *   NULL: Fail.
 */
const char* st20_fmt_name(enum st20_fmt fmt);

/**
 * Get st20_fmt from name
 *
 * @param name
 *   name.
 * @return
 *   The st20_fmt.
 *   ST20_FMT_MAX: Fail.
 */
enum st20_fmt st20_name_to_fmt(const char* name);

#if defined(__cplusplus)
}
#endif

#endif
