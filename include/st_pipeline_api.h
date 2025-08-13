/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2022 Intel Corporation
 */

/**
 * @file st_pipeline_api.h
 *
 * Interfaces for st2110-20/22 pipeline transport.
 * It include a plugin layer to hide the covert/encode detail that application can
 * focus on the raw pixel handling.
 *
 */

#include "st20_api.h"

#ifndef _ST_PIPELINE_API_HEAD_H_
#define _ST_PIPELINE_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Handle to tx st2110-22 pipeline session of lib */
typedef struct st22p_tx_ctx* st22p_tx_handle;
/** Handle to rx st2110-22 pipeline session of lib */
typedef struct st22p_rx_ctx* st22p_rx_handle;
/** Handle to tx st2110-20 pipeline session of lib */
typedef struct st20p_tx_ctx* st20p_tx_handle;
/** Handle to rx st2110-20 pipeline session of lib */
typedef struct st20p_rx_ctx* st20p_rx_handle;

/** Handle to st2110-22 encode device of lib */
typedef struct st22_encode_dev_impl* st22_encoder_dev_handle;
/** Handle to st2110-22 decode device of lib */
typedef struct st22_decode_dev_impl* st22_decoder_dev_handle;
/** Handle to st2110-20 convert device of lib */
typedef struct st20_convert_dev_impl* st20_converter_dev_handle;

/** Handle to the st22 encode session private data */
typedef void* st22_encode_priv;

/** Handle to the st2110-22 pipeline encode session of lib */
typedef struct st22_encode_session_impl* st22p_encode_session;

/** Handle to the st22 decode session private data */
typedef void* st22_decode_priv;

/** Handle to the st2110-22 pipeline decode session of lib */
typedef struct st22_decode_session_impl* st22p_decode_session;

/** Handle to the st20 convert session private data */
typedef void* st20_convert_priv;

/** Handle to the st2110-20 pipeline convert session of lib */
typedef struct st20_convert_session_impl* st20p_convert_session;

/** Handle to the private data of plugin */
typedef void* st_plugin_priv;

/** Version type of st plugin */
enum st_plugin_version {
  /** auto */
  ST_PLUGIN_VERSION_UNKNOWN = 0,
  /** V1 */
  ST_PLUGIN_VERSION_V1,
  /** max value of this enum */
  ST_PLUGIN_VERSION_MAX,
};

/** Macro to compute a plugin magic */
#define ST_PLUGIN_MAGIC(a, b, c, d) ((a) << 24 | (b) << 16 | (c) << 16 | (d))

/** Macro to plugin magic of V1 */
#define ST_PLUGIN_VERSION_V1_MAGIC ST_PLUGIN_MAGIC('p', 'l', 'v', '1')

/** The structure info for plugin meta. */
struct st_plugin_meta {
  /** plugin version */
  enum st_plugin_version version;
  /** plugin magic */
  uint32_t magic;
};

/** Get meta function prototype of plugin */
typedef int (*st_plugin_get_meta_fn)(struct st_plugin_meta* meta);
/** Get meta function name of plugin */
#define ST_PLUGIN_GET_META_API "st_plugin_get_meta"
/** Create function prototype of plugin */
typedef st_plugin_priv (*st_plugin_create_fn)(mtl_handle mt);
/** Create function name of plugin */
#define ST_PLUGIN_CREATE_API "st_plugin_create"
/** Free function prototype of plugin */
typedef int (*st_plugin_free_fn)(st_plugin_priv handle);
/** Free function name of plugin */
#define ST_PLUGIN_FREE_API "st_plugin_free"

/** Frame format */
enum st_frame_fmt {
  /** Start of yuv format list */
  ST_FRAME_FMT_YUV_START = 0,
  /** YUV 422 planar 10bit little endian */
  ST_FRAME_FMT_YUV422PLANAR10LE = 0,
  /** YUV 422 packed, 3 samples on a 32-bit word, 10 bits per sample */
  ST_FRAME_FMT_V210 = 1,
  /** YUV 422 packed, 16 bits per sample with least significant 6 paddings */
  ST_FRAME_FMT_Y210 = 2,
  /** YUV 422 planar 8bit */
  ST_FRAME_FMT_YUV422PLANAR8 = 3,
  /** YUV 422 packed 8bit(aka ST20_FMT_YUV_422_8BIT) */
  ST_FRAME_FMT_UYVY = 4,
  /**
   * RFC4175 in ST2110(ST20_FMT_YUV_422_10BIT),
   * two YUV 422 10 bit pixel groups on 5 bytes, big endian
   */
  ST_FRAME_FMT_YUV422RFC4175PG2BE10 = 5,
  /** YUV 422 planar 12bit little endian */
  ST_FRAME_FMT_YUV422PLANAR12LE = 6,
  /**
   * RFC4175 in ST2110(ST20_FMT_YUV_422_12BIT),
   * two YUV 422 12 bit pixel groups on 6 bytes, big endian
   */
  ST_FRAME_FMT_YUV422RFC4175PG2BE12 = 7,
  /** YUV 444 planar 10bit little endian */
  ST_FRAME_FMT_YUV444PLANAR10LE = 8,
  /**
   * RFC4175 in ST2110(ST20_FMT_YUV_444_10BIT),
   * four YUV 444 10 bit pixel groups on 15 bytes, big endian
   */
  ST_FRAME_FMT_YUV444RFC4175PG4BE10 = 9,
  /** YUV 444 planar 12bit little endian */
  ST_FRAME_FMT_YUV444PLANAR12LE = 10,
  /**
   * RFC4175 in ST2110(ST20_FMT_YUV_444_12BIT),
   * two YUV 444 12 bit pixel groups on 9 bytes, big endian
   */
  ST_FRAME_FMT_YUV444RFC4175PG2BE12 = 11,
  /** Customized YUV 420 8bit, set transport format as ST20_FMT_YUV_420_8BIT.
   * This is used when user wants to directly transport none-RFC4175 formats like
   * I420/NV12. When this input/output format is set, the frame is identical to
   * transport frame without conversion. The frame should not have lines padding.
   */
  ST_FRAME_FMT_YUV420CUSTOM8 = 12,
  /** Customized YUV 422 8bit, set transport format as ST20_FMT_YUV_422_8BIT.
   * This is used when user wants to directly transport none-RFC4175 formats like
   * YUY2. When this input/output format is set, the frame is identical to
   * transport frame without conversion. The frame should not have lines padding.
   */
  ST_FRAME_FMT_YUV422CUSTOM8 = 13,
  /** YUV 420 planar 8bit */
  ST_FRAME_FMT_YUV420PLANAR8 = 14,
  /** YUV 422 planar 10bit little endian, with 6-bit padding in least significant bits*/
  ST_FRAME_FMT_YUV422PLANAR16LE = 15,
  /** End of yuv format list, new yuv should be inserted before this */
  ST_FRAME_FMT_YUV_END,

  /** Start of rgb format list */
  ST_FRAME_FMT_RGB_START = 32,
  /** one ARGB pixel per 32 bit word, 8 bits per sample */
  ST_FRAME_FMT_ARGB = 32,
  /** one BGRA pixel per 32 bit word, 8 bits per sample */
  ST_FRAME_FMT_BGRA = 33,
  /** one RGB pixel per 24 bit word, 8 bits per sample(aka ST20_FMT_RGB_8BIT) */
  ST_FRAME_FMT_RGB8 = 34,
  /** GBR planar 10bit little endian */
  ST_FRAME_FMT_GBRPLANAR10LE = 35,
  /**
   * RFC4175 in ST2110(ST20_FMT_RGB_10BIT),
   * four RGB 10 bit pixel groups on 15 bytes, big endian
   */
  ST_FRAME_FMT_RGBRFC4175PG4BE10 = 36,
  /** GBR planar 12bit little endian */
  ST_FRAME_FMT_GBRPLANAR12LE = 37,
  /**
   * RFC4175 in ST2110(ST20_FMT_RGB_12BIT),
   * two RGB 12 bit pixel groups on 9 bytes, big endian
   */
  ST_FRAME_FMT_RGBRFC4175PG2BE12 = 38,
  /** End of rgb format list, new rgb should be inserted before this */
  ST_FRAME_FMT_RGB_END,

