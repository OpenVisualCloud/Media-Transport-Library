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
/** ST format cap of ST_FRAME_FMT_UYVY */
#define ST_FMT_CAP_UYVY (MTL_BIT64(ST_FRAME_FMT_UYVY))
/** ST format cap of ST_FRAME_FMT_YUV422RFC4175PG2BE10 */
#define ST_FMT_CAP_YUV422RFC4175PG2BE10 (MTL_BIT64(ST_FRAME_FMT_YUV422RFC4175PG2BE10))

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

/**
 * Flag bit in flags of struct st_frame.
 * Frame has external buffer attached.
 */
#define ST_FRAME_FLAG_EXT_BUF (MTL_BIT32(0))

/** Max planes number for one frame */
#define ST_MAX_PLANES (4)

/** The structure info for external frame */
struct st_ext_frame {
  /** Each plane's virtual address of external frame */
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

  /** priv pointer for lib, do not touch this */
  void* priv;
  /** priv data for user */
  void* opaque;
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

/**
 * Flag bit in flags of struct st22p_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST22P_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st22p_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST22P_TX_FLAG_USER_R_MAC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st22p_tx_ops.
 * Disable ST22 boxes
 */
#define ST22P_TX_FLAG_DISABLE_BOXES (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st22p_tx_ops.
 * User control the frame pacing by pass a timestamp in st_frame,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST22P_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st22p_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST22P_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st22p_tx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST22P_TX_FLAG_ENABLE_VSYNC (MTL_BIT32(5))
/**
 * Flag bit in flags of struct st22p_tx_ops.
 * If enable the rtcp.
 */
#define ST22P_TX_FLAG_ENABLE_RTCP (MTL_BIT32(6))

/**
 * Flag bit in flags of struct st20p_tx_ops.
 * P TX destination mac assigned by user
 */
#define ST20P_TX_FLAG_USER_P_MAC (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * R TX destination mac assigned by user
 */
#define ST20P_TX_FLAG_USER_R_MAC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * Lib uses user allocated memory for frames.
 * The external frames are provided by calling
 * st20_tx_put_ext_frame.
 */
#define ST20P_TX_FLAG_EXT_FRAME (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * User control the frame pacing by pass a timestamp in st_frame,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST20P_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST20P_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST20P_TX_FLAG_ENABLE_VSYNC (MTL_BIT32(5))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * If disable the static RL pad interval profiling.
 */
#define ST20P_TX_FLAG_DISABLE_STATIC_PAD_P (MTL_BIT32(6))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * If enable the rtcp.
 */
#define ST20P_TX_FLAG_ENABLE_RTCP (MTL_BIT32(7))
/**
 * Flag bit in flags of struct st20p_tx_ops.
 * Set this flag to set rtp timestamp at the time of the first packet egresses from the
 * sender.
 */
#define ST20P_TX_FLAG_RTP_TIMESTAMP_FIRST_PKT (MTL_BIT32(8))

/**
 * Flag bit in flags of struct st22p_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st22p_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST22P_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st22p_rx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST22P_RX_FLAG_ENABLE_VSYNC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st22p_rx_ops.
 * If enable the rtcp.
 */
#define ST22P_RX_FLAG_ENABLE_RTCP (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st22p_rx_ops.
 * If set, lib will pass the incomplete frame to app also.
 * User can check st_frame_status data for the frame integrity
 */
#define ST22P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME (MTL_BIT32(16))

/**
 * Flag bit in flags of struct st20p_rx_ops, for non MTL_PMD_DPDK_USER.
 * If set, it's application duty to set the rx flow(queue) and multicast join/drop.
 * Use st20p_rx_get_queue_meta to get the queue meta(queue number etc) info.
 */
#define ST20P_RX_FLAG_DATA_PATH_ONLY (MTL_BIT32(0))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST20P_RX_FLAG_ENABLE_VSYNC (MTL_BIT32(1))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * Only used for internal convert mode.
 * The external frames are provided by calling
 * st20p_rx_get_ext_frame.
 */
#define ST20P_RX_FLAG_EXT_FRAME (MTL_BIT32(2))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * Only used for internal convert mode and limited formats:
 * ST_FRAME_FMT_YUV422PLANAR10LE, ST_FRAME_FMT_Y210, ST_FRAME_FMT_UYVY
 * Perform the color format conversion on each packet.
 */
#define ST20P_RX_FLAG_PKT_CONVERT (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * If enable the rtcp.
 */
#define ST20P_RX_FLAG_ENABLE_RTCP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * If set, lib will pass the incomplete frame to app also.
 * User can check st_frame_status data for the frame integrity
 */
#define ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME (MTL_BIT32(16))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * If set, lib will try to allocate DMA memory copy offload from
 * dma_dev_port(mtl_init_params) list.
 * Pls note it could fallback to CPU if no DMA device is available.
 */
#define ST20P_RX_FLAG_DMA_OFFLOAD (MTL_BIT32(17))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * Only ST20_PACKING_BPM stream can enable this offload as software limit
 * Try to enable header split offload feature.
 */
#define ST20P_RX_FLAG_HDR_SPLIT (MTL_BIT32(19))
/**
 * Flag bit in flags of struct st20p_rx_ops.
 * Only for MTL_FLAG_RX_VIDEO_MIGRATE is enabled.
 * Always disable MIGRATE for this session.
 */
#define ST20P_RX_FLAG_DISABLE_MIGRATE (MTL_BIT32(20))

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
  /** Session input frame format, set by lib */
  enum st_frame_fmt input_fmt;
  /** Session output frame format, set by lib */
  enum st_frame_fmt output_fmt;
  /** frame buffer count, set by lib */
  uint16_t framebuff_cnt;
  /** thread count, set by lib */
  uint32_t codec_thread_cnt;
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
  /** Encode source frame */
  struct st_frame* src;
  /** Encode dst frame */
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
  /** destination IP address */
  uint8_t dip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /** UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
};

/** The structure info for st rx port, used in creating session. */
struct st_rx_port {
  /** source IP address of sender */
  uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
};

/** The structure describing how to create a tx st2110-20 pipeline session. */
struct st20p_tx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** tx port info */
  struct st_tx_port port;
  /** flags, value in ST20P_TX_FLAG_* */
  uint32_t flags;
  /**
   * tx destination mac address.
   * Valid if ST20P_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /**
   * The start vrx buffer.
   * Leave to zero if not know detail, lib will assign a start value of vrx(narrow) based
   * on resolution and timing. Refer to st21 spec for the possible vrx value and also fine
   * tune is required since network setup difference and RL burst.
   */
  uint16_t start_vrx;
  /**
   * Manually assigned padding pkt interval(pkts level) for RL pacing.
   * Leave to zero if not know detail, lib will train the interval in the initial routine.
   */
  uint16_t pad_interval;
  /**
   * The rtp timestamp delta(us) to the start time of frame.
   * Zero means the rtp timestamp at the start of the frame.
   */
  int32_t rtp_timestamp_delta_us;