  /** Start of codestream format list */
  ST_FRAME_FMT_CODESTREAM_START = 56,
  /** ST22 jpegxs codestream */
  ST_FRAME_FMT_JPEGXS_CODESTREAM = 56,
  /** ST22 h264 cbr codestream */
  ST_FRAME_FMT_H264_CBR_CODESTREAM = 57,
  /** ST22 h264 codestream */
  ST_FRAME_FMT_H264_CODESTREAM = 58,
  /** ST22 h265 codestream */
  ST_FRAME_FMT_H265_CBR_CODESTREAM = 59,
  /** ST22 h265 codestream */
  ST_FRAME_FMT_H265_CODESTREAM = 60,
  /** End of codestream format list */
  ST_FRAME_FMT_CODESTREAM_END,
  /** max value(< sizeof(uint64_t)) of this enum */
  ST_FRAME_FMT_MAX,
};

/** ST format cap of ST_FRAME_FMT_YUV422PLANAR10LE */
#define ST_FMT_CAP_YUV422PLANAR10LE (MTL_BIT64(ST_FRAME_FMT_YUV422PLANAR10LE))
/** ST format cap of ST_FRAME_FMT_V210 */
#define ST_FMT_CAP_V210 (MTL_BIT64(ST_FRAME_FMT_V210))
/** ST format cap of ST_FRAME_FMT_Y210 */
#define ST_FMT_CAP_Y210 (MTL_BIT64(ST_FRAME_FMT_Y210))
/** ST format cap of ST_FRAME_FMT_YUV422PLANAR8 */
#define ST_FMT_CAP_YUV422PLANAR8 (MTL_BIT64(ST_FRAME_FMT_YUV422PLANAR8))
/** ST format cap of ST_FRAME_FMT_YUV420PLANAR8 */
#define ST_FMT_CAP_YUV420PLANAR8 (MTL_BIT64(ST_FRAME_FMT_YUV420PLANAR8))
/** ST format cap of ST_FRAME_FMT_UYVY */
#define ST_FMT_CAP_UYVY (MTL_BIT64(ST_FRAME_FMT_UYVY))
/** ST format cap of ST_FRAME_FMT_YUV422RFC4175PG2BE10 */
#define ST_FMT_CAP_YUV422RFC4175PG2BE10 (MTL_BIT64(ST_FRAME_FMT_YUV422RFC4175PG2BE10))
/** ST format cap of ST_FRAME_FMT_YUV422PLANAR16LE (10 bit with 6 bit padding)*/
#define ST_FMT_CAP_YUV422PLANAR16LE (MTL_BIT64(ST_FRAME_FMT_YUV422PLANAR16LE))

/** ST format cap of ST_FRAME_FMT_ARGB */
#define ST_FMT_CAP_ARGB (MTL_BIT64(ST_FRAME_FMT_ARGB))
/** ST format cap of ST_FRAME_FMT_ARGB */
#define ST_FMT_CAP_BGRA (MTL_BIT64(ST_FRAME_FMT_BGRA))
/** ST format cap of ST_FRAME_FMT_RGB8 */
#define ST_FMT_CAP_RGB8 (MTL_BIT64(ST_FRAME_FMT_RGB8))

/** ST format cap of ST_FRAME_FMT_JPEGXS_CODESTREAM, used in the st22_plugin caps */
#define ST_FMT_CAP_JPEGXS_CODESTREAM (MTL_BIT64(ST_FRAME_FMT_JPEGXS_CODESTREAM))
/** ST format cap of ST_FRAME_FMT_H264_CBR_CODESTREAM, used in the st22_plugin caps */
#define ST_FMT_CAP_H264_CBR_CODESTREAM (MTL_BIT64(ST_FRAME_FMT_H264_CBR_CODESTREAM))
/** ST format cap of ST_FRAME_FMT_H264_CODESTREAM, used in the st22_plugin caps */
#define ST_FMT_CAP_H264_CODESTREAM (MTL_BIT64(ST_FRAME_FMT_H264_CODESTREAM))
/** ST format cap of ST_FRAME_FMT_H265_CBR_CODESTREAM, used in the st22_plugin caps */
#define ST_FMT_CAP_H265_CBR_CODESTREAM (MTL_BIT64(ST_FRAME_FMT_H265_CBR_CODESTREAM))
/** ST format cap of ST_FRAME_FMT_H265_CODESTREAM, used in the st22_plugin caps */
#define ST_FMT_CAP_H265_CODESTREAM (MTL_BIT64(ST_FRAME_FMT_H265_CODESTREAM))

/** Flag bit in flags of struct st_frame. */
enum st_frame_flag {
  /** Frame has external buffer attached. */
  ST_FRAME_FLAG_EXT_BUF = (MTL_BIT32(0)),
  /** Frame planes data by single malloc */
  ST_FRAME_FLAG_SINGLE_MALLOC = (MTL_BIT32(1)),
  /** Frame planes data by rte_malloc */
  ST_FRAME_FLAG_RTE_MALLOC = (MTL_BIT32(2)),
};

/** Max planes number for one frame */
#define ST_MAX_PLANES (4)

/** The structure info for external frame */
struct st_ext_frame { /** Each plane's virtual address of external frame */
  void* addr[ST_MAX_PLANES];
  /** Each plane's IOVA of external frame */
  mtl_iova_t iova[ST_MAX_PLANES];
  /** Each plane's linesize of external frame,
   * if no padding, can be calculated from st_frame_least_linesize */
  size_t linesize[ST_MAX_PLANES];
  /** Buffer size of external frame */
  size_t size;
  /** Private data for user */
  void* opaque;
};

/** The structure info for frame meta. */
struct st_frame {
  /** frame buffer address of each plane */
  void* addr[ST_MAX_PLANES];
  /** frame buffer IOVA of each plane */
  mtl_iova_t iova[ST_MAX_PLANES];
  /** frame buffer linesize of each plane */
  size_t linesize[ST_MAX_PLANES];
  /** frame format */
  enum st_frame_fmt fmt;
  /** interlace or not, false: non-interlaced: true: interlaced */
  bool interlaced;
  /** Second field type indicate for interlaced mode, for tx it's set by user */
  bool second_field;
  /** frame buffer size, include all planes */
  size_t buffer_size;
  /**
   * frame valid data size, may <= buffer_size for one encoded frame.
   * encode dev put the real codestream size here.
   * Same for decode.
   */
  size_t data_size;
  /** frame resolution width */
  uint32_t width;
  /** frame resolution height */
  uint32_t height;
  /** frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** frame timestamp value */
  uint64_t timestamp;
  /** epoch info for the done frame */
  uint64_t epoch;
  /** Timestamp value in the rtp header */
  uint32_t rtp_timestamp;
  /** flags, value in ST_FRAME_FLAG_* */
  uint32_t flags;
  /** frame status, complete or not */
  enum st_frame_status status;

  /**
   * The user meta data buffer for current frame of st20, the size must smaller than
   * MTL_PKT_MAX_RTP_BYTES. This data will be transported to RX with video data and passed
   * to user by user_meta also.
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

  /** priv pointer for lib, do not touch this */
  void* priv;
  /** priv data for user */
  void* opaque;
  /** timing parser meta for st20p_rx_get_frame, only active if
   * ST20P_RX_FLAG_TIMING_PARSER_META */
  struct st20_rx_tp_meta* tp[MTL_SESSION_PORT_MAX];
  /** TAI timestamp measured right after first packet of the frame was received */
  uint64_t receive_timestamp;
};

/** Device type of st plugin */
enum st_plugin_device {
  /** auto */
  ST_PLUGIN_DEVICE_AUTO = 0,
  /** CPU */
  ST_PLUGIN_DEVICE_CPU,
  /** GPU */
  ST_PLUGIN_DEVICE_GPU,
  /** FPGA */
  ST_PLUGIN_DEVICE_FPGA,
  /** For test only, don't use */
  ST_PLUGIN_DEVICE_TEST,
  /** For test only, don't use */
  ST_PLUGIN_DEVICE_TEST_INTERNAL,
  /** max value of this enum */
  ST_PLUGIN_DEVICE_MAX,
};

/** Codec type of st22 */
enum st22_codec {
  /** jpegxs codec */
  ST22_CODEC_JPEGXS = 0,
  /** h264 cbr codec */
  ST22_CODEC_H264_CBR,
  /** h264 codec */
  ST22_CODEC_H264,
  /** h265 cbr codec */
  ST22_CODEC_H265_CBR,
  /** h265 codec */
  ST22_CODEC_H265,
  /** max value of this enum */
  ST22_CODEC_MAX,
};

/** Quality mode type of st22, speed or quality */
enum st22_quality_mode {
  /** speed mode */
  ST22_QUALITY_MODE_SPEED = 0,
  /** quality mode */
  ST22_QUALITY_MODE_QUALITY,
  /** max value of this enum */
  ST22_QUALITY_MODE_MAX,
};

/** Bit define for flags of struct st22p_tx_ops. */
enum st22p_tx_flag {
  /**
   * P TX destination mac assigned by user
   */
  ST22P_TX_FLAG_USER_P_MAC = (MTL_BIT32(0)),
  /**
   * R TX destination mac assigned by user
   */
  ST22P_TX_FLAG_USER_R_MAC = (MTL_BIT32(1)),
  /**
   * Disable ST22 boxes
   */
  ST22P_TX_FLAG_DISABLE_BOXES = (MTL_BIT32(2)),
  /**
   * User control the frame pacing by pass a timestamp in st_frame,
   * lib will wait until timestamp is reached for each frame.
   */
  ST22P_TX_FLAG_USER_PACING = (MTL_BIT32(3)),
  /**
   * Drop frames when the mtl reports late frames (transport can't keep up).
   * When late frame is detected, next frame from pipeline is ommited.
   * Untill we resume normal frame sending.
   */
  ST22P_TX_FLAG_DROP_WHEN_LATE = (MTL_BIT32(12)),
  /**
   * If enabled, lib will assign the rtp timestamp to the value in
   * tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
   */
  ST22P_TX_FLAG_USER_TIMESTAMP = (MTL_BIT32(4)),
  /**
   * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
   */
  ST22P_TX_FLAG_ENABLE_VSYNC = (MTL_BIT32(5)),
  /**
   * If enable the rtcp.
   */
  ST22P_TX_FLAG_ENABLE_RTCP = (MTL_BIT32(6)),
  /**
   * Set this flag to the bulk operation on all internal buffer rings. It may degrade the
   * performance since the object enqueue/dequeue will be acted one by one.
   */
  ST22P_TX_FLAG_DISABLE_BULK = (MTL_BIT32(7)),
  /**
   * Lib uses user dynamic allocated memory for frames.
   * The external frames are provided by calling
   * st22p_tx_put_ext_frame.
   */
  ST22P_TX_FLAG_EXT_FRAME = (MTL_BIT32(8)),
  /** Force the numa of the created session, both CPU and memory */
  ST22P_TX_FLAG_FORCE_NUMA = (MTL_BIT32(9)),
  /** Enable the st22p_tx_get_frame block behavior to wait until a frame becomes
   available or timeout(default: 1s, use st22p_tx_set_block_timeout to customize) */
  ST22P_TX_FLAG_BLOCK_GET = (MTL_BIT32(15))
};

/** Bit define for flags of struct st20p_tx_ops. */
enum st20p_tx_flag {
  /**
   * P TX destination mac assigned by user
   */
  ST20P_TX_FLAG_USER_P_MAC = (MTL_BIT32(0)),
  /**
   * R TX destination mac assigned by user
   */
  ST20P_TX_FLAG_USER_R_MAC = (MTL_BIT32(1)),
  /**
   * Lib uses user dynamic allocated memory for frames.
   * The external frames are provided by calling
   * st20p_tx_put_ext_frame.
   */
  ST20P_TX_FLAG_EXT_FRAME = (MTL_BIT32(2)),
  /**
   * User control frame transmission time by pass a timestamp in st_frame.timestamp,
   * lib will wait until timestamp is reached for each frame. The time of sending is
   * aligned with virtual receiver read schedule.
   */
  ST20P_TX_FLAG_USER_PACING = (MTL_BIT32(3)),
  /**
   * Drop frames when the mtl reports late frames (transport can't keep up).
   * When late frame is detected, next frame from pipeline is ommited.
   * Untill we resume normal frame sending.
   */
  ST20P_TX_FLAG_DROP_WHEN_LATE = (MTL_BIT32(12)),
  /**
   * If enabled, lib will assign the rtp timestamp to the value of timestamp in
   * st_frame.timestamp (if needed the value will be converted to
   * ST10_TIMESTAMP_FMT_MEDIA_CLK)
   */
  ST20P_TX_FLAG_USER_TIMESTAMP = (MTL_BIT32(4)),
  /**
   * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
   */
  ST20P_TX_FLAG_ENABLE_VSYNC = (MTL_BIT32(5)),
  /**
   * If enable the static RL pad interval profiling.
   * Static padding is trained only for e810, it is not recommended to use this flag
   * for other NICs.
   */
  ST20P_TX_FLAG_ENABLE_STATIC_PAD_P = (MTL_BIT32(6)),
  /**
   * If enable the rtcp.
   */
  ST20P_TX_FLAG_ENABLE_RTCP = (MTL_BIT32(7)),
  /**
   * It changes how ST20_TX_FLAG_USER_PACING works. if enabled, it does not align the
   * transmission time to the virtual receiver read schedule. The
   * first packet of the frame will be sent exactly at the time specified by the user.
   */
  ST20P_TX_FLAG_EXACT_USER_PACING = (MTL_BIT32(8)),
  /**
   * If enabled the RTP timestamp will be set exactly to epoch + N *
   * frame_time, omitting TR_offset.
   */
  ST20P_TX_FLAG_RTP_TIMESTAMP_EPOCH = (MTL_BIT32(9)),
  /**
   * Set this flag to the bulk operation on all internal buffer rings. It may degrade the
   * performance since the object enqueue/dequeue will be acted one by one.
   */
  ST20P_TX_FLAG_DISABLE_BULK = (MTL_BIT32(10)),
  /** Force the numa of the created session, both CPU and memory */
  ST20P_TX_FLAG_FORCE_NUMA = (MTL_BIT32(11)),
  /** Enable the st20p_tx_get_frame block behavior to wait until a frame becomes
     available or (default: 1s, use st20p_tx_set_block_timeout to customize) */
  ST20P_TX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
};

/** Bit define for flags of struct st22p_rx_ops. */
enum st22p_rx_flag {
  /**
   * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
   * Use st22p_rx_get_queue_meta to get the queue meta(queue number etc) info.
   */
  ST22P_RX_FLAG_DATA_PATH_ONLY = (MTL_BIT32(0)),
  /**
   * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
   */
  ST22P_RX_FLAG_ENABLE_VSYNC = (MTL_BIT32(1)),
  /**
   * If enable the rtcp.
   */
  ST22P_RX_FLAG_ENABLE_RTCP = (MTL_BIT32(2)),
  /**
   * Flag bit in flags of struct st22p_rx_ops.
   * If enabled, simulate random packet loss, test usage only.
   */
  ST22P_RX_FLAG_SIMULATE_PKT_LOSS = (MTL_BIT32(3)),
  /**
   * Enable the dynamic external frame mode, and user must provide a query
   * callback(query_ext_frame in st22p_rx_ops) to let MTL can get the frame when needed.
   */
  ST22P_RX_FLAG_EXT_FRAME = (MTL_BIT32(4)),
  /** Force the numa of the created session, both CPU and memory */
  ST22P_RX_FLAG_FORCE_NUMA = MTL_BIT32(5),

  /** Enable the st22p_rx_get_frame block behavior to wait until a frame becomes
     available or timeout(default: 1s, use st22p_rx_set_block_timeout to customize) */
  ST22P_RX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
  /**
   * If set, lib will pass the incomplete frame to app also.
   * User can check st_frame_status data for the frame integrity
   */
  ST22P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME = (MTL_BIT32(16)),
};

/** Bit define for flags of struct st20p_rx_ops. */
enum st20p_rx_flag {
  /**
   * for non MTL_PMD_DPDK_USER.
   * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
   * Use st20p_rx_get_queue_meta to get the queue meta(queue number etc) info.
   */
  ST20P_RX_FLAG_DATA_PATH_ONLY = (MTL_BIT32(0)),
  /**
   * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
   */
  ST20P_RX_FLAG_ENABLE_VSYNC = (MTL_BIT32(1)),
  /**
   * Enable the dynamic external frame mode, and user must provide a query
   * callback(query_ext_frame in st20p_rx_ops) to let MTL can get the frame when needed.
   * Note to enable ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME also for non-converter mode.
   */
  ST20P_RX_FLAG_EXT_FRAME = (MTL_BIT32(2)),
  /**
   * Only used for internal convert mode and limited formats:
   * ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210, ST_FRAME_FMT_UYVY
   * Perform the color format conversion on each packet.
   */
  ST20P_RX_FLAG_PKT_CONVERT = (MTL_BIT32(3)),
  /**
   * If enable the rtcp.
   */
  ST20P_RX_FLAG_ENABLE_RTCP = (MTL_BIT32(4)),
  /**
   * Flag bit in flags of struct st20p_rx_ops.
   * If enabled, simulate random packet loss, test usage only.
   */
  ST20P_RX_FLAG_SIMULATE_PKT_LOSS = (MTL_BIT32(5)),
  /** Force the numa of the created session, both CPU and memory */
  ST20P_RX_FLAG_FORCE_NUMA = (MTL_BIT32(6)),