  /**
   * the time for lib to detect the hang on the tx queue and try to recovery
   * Leave to zero system will use the default value(1s).
   */
  uint32_t tx_hang_detect_ms;

  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session input frame format */
  enum st_frame_fmt input_fmt;
  /** Session transport frame format */
  enum st20_fmt transport_fmt;
  /** interlace or not, false: non-interlaced: true: interlaced */
  bool interlaced;
  /** Linesize for transport frame, only for non-convert mode */
  size_t transport_linesize;
  /** Convert plugin device, auto or special */
  enum st_plugin_device device;
  /** Array of external frames */
  struct st_ext_frame* ext_frames;
  /**
   * The frame buffer count requested for one st20 pipeline tx session,
   * should be in range [2, ST20_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Callback when frame done in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, struct st_frame* frame);
  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
};

/** The structure describing how to create a rx st2110-20 pipeline session. */
struct st20p_rx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** rx port info */
  struct st_rx_port port;
  /** flags, value in ST20P_RX_FLAG_* */
  uint32_t flags;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session transport frame format */
  enum st20_fmt transport_fmt;
  /** Linesize for transport frame, only for non-convert mode */
  size_t transport_linesize;
  /** Session output frame format */
  enum st_frame_fmt output_fmt;
  /** interlace or not, false: non-interlaced: true: interlaced */
  bool interlaced;
  /** Convert plugin device, auto or special */
  enum st_plugin_device device;
  /** Array of external frames */
  struct st_ext_frame* ext_frames;
  /**
   * The frame buffer count requested for one st20 pipeline rx session,
   * should be in range [2, ST20_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);

  /**
   * Callback when the lib query next external frame's data address.
   * Only for non-convert mode with ST20P_RX_FLAG_RECEIVE_INCOMPLETE_FRAME.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*query_ext_frame)(void* priv, struct st20_ext_frame* ext_frame,
                         struct st20_rx_frame_meta* meta);
  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
};

/** The structure describing how to create a tx st2110-22 pipeline session. */
struct st22p_tx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** tx port info */
  struct st_tx_port port;
  /** flags, value in ST22P_TX_FLAG_* */
  uint32_t flags;
  /**
   * tx destination mac address.
   * Valid if ST22P_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session input frame format */
  enum st_frame_fmt input_fmt;
  /** packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** codec for this pipeline */
  enum st22_codec codec;
  /** encode plugin device, auto or special */
  enum st_plugin_device device;
  /** speed or quality mode */
  enum st22_quality_mode quality;
  /** thread count for codec, leave to zero if not know */
  uint32_t codec_thread_cnt;
  /** codestream size, calculate as compress ratio */
  size_t codestream_size;
  /**
   * the frame buffer count requested for one st22 pipeline tx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * Callback when frame done in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, struct st_frame* frame);
  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
};

/** The structure describing how to create a rx st2110-22 pipeline session. */
struct st22p_rx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** tx port info */
  struct st_rx_port port;
  /** flags, value in ST22P_RX_FLAG_* */
  uint32_t flags;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session output frame format */
  enum st_frame_fmt output_fmt;
  /** packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** codec for this pipeline */
  enum st22_codec codec;
  /** encode plugin device, auto or special */
  enum st_plugin_device device;
  /** thread count for codec, leave to zero if not know */
  uint32_t codec_thread_cnt;
  /** max codestream size, lib will use output frame size if not set */
  size_t max_codestream_size;
  /**
   * the frame buffer count requested for one st22 pipeline rx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   */
  uint16_t framebuff_cnt;
  /**
   * Callback when frame available in the lib.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_available)(void* priv);
  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
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
 *   The handle to the tx st2110-22 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st22_encode_frame_meta* st22_encoder_get_frame(st22p_encode_session session);

/**
 * Put back the frame which get by st22_encoder_get_frame to the tx
 * st2110-22 pipeline session.
 *
 * @param session
 *   The handle to the tx st2110-22 pipeline session.
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
 *   The handle to the rx st2110-22 pipeline session.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st22_decode_frame_meta* st22_decoder_get_frame(st22p_decode_session session);

/**
 * Put back the frame which get by st22_decoder_get_frame to the rx
 * st2110-22 pipeline session.
 *
 * @param session
 *   The handle to the rx st2110-22 pipeline session.
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
 * Call st22p_tx_put_frame to return the frame to session.
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
 * Call st20p_tx_put_frame to return the frame to session.
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
 * Retrieve the general statistics(I/O) for one tx st2110-20(pipeline) session port.
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
int st20p_tx_get_port_stats(st20p_tx_handle handle, enum mtl_session_port port,
                            struct st20_tx_port_status* stats);

/**
 * Reset the general statistics(I/O) for one tx st2110-20(pipeline) session port.
 *
 * @param handle
 *   The handle to the tx st2110-20(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20p_tx_reset_port_stats(st20p_tx_handle handle, enum mtl_session_port port);

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
 * Get one rx frame from the rx st2110-20 pipeline session with external framebuffer.
 * This is only used for internal convert mode, the convert is done in this call.
 * Call st20p_rx_put_frame to return the frame to session.
 *
 * @param handle
 *   The handle to the rx st2110-20 pipeline session.
 * @param ext_frame
 *   The pointer to the structure describing external framebuffer.
 * @return
 *   - NULL if no available frame in the session.
 *   - Otherwise, the frame pointer.
 */
struct st_frame* st20p_rx_get_ext_frame(st20p_rx_handle handle,
                                        struct st_ext_frame* ext_frame);

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
 * Retrieve the general statistics(I/O) for one rx st2110-20(pipeline) session port.
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
int st20p_rx_get_port_stats(st20p_rx_handle handle, enum mtl_session_port port,
                            struct st20_rx_port_status* stats);

/**
 * Reset the general statistics(I/O) for one rx st2110-20(pipeline) session port.
 *
 * @param handle
 *   The handle to the rx st2110-20(pipeline) session.
 * @param port
 *   The port index.
 * @return
 *   - >=0 succ.
 *   - <0: Error code.
 */
int st20p_rx_reset_port_stats(st20p_rx_handle handle, enum mtl_session_port port);

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
  return frame->linesize[plane] * frame->height;
}

#if defined(__cplusplus)
}
#endif

#endif