  /** Enable the st20p_rx_get_frame block behavior to wait until a frame becomes
     available or (default: 1s, use st20p_rx_set_block_timeout to customize) */
  ST20P_RX_FLAG_BLOCK_GET = (MTL_BIT32(15)),
  /**
   * If set, lib will pass the incomplete frame to app also.
   * User can check st_frame_status data for the frame integrity
   */
  ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME = (MTL_BIT32(16)),
  /**
   * If set, lib will try to allocate DMA memory copy offload from
   * dma_dev_port(mtl_init_params) list.
   * Pls note it could fallback to CPU if no DMA device is available.
   */
  ST20P_RX_FLAG_DMA_OFFLOAD = (MTL_BIT32(17)),
  /**
   * Flag bit in flags of struct st20p_rx_ops.
   * If set, lib will automatically detect video format.
   * Width, height and fps set by app will be invalid.
   */
  ST20P_RX_FLAG_AUTO_DETECT = (MTL_BIT32(18)),
  /**
   * Flag bit in flags of struct st20p_rx_ops.
   * Only ST20_PACKING_BPM stream can enable this offload as software limit
   * Try to enable header split offload feature.
   */
  ST20P_RX_FLAG_HDR_SPLIT = (MTL_BIT32(19)),
  /**
   * Only for MTL_FLAG_RX_VIDEO_MIGRATE is enabled.
   * Always disable MIGRATE for this session.
   */
  ST20P_RX_FLAG_DISABLE_MIGRATE = (MTL_BIT32(20)),
  /**
   * Enable the timing analyze info in the stat dump
   */
  ST20P_RX_FLAG_TIMING_PARSER_STAT = (MTL_BIT32(21)),
  /**
   * Enable the timing analyze info in the the return `struct st_frame` by the
   * st20p_rx_get_frame
   */
  ST20P_RX_FLAG_TIMING_PARSER_META = (MTL_BIT32(22)),
  /**
   * Force to use multi(only two now) threads for the rx packet processing
   */
  ST20P_RX_FLAG_USE_MULTI_THREADS = (MTL_BIT32(23)),
  /**
   * Use gpu_direct vram for framebuffers
   */
  ST20P_RX_FLAG_USE_GPU_DIRECT_FRAMEBUFFERS = (MTL_BIT32(24)),
};

/** Bit define for flag_resp of struct st22_decoder_create_req. */
enum st22_decoder_resp_flag {
  /** Enable the st22_decoder_get_frame block behavior to wait until a frame becomes
     available or timeout(default: 1s, use st22_decoder_set_block_timeout to customize)
   */
  ST22_DECODER_RESP_FLAG_BLOCK_GET = (MTL_BIT32(0)),
};

/** Bit define for flag_resp of struct st22_encoder_create_req. */
enum st22_encoder_resp_flag {
  /** Enable the st22_encoder_get_frame block behavior to wait until a frame becomes
     available or timeout(default: 1s, use st22_encoder_set_block_timeout to customize)
   */
  ST22_ENCODER_RESP_FLAG_BLOCK_GET = (MTL_BIT32(0)),
};

/** The structure info for st plugin encode session create request. */
struct st22_encoder_create_req {
  /** codestream size required */
  size_t codestream_size;
  /** Session resolution width, set by lib */
  uint32_t width;
  /** Session resolution height, set by lib */
  uint32_t height;
  /** Session resolution fps, set by lib */
  enum st_fps fps;
  /** Interlaced or not, set by lib */
  bool interlaced;
  /** Session input frame format, set by lib */
  enum st_frame_fmt input_fmt;
  /** Session output frame format, set by lib */
  enum st_frame_fmt output_fmt;
  /** speed or quality mode, set by lib */
  enum st22_quality_mode quality;
  /** frame buffer count, set by lib */
  uint16_t framebuff_cnt;
  /** thread count, set by lib */
  uint32_t codec_thread_cnt;
  /** max size for frame(encoded code stream), set by plugin */
  size_t max_codestream_size;
  /** the flag indicated by plugin to customize the behavior */
  uint32_t resp_flag;
  /** numa socket id, set by lib */
  int socket_id;
};

/** The structure info for st22 encoder dev. */
struct st22_encoder_dev {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** device, cpu/gpu/fpga/others */
  enum st_plugin_device target_device;

  /** supported input format for encode, ST_FMT_CAP_* */
  uint64_t input_fmt_caps;
  /** supported output format for encode, ST_FMT_CAP_* */
  uint64_t output_fmt_caps;
  /** create session function */
  st22_encode_priv (*create_session)(void* priv, st22p_encode_session session_p,
                                     struct st22_encoder_create_req* req);
  /** Callback when frame available in the lib. */
  int (*notify_frame_available)(st22_encode_priv encode_priv);
  /** free session function */
  int (*free_session)(void* priv, st22_encode_priv encode_priv);
};

/** The structure info for st22 encode frame meta. */
struct st22_encode_frame_meta {
  /** Encode source frame */
  struct st_frame* src;
  /** Encode dst frame */
  struct st_frame* dst;
  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** The structure info for st plugin decode session create request. */
struct st22_decoder_create_req {
  /** Session resolution width, set by lib */
  uint32_t width;
  /** Session resolution height, set by lib */
  uint32_t height;
  /** Session resolution fps, set by lib */
  enum st_fps fps;
  /** Interlaced or not, set by lib */
  bool interlaced;
  /** Session input frame format, set by lib */
  enum st_frame_fmt input_fmt;
  /** Session output frame format, set by lib */
  enum st_frame_fmt output_fmt;
  /** frame buffer count, set by lib */
  uint16_t framebuff_cnt;
  /** thread count, set by lib */
  uint32_t codec_thread_cnt;
  /** the flag indicated by plugin to customize the behavior */
  uint32_t resp_flag;
  /** numa socket id, set by lib */
  int socket_id;
};

/** The structure info for st22 decoder dev. */
struct st22_decoder_dev {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** device, cpu/gpu/fpga/others */
  enum st_plugin_device target_device;

  /** supported input format for decode, ST_FMT_CAP_* */
  uint64_t input_fmt_caps;
  /** supported output format for decode, ST_FMT_CAP_* */
  uint64_t output_fmt_caps;
  /** create session function */
  st22_decode_priv (*create_session)(void* priv, st22p_decode_session session_p,
                                     struct st22_decoder_create_req* req);
  /** Callback when frame available in the lib. */
  int (*notify_frame_available)(st22_decode_priv decode_priv);
  /** free session function */
  int (*free_session)(void* priv, st22_decode_priv decode_priv);
};

/** The structure info for st22 decode frame meta. */
struct st22_decode_frame_meta {
  /** Decode source frame */
  struct st_frame* src;
  /** Decode dst frame */
  struct st_frame* dst;
  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** The structure info for st plugin convert session create request. */
struct st20_converter_create_req {
  /** Session resolution width, set by lib */
  uint32_t width;
  /** Session resolution height, set by lib */
  uint32_t height;
  /** Session resolution fps, set by lib */
  enum st_fps fps;
  /** interlace or not, false: non-interlaced: true: interlaced, set by lib */
  bool interlaced;
  /** Session input frame format, set by lib */
  enum st_frame_fmt input_fmt;
  /** Session output frame format, set by lib */
  enum st_frame_fmt output_fmt;
  /** frame buffer count, set by lib */
  uint16_t framebuff_cnt;
};

/** The structure info for st20 converter dev. */
struct st20_converter_dev {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** device, cpu/gpu/fpga/others */
  enum st_plugin_device target_device;

  /** supported input format for convert, ST_FMT_CAP_* */
  uint64_t input_fmt_caps;
  /** supported output format for convert, ST_FMT_CAP_* */
  uint64_t output_fmt_caps;
  /** create session function */
  st20_convert_priv (*create_session)(void* priv, st20p_convert_session session_p,
                                      struct st20_converter_create_req* req);
  /** Callback when frame available in the lib. */
  int (*notify_frame_available)(st20_convert_priv convert_priv);
  /** free session function */
  int (*free_session)(void* priv, st20_convert_priv convert_priv);
};

/** The structure info for st20 convert frame meta. */
struct st20_convert_frame_meta {
  /** Encode source frame */
  struct st_frame* src;
  /** Encode dst frame */
  struct st_frame* dst;
  /** priv pointer for lib, do not touch this */
  void* priv;
};

/** The structure info for st tx port, used in creating session. */
struct st_tx_port {
  /** Mandatory. Destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Mandatory. Pcie BDF path like 0000:af:00.0, should align to BDFs of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** Mandatory. 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Mandatory. UDP destination port number for this tx session */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
  /** Mandatory. 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** Optional. UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /** Optional. Synchronization source defined in RFC3550, if zero the session will assign
   * a random value */
  uint32_t ssrc;
};

/** The structure info for st rx port, used in creating session. */
struct st_rx_port {
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
  /** Mandatory. UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
  /** Mandatory. 7 bits payload type define in RFC3550. Zero means disable the
   * payload_type check on the RX pkt path */
  uint8_t payload_type;
  /** Optional. Synchronization source defined in RFC3550, RX session will check the
   * incoming RTP packets match the ssrc. Leave to zero to disable the ssrc check */
  uint32_t ssrc;
  /** Optional. source filter IP address of multicast */
  uint8_t mcast_sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
};

/** The structure describing how to create a tx st2110-20 pipeline session. */
struct st20p_tx_ops {
  /** Mandatory. tx port info */
  struct st_tx_port port;

  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session input frame format */
  enum st_frame_fmt input_fmt;
  /** Mandatory. Session transport pacing type */
  enum st21_pacing transport_pacing;
  /** Mandatory. Session transport packing type */
  enum st20_packing transport_packing;
  /** Mandatory. Session transport frame format */
  enum st20_fmt transport_fmt;
  /** Mandatory. Convert plugin device, auto or special */
  enum st_plugin_device device;
  /**
   * Mandatory. The frame buffer count requested for one st20 pipeline tx session,
   * should be in range [2, ST20_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;

  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST20P_TX_FLAG_* for possible value
   */
  uint32_t flags;
  /**
   * Optional.Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Optional. Callback when frame done in the lib. If TX_FLAG_DROP_WHEN_LATE is enabled
   * this will be called only when the notify_frame_late is not triggered.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, struct st_frame* frame);

  /**
   * Optional. Callback when frame timing issues occur.
   * If ST20P_TX_FLAG_DROP_WHEN_LATE is enabled: triggered when a frame is dropped
   * from the pipeline due to late transmission.
   * If ST20P_TX_FLAG_DROP_WHEN_LATE is disabled: triggered when the transport
   * layer reports late frame delivery.
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);

  /** Optional. Linesize for transport frame, only for non-convert mode */
  size_t transport_linesize;

  /** Optional for ST20P_TX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_tx_rtcp_ops rtcp;
  /**
   * Optional. tx destination mac address.
   * Valid if ST20P_TX_FLAG_USER_P(R)_MAC is enabled
   */
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
   * Optional. the time for lib to detect the hang on the tx queue and try to recovery
   * Leave to zero system will use the default value(1s).
   */
  uint32_t tx_hang_detect_ms;
  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /**  Use this socket if ST20P_TX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/** The structure describing how to create a rx st2110-20 pipeline session. */
struct st20p_rx_ops {
  /** Mandatory. rx port info */
  struct st_rx_port port;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session transport frame format */
  enum st20_fmt transport_fmt;
  /** Mandatory. Session output frame format */
  enum st_frame_fmt output_fmt;
  /** Mandatory. Convert plugin device, auto or special */
  enum st_plugin_device device;
  /**
   * Mandatory. The frame buffer count requested for one st20 pipeline rx session,
   * should be in range [2, ST20_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;

  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST20P_RX_FLAG_* for possible value
   */
  uint32_t flags;
  /* Optional, the size for each mt_rxq_burst, leave to zero to let system select a
   * default value */
  uint16_t rx_burst_size;
  /**
   * Optional. Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);

  /** Optional. Linesize for transport frame, only for non-convert mode */
  size_t transport_linesize;
  /** Optional. Array of external frames */
  struct st_ext_frame* ext_frames;
  /** Optional for ST20_RX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_rx_rtcp_ops rtcp;
  /**
   * Optional. Callback when the lib query next external frame's data address.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*query_ext_frame)(void* priv, struct st_ext_frame* ext_frame,
                         struct st20_rx_frame_meta* meta);
  /**
   * Optional. event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /**
   * Optional with ST20_RX_FLAG_AUTO_DETECT. The callback when lib detected video format.
   * And only non-block method can be used in this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_detected)(void* priv, const struct st20_detect_meta* meta,
                         struct st20_detect_reply* reply);
  /**  Use this socket if ST20P_RX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;

  /* use to store framebuffers on vram */
  void* gpu_context;
};

/** The structure describing how to create a tx st2110-22 pipeline session. */
struct st22p_tx_ops {
  /** Mandatory. tx port info */
  struct st_tx_port port;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session input frame format */
  enum st_frame_fmt input_fmt;
  /** Mandatory. packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** Mandatory. codec for this pipeline */
  enum st22_codec codec;
  /** Mandatory. encode plugin device, auto or special */
  enum st_plugin_device device;
  /** Mandatory. speed or quality mode */
  enum st22_quality_mode quality;
  /** Mandatory. codestream size, calculate as compress ratio. For interlaced, it's the
   * expected codestream size for each field */
  size_t codestream_size;
  /**
   *  Mandatory. the frame buffer count requested for one st22 pipeline tx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;

  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST22P_TX_FLAG_* for possible value
   */
  uint32_t flags;
  /** Optional. thread count for codec, leave to zero if not know */
  uint32_t codec_thread_cnt;
  /**
   * Optional. Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Optional. Callback when frame done in the lib. If TX_FLAG_DROP_WHEN_LATE is enabled
   * this will be called only when the notify_frame_late is not triggered.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, struct st_frame* frame);

  /**
   * Optional. Callback when frame timing issues occur.
   * If ST22P_TX_FLAG_DROP_WHEN_LATE is enabled: triggered when a frame is dropped
   * from the pipeline due to late transmission.
   * If ST22P_TX_FLAG_DROP_WHEN_LATE is disabled: triggered when the transport
   * layer reports late frame delivery.
   */
  int (*notify_frame_late)(void* priv, uint64_t epoch_skipped);

  /** Optional for ST22P_TX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_tx_rtcp_ops rtcp;
  /**
   * Optional. tx destination mac address.
   * Valid if ST22P_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];
  /**
   * Optional. event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /**  Use this socket if ST22P_TX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/** The structure describing how to create a rx st2110-22 pipeline session. */
struct st22p_rx_ops {
  /** Mandatory. tx port info */
  struct st_rx_port port;
  /** Mandatory. Session resolution width */
  uint32_t width;
  /** Mandatory. Session resolution height */
  uint32_t height;
  /** Mandatory. Session resolution fps */
  enum st_fps fps;
  /** Mandatory. interlaced or not */
  bool interlaced;
  /** Mandatory. Session output frame format */
  enum st_frame_fmt output_fmt;
  /** Mandatory. packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** Mandatory. codec for this pipeline */
  enum st22_codec codec;
  /** Mandatory. encode plugin device, auto or special */
  enum st_plugin_device device;
  /**
   * Mandatory. the frame buffer count requested for one st22 pipeline rx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;

  /** Optional. name */
  const char* name;
  /** Optional. private data to the callback function */
  void* priv;
  /** Optional. Flags to control session behaviors. See ST22P_RX_FLAG_* for possible value
   */
  uint32_t flags;
  /** Optional. thread count for codec, leave to zero if not know */
  uint32_t codec_thread_cnt;
  /** Optional. max codestream size, lib will use output frame size if not set. For
   * interlaced, it's the expected codestream size for each field */
  size_t max_codestream_size;
  /** Optional for ST22P_RX_FLAG_ENABLE_RTCP. RTCP info */
  struct st_rx_rtcp_ops rtcp;
  /**
   * Optional. Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Optional. event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
  /**
   * Mandatory for ST22P_RX_FLAG_EXT_FRAME. Callback when the lib query next external
   * frame's data address. And only non-block method can be used within this callback as
   * it run from lcore tasklet routine.
   */
  int (*query_ext_frame)(void* priv, struct st_ext_frame* ext_frame,
                         struct st22_rx_frame_meta* meta);
  /**  Use this socket if ST22P_RX_FLAG_FORCE_NUMA is on, default use the NIC numa */
  int socket_id;
};

/**
 * Register one st22 encoder.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param dev
 *   The pointer to the structure describing a st plugin encode.
 * @return
 *   - NULL: fail.
 *   - Others: the handle to the encode dev context.
 */
st22_encoder_dev_handle st22_encoder_register(mtl_handle mt,
                                              struct st22_encoder_dev* dev);

/**
 * Unregister one st22 encoder.
 *
 * @param handle
 *   The handle to the the encode dev context.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st22_encoder_unregister(st22_encoder_dev_handle handle);

/**
 * Get one encode frame from the tx st2110-22 pipeline session.
 * Call st22_encoder_put_frame to return the frame to session.
 *
 * @param session
 *   The handle to the st2110-22 encoder session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st22_encode_frame_meta* st22_encoder_get_frame(st22p_encode_session session);

/**
 * Wake up the block wait on st22_encoder_get_frame if ST22_ENCODER_RESP_FLAG_BLOCK_GET is
 * enabled.
 *
 * @param session
 *   The handle to the st2110-22 encoder session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22_encoder_wake_block(st22p_encode_session session);

/**
 * Set the block timeout time on st22_decoder_get_frame if
 * ST22_ENCODER_RESP_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the st2110-22 encoder session.
 * @param timedwait_ns
 *   The timeout time in ns.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22_encoder_set_block_timeout(st22p_encode_session session, uint64_t timedwait_ns);

/**
 * Put back the frame which get by st22_encoder_get_frame to the tx
 * st2110-22 pipeline session.
 *
 * @param session
 *   The handle to the st2110-22 encoder session.
 * @param frame
 *   the frame pointer by st22_encoder_get_frame.
 * @param result
 *   the encode result for current frame, < 0 means fail.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22_encoder_put_frame(st22p_encode_session session,
                           struct st22_encode_frame_meta* frame, int result);

/**
 * Register one st22 decoder.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param dev
 *   The pointer to the structure describing a st plugin encode.
 * @return
 *   - NULL: fail.
 *   - Others: the handle to the encode dev
 */
st22_decoder_dev_handle st22_decoder_register(mtl_handle mt,
                                              struct st22_decoder_dev* dev);

/**
 * Unregister one st22 decoder.
 *
 * @param handle
 *   The handle to the the decode dev context.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st22_decoder_unregister(st22_decoder_dev_handle handle);

/**
 * Get one decode frame from the rx st2110-22 pipeline session.
 * Call st22_decoder_put_frame to return the frame to session.
 *
 * @param session
 *   The handle to the st2110-22 decode session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st22_decode_frame_meta* st22_decoder_get_frame(st22p_decode_session session);

/**
 * Wake up the block wait on st22_decoder_get_frame if ST22_DECODER_RESP_FLAG_BLOCK_GET is
 * enabled.
 *
 * @param session
 *   The handle to the st2110-22 decode session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22_decoder_wake_block(st22p_decode_session session);

/**
 * Set the block timeout time on st22_decoder_get_frame if
 * ST22_DECODER_RESP_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the st2110-22 decode session.
 * @param timedwait_ns
 *   The timeout time in ns.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22_decoder_set_block_timeout(st22p_decode_session session, uint64_t timedwait_ns);

/**
 * Put back the frame which get by st22_decoder_get_frame to the rx
 * st2110-22 pipeline session.
 *
 * @param session
 *   The handle to the st2110-22 decode session.
 * @param frame
 *   the frame pointer by st22_encoder_get_frame.
 * @param result
 *   the decode result for current frame, < 0 means fail.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22_decoder_put_frame(st22p_decode_session session,
                           struct st22_decode_frame_meta* frame, int result);

/**
 * Register one st20 converter.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param dev
 *   The pointer to the structure describing a st20 plugin convert.
 * @return
 *   - NULL: fail.
 *   - Others: the handle to the convert dev
 */
st20_converter_dev_handle st20_converter_register(mtl_handle mt,
                                                  struct st20_converter_dev* dev);

/**
 * Unregister one st20 converter.
 *
 * @param handle
 *   The handle to the the convert dev context.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st20_converter_unregister(st20_converter_dev_handle handle);

/**
 * Get one convert frame from the rx st2110-20  pipeline session.
 * Call st20_converter_put_frame to return the frame to session.
 *
 * @param session
 *   The handle to the rx st2110-20 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st20_convert_frame_meta* st20_converter_get_frame(st20p_convert_session session);

/**
 * Put back the frame which get by st20_converter_get_frame to the rx
 * st2110-20 pipeline session.
 *
 * @param session
 *   The handle to the rx st2110-20 pipeline session.
 * @param frame
 *   the frame pointer by st20_converter_get_frame.
 * @param result
 *   the convert result for current frame, < 0 means fail.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st20_converter_put_frame(st20p_convert_session session,
                             struct st20_convert_frame_meta* frame, int result);

/**
 * Register one st plugin so.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param path
 *   The path to the plugin so.
 *   Ex: /usr/local/lib/x86_64-linux-gnu/libst_plugin_sample.so
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st_plugin_register(mtl_handle mt, const char* path);

/**
 * Unregister one st plugin so.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param path
 *   The path to the plugin so.
 *   Ex: /usr/local/lib/x86_64-linux-gnu/libst_plugin_sample.so
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st_plugin_unregister(mtl_handle mt, const char* path);

/**
 * Get the number of registered plugins lib.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @return
 *   - number.
 */
int st_get_plugins_nb(mtl_handle mt);

/**
 * Create one tx st2110-22 pipeline session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 * st2110-22 pipeline session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-22 pipeline session.
 */
st22p_tx_handle st22p_tx_create(mtl_handle mt, struct st22p_tx_ops* ops);

/**
 * Free the tx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - 0: Success, tx st2110-22 pipeline session freed.
 *   - <0: Error code of the tx st2110-22 pipeline session free.
 */
int st22p_tx_free(st22p_tx_handle handle);

/**
 * Get one tx frame from the tx st2110-22 pipeline session.
 * Call st22p_tx_put_frame or st22p_tx_put_ext_frame(if ST22P_TX_FLAG_EXT_FRAME) to return
 * the frame to session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame meta pointer.
 */
struct st_frame* st22p_tx_get_frame(st22p_tx_handle handle);

/**
 * Put back the frame which get by st22p_tx_get_frame to the tx
 * st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @param frame
 *   the frame pointer by st22p_tx_get_frame.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22p_tx_put_frame(st22p_tx_handle handle, struct st_frame* frame);

/**
 * Put back the frame which get by st22p_tx_get_frame to the tx
 * st2110-22 pipeline session with external framebuffer.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @param frame
 *   The frame pointer by st22p_tx_get_frame.
 * @param ext_frame
 *   The pointer to the structure describing external framebuffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22p_tx_put_ext_frame(st22p_tx_handle handle, struct st_frame* frame,
                           struct st_ext_frame* ext_frame);

/**
 * Get the framebuffer pointer from the tx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st22p_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st22p_tx_get_fb_addr(st22p_tx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size from the tx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - size
 */
size_t st22p_tx_frame_size(st22p_tx_handle handle);

/**
 * Online update the destination info for the tx st2110-22(pipeline) session.
 *
 * @param handle
 *   The handle to the tx st2110-22(pipeline) session.
 * @param dst
 *   The pointer to the tx st2110-22(pipeline) destination info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_tx_update_destination(st22p_tx_handle handle, struct st_tx_dest_info* dst);

/**
 * Wake up the block wait on st22p_tx_get_frame if ST22P_TX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the tx st2110-22(pipeline) session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_tx_wake_block(st22p_tx_handle handle);

/**
 * Set the block timeout time on st22p_tx_get_frame if ST22P_TX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the tx st2110-22(pipeline) session.
 * @param timedwait_ns
 *   The timeout time in ns.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_tx_set_block_timeout(st22p_tx_handle handle, uint64_t timedwait_ns);

/**
 * Create one rx st2110-22 pipeline session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-22 pipeline session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-22 pipeline session.
 */
st22p_rx_handle st22p_rx_create(mtl_handle mt, struct st22p_rx_ops* ops);

/**
 * Free the rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - 0: Success, rx st2110-22 pipeline session freed.
 *   - <0: Error code of the rx st2110-22 pipeline session free.
 */
int st22p_rx_free(st22p_rx_handle handle);

/**
 * Get one rx frame from the rx st2110-22 pipeline session.
 * Call st22p_rx_put_frame to return the frame to session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st_frame* st22p_rx_get_frame(st22p_rx_handle handle);

/**
 * Put back the frame which get by st22p_rx_get_frame to the rx
 * st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param frame
 *   the frame pointer by st22p_rx_get_frame.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st22p_rx_put_frame(st22p_rx_handle handle, struct st_frame* frame);

/**
 * Get the framebuffer pointer from the rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st22p_rx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st22p_rx_get_fb_addr(st22p_rx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size from the rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - size
 */
size_t st22p_rx_frame_size(st22p_rx_handle handle);

/**
 * Dump st2110-22 pipeline packets to pcapng file.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param max_dump_packets
 *   The max number of packets to be dumped.
 * @param sync
 *   synchronous or asynchronous, true means this func will return after dump
 * progress is finished.
 * @param meta
 *   The meta data returned, only for synchronous, leave to NULL if not need the meta.
 * @return
 *   - 0: Success, rx st2110-22 pipeline session pcapng dump succ.
 *   - <0: Error code of the rx st2110-22 pipeline session pcapng dump.
 */
int st22p_rx_pcapng_dump(st22p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta);

/**
 * Get the queue meta attached to rx st2110-22 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-22 pipeline session.
 * @param meta
 *   The rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_rx_get_queue_meta(st22p_rx_handle handle, struct st_queue_meta* meta);

/**
 * Online update the source info for the rx st2110-22(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-22(pipeline) session.
 * @param src
 *   The pointer to the rx st2110-22(pipeline) source info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_rx_update_source(st22p_rx_handle handle, struct st_rx_source_info* src);

/**
 * Wake up the block wait on st22p_rx_get_frame if ST22P_RX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the rx st2110-22(pipeline) session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_rx_wake_block(st22p_rx_handle handle);

/**
 * Set the block timeout time on st22p_rx_get_frame if ST22P_RX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the rx st2110-22(pipeline) session.
 * @param timedwait_ns
 *   The timeout time in ns.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st22p_rx_set_block_timeout(st22p_rx_handle handle, uint64_t timedwait_ns);

/**
 * Create one tx st2110-20 pipeline session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 * st2110-20 pipeline session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-20 pipeline session.
 */
st20p_tx_handle st20p_tx_create(mtl_handle mt, struct st20p_tx_ops* ops);

/**
 * Free the tx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-20 pipeline session.
 * @return
 *   - 0: Success, tx st2110-20 pipeline session freed.
 *   - <0: Error code of the tx st2110-20 pipeline session free.
 */
int st20p_tx_free(st20p_tx_handle handle);

/**
 * Get one tx frame from the tx st2110-20 pipeline session.
 * Call st20p_tx_put_frame/st20p_tx_put_ext_frame(if ST20P_TX_FLAG_EXT_FRAME) to return
 * the frame to session.
 *
 * @param handle
 *   The handle to the tx st2110-20 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame meta pointer.
 */
struct st_frame* st20p_tx_get_frame(st20p_tx_handle handle);

/**
 * Put back the frame which get by st20p_tx_get_frame to the tx
 * st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-20 pipeline session.
 * @param frame
 *   The frame pointer by st20p_tx_get_frame.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st20p_tx_put_frame(st20p_tx_handle handle, struct st_frame* frame);

/**
 * Put back the frame which get by st20p_tx_get_frame to the tx
 * st2110-20 pipeline session with external framebuffer.
 *
 * @param handle
 *   The handle to the tx st2110-20 pipeline session.
 * @param frame
 *   The frame pointer by st20p_tx_get_frame.
 * @param ext_frame
 *   The pointer to the structure describing external framebuffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st20p_tx_put_ext_frame(st20p_tx_handle handle, struct st_frame* frame,
                           struct st_ext_frame* ext_frame);

/**
 * Get the framebuffer pointer from the tx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-20 pipeline session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st20p_tx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st20p_tx_get_fb_addr(st20p_tx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size from the tx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the tx st2110-20 pipeline session.
 * @return
 *   - size
 */
size_t st20p_tx_frame_size(st20p_tx_handle handle);

/**
 * Get the scheduler index for the tx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @return
 *   - >=0 the scheduler index.
 *   - <0: Error code.
 */
int st20p_tx_get_sch_idx(st20p_tx_handle handle);

/**
 * Retrieve pacing parameters for a tx st2110-20(pipeline) session.
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
int st20p_tx_get_pacing_params(st20p_tx_handle handle, double* tr_offset_ns,
                               double* trs_ns, uint32_t* vrx_pkts);
/**
 * Retrieve the general statistics(I/O) for one tx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20p_tx_get_session_stats(st20p_tx_handle handle, struct st20_tx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one tx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20p_tx_reset_session_stats(st20p_tx_handle handle);

/**
 * Online update the destination info for the tx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @param dst
 *   The pointer to the tx st2110-20(pipeline) destination info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_tx_update_destination(st20p_tx_handle handle, struct st_tx_dest_info* dst);

/**
 * Wake up the block wait on st20p_tx_get_frame if ST20P_TX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_tx_wake_block(st20p_tx_handle handle);

/**
 * Set the block timeout time on st20p_tx_get_frame if ST20P_TX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @param timedwait_ns
 *   The timeout time in ns.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_tx_set_block_timeout(st20p_tx_handle handle, uint64_t timedwait_ns);

/**
 * Create one rx st2110-20 pipeline session.
 *
 * @param mt
 *   The handle to the media transport device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-20 pipeline session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-20 pipeline session.
 */
st20p_rx_handle st20p_rx_create(mtl_handle mt, struct st20p_rx_ops* ops);

/**
 * Free the rx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @return
 *   - 0: Success, rx st2110-20 pipeline session freed.
 *   - <0: Error code of the rx st2110-20 pipeline session free.
 */
int st20p_rx_free(st20p_rx_handle handle);

/**
 * Get one rx frame from the rx st2110-20 pipeline session.
 * Call st20p_rx_put_frame to return the frame to session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st_frame* st20p_rx_get_frame(st20p_rx_handle handle);

/**
 * Put back the frame which get by st20p_rx_get_frame to the rx
 * st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @param frame
 *   the frame pointer by st20p_rx_get_frame.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st20p_rx_put_frame(st20p_rx_handle handle, struct st_frame* frame);

/**
 * Get the framebuffer pointer from the rx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @param idx
 *   The framebuffer index, should be in range [0, framebuff_cnt of st20p_rx_ops].
 * @return
 *   - NULL on error.
 *   - Otherwise, the framebuffer pointer.
 */
void* st20p_rx_get_fb_addr(st20p_rx_handle handle, uint16_t idx);

/**
 * Get the framebuffer size from the rx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @return
 *   - size
 */
size_t st20p_rx_frame_size(st20p_rx_handle handle);

/**
 * Dump st2110-20 pipeline packets to pcapng file.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @param max_dump_packets
 *   The max number of packets to be dumped.
 * @param sync
 *   synchronous or asynchronous, true means this func will return after dump
 * progress is finished.
 * @param meta
 *   The meta data returned, only for synchronous, leave to NULL if not need the meta.
 * @return
 *   - 0: Success, rx st2110-20 pipeline session pcapng dump succ.
 *   - <0: Error code of the rx st2110-20 pipeline session pcapng dump.
 */
int st20p_rx_pcapng_dump(st20p_rx_handle handle, uint32_t max_dump_packets, bool sync,
                         struct st_pcap_dump_meta* meta);

/**
 * Get the queue meta attached to rx st2110-20 pipeline session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @param meta
 *   the rx queue meta info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_rx_get_queue_meta(st20p_rx_handle handle, struct st_queue_meta* meta);

/**
 * Get the scheduler index for the rx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @return
 *   - >=0 the scheduler index.
 *   - <0: Error code.
 */
int st20p_rx_get_sch_idx(st20p_rx_handle handle);

/**
 * Retrieve the general statistics(I/O) for one rx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @param port
 *   The port index.
 * @param stats
 *   A pointer to stats structure.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20p_rx_get_session_stats(st20p_rx_handle handle, struct st20_rx_user_stats* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20p_rx_reset_session_stats(st20p_rx_handle handle);

/**
 * Online update the source info for the rx st2110-20(pipeline) session.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @param src
 *   The pointer to the rx st2110-20(pipeline) source info.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_rx_update_source(st20p_rx_handle handle, struct st_rx_source_info* src);

/**
 * Get the timing parser pass critical to rx st2110-20(pipeline) session.
 * Only available if ST20P_RX_FLAG_TIMING_PARSER_META is enabled.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @param pass
 *   the pointer to save the timing parser pass critical.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_rx_timing_parser_critical(st20p_rx_handle handle, struct st20_rx_tp_pass* pass);

/**
 * Wake up the block wait on st20p_rx_get_frame if ST20P_RX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_rx_wake_block(st20p_rx_handle handle);

/**
 * Set the block timeout time on st20p_rx_get_frame if ST20P_RX_FLAG_BLOCK_GET is enabled.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @param timedwait_ns
 *   The timeout time in ns.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st20p_rx_set_block_timeout(st20p_rx_handle handle, uint64_t timedwait_ns);

/**
 * Convert color format from source frame to destination frame.
 *
 * @param src
 *   The source frame.
 * @param dst
 *   The destination frame.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st_frame_convert(struct st_frame* src, struct st_frame* dst);

/**
 * Downsample frame size to destination frame.
 *
 * @param src
 *   The source frame.
 * @param dst
 *   The destination frame.
 * @param idx
 *   The index of the sample box.
 * @return
 *   - 0: Success.
 *   - <0: Error code.
 */
int st_frame_downsample(struct st_frame* src, struct st_frame* dst, int idx);

/**
 * Calculate the least linesize per the format, w, plane
 *
 * @param fmt
 *   format.
 * @param width
 *   width.
 * @param plane
 *   plane index.
 * @return
 *   > 0 if successful.
 *   0: Fail.
 */
size_t st_frame_least_linesize(enum st_frame_fmt fmt, uint32_t width, uint8_t plane);

/**
 * Calculate the frame size per the format, w and h
 *
 * @param fmt
 *   format.
 * @param width
 *   width.
 * @param height
 *   height.
 * @param interlaced
 *   interlace or not, false: non-interlaced: true: interlaced.
 * @return
 *   > 0 if successful.
 *   0: Fail.
 */
size_t st_frame_size(enum st_frame_fmt fmt, uint32_t width, uint32_t height,
                     bool interlaced);

/**
 * St_frame sanity check
 *
 * @param frame
 *   The st_frame pointer.
 * @return
 *   0 if successful.
 *   < 0: Fail.
 */
int st_frame_sanity_check(struct st_frame* frame);

/**
 * Get the name of st_frame_fmt
 *
 * @param fmt
 *   format.
 * @return
 *   The pointer to name.
 *   NULL: Fail.
 */
const char* st_frame_fmt_name(enum st_frame_fmt fmt);

/**
 * Get st_frame_fmt from name
 *
 * @param name
 *   name.
 * @return
 *   The frame fmt.
 *   ST_FRAME_FMT_MAX: Fail.
 */
enum st_frame_fmt st_frame_name_to_fmt(const char* name);

/**
 * Get number of planes of st_frame_fmt
 *
 * @param fmt
 *   format.
 * @return
 *   The planes number.
 *   0: Not match any fmt.
 */
uint8_t st_frame_fmt_planes(enum st_frame_fmt fmt);

/** helper to know if it's a codestream fmt */
static inline bool st_frame_fmt_is_codestream(enum st_frame_fmt fmt) {
  if (fmt >= ST_FRAME_FMT_CODESTREAM_START && fmt <= ST_FRAME_FMT_CODESTREAM_END)
    return true;
  else
    return false;
}

/**
 * Get st20 transport format from st_frame_fmt
 *
 * @param fmt
 *   st_frame_fmt format.
 * @return
 *   The compatible st20 transport fmt.
 *   ST20_FMT_MAX: Fail.
 */
enum st20_fmt st_frame_fmt_to_transport(enum st_frame_fmt fmt);

/**
 * Get st_frame_fmt from st20 transport format
 *
 * @param tfmt
 *   transport format.
 * @return
 *   The compatible st_frame_fmt.
 *   ST_FRAME_FMT_MAX: Fail.
 */
enum st_frame_fmt st_frame_fmt_from_transport(enum st20_fmt tfmt);

/**
 * Check if it's the same layout between st_frame_fmt and st20_fmt
 *
 * @param fmt
 *   st_frame_fmt format.
 * @param tfmt
 *   st20_fmt format.
 * @return
 *   - true if the same.
 *   - false if not the same.
 */
bool st_frame_fmt_equal_transport(enum st_frame_fmt fmt, enum st20_fmt tfmt);

/**
 * Draw a logo on the frame, only ST_FRAME_FMT_YUV422RFC4175PG2BE10 now
 *
 * @param frame
 *   the frame meta pointer of the source.
 * @param logo
 *   the frame meta pointer of the logo.
 * @param x
 *   the logo position x.
 * @param y
 *   the logo position y.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int st_draw_logo(struct st_frame* frame, struct st_frame* logo, uint32_t x, uint32_t y);

/**
 * Helper to get st frame plane size
 *
 * @param frame
 *   The st_frame pointer.
 * @param plane
 *   The plane index.
 * @return
 *   size
 */
static inline size_t st_frame_plane_size(struct st_frame* frame, uint8_t plane) {
  /* no line size for codestream */
  if (st_frame_fmt_is_codestream(frame->fmt)) return frame->data_size;

  size_t sz = frame->linesize[plane] * frame->height;
  if (frame->interlaced) sz /= 2;
  return sz;
}

/**
 * This helper function retrieves the actual data height in one st frame. Please note that
 * for an interlaced frame, it will return half the height.
 *
 * @param frame
 *   The st_frame pointer.
 * @return
 *   height
 */
static inline uint32_t st_frame_data_height(struct st_frame* frame) {
  uint32_t h = frame->height;
  if (frame->interlaced) h /= 2;
  return h;
}

/** Helper to set the port for struct st_rx_port */
int st_rxp_para_port_set(struct st_rx_port* p, enum mtl_session_port port, char* name);
/** Helper to set the ip for struct st_rx_port */
int st_rxp_para_ip_set(struct st_rx_port* p, enum mtl_port port, char* ip);
/** Helper to set the udp port number for struct st_rx_port */
static inline void st_rxp_para_udp_port_set(struct st_rx_port* p, enum mtl_port port,
                                            uint16_t udp_port) {
  p->udp_port[port] = udp_port;
}

/** Helper to set the port for struct st_tx_port */
int st_txp_para_port_set(struct st_tx_port* p, enum mtl_session_port port, char* name);
/** Helper to set the dip for struct st_tx_port */
int st_txp_para_dip_set(struct st_tx_port* p, enum mtl_port port, char* ip);
/** Helper to set the udp port number for struct st_tx_port */
static inline void st_txp_para_udp_port_set(struct st_tx_port* p, enum mtl_port port,
                                            uint16_t udp_port) {
  p->udp_port[port] = udp_port;
}

/** Helper to get the frame addr from struct st_frame */
static inline void* st_frame_addr(struct st_frame* frame, uint8_t plane) {
  return frame->addr[plane];
}
/** Helper to get the frame addr(mtl_cpuva_t) from struct st_frame */
static inline mtl_cpuva_t st_frame_addr_cpuva(struct st_frame* frame, uint8_t plane) {
  return (mtl_cpuva_t)frame->addr[plane];
}
/** Helper to get the frame iova from struct st_frame */
static inline mtl_iova_t st_frame_iova(struct st_frame* frame, uint8_t plane) {
  return frame->iova[plane];
}
/** Helper to get the frame tp meta from struct st_frame */
static inline struct st20_rx_tp_meta* st_frame_tp_meta(struct st_frame* frame,
                                                       enum mtl_session_port port) {
  return frame->tp[port];
}

/** request to create a plain memory by rte malloc to hold the frame buffer */
struct st_frame* st_frame_create(mtl_handle mt, enum st_frame_fmt fmt, uint32_t w,
                                 uint32_t h, bool interlaced);
/** free the frame created by st_frame_create */
int st_frame_free(struct st_frame* frame);
/** request to create a plain memory by libc malloc */
struct st_frame* st_frame_create_by_malloc(enum st_frame_fmt fmt, uint32_t w, uint32_t h,
                                           bool interlaced);

/** merge two fields to one full frame */
int st_field_merge(const struct st_frame* first, const struct st_frame* second,
                   struct st_frame* frame);
/** split one full frame to two fields */
int st_field_split(const struct st_frame* frame, struct st_frame* first,
                   struct st_frame* second);

/** helper for name to codec */
enum st22_codec st_name_to_codec(const char* name);

#if defined(__cplusplus)
}
#endif

#endif
