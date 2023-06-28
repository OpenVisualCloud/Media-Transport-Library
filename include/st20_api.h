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
 * User control the frame pacing by pass a timestamp in st20_tx_frame_meta,
 * lib will wait until timestamp is reached for each frame.
 */
#define ST20_TX_FLAG_USER_PACING (MTL_BIT32(3))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enabled, lib will assign the rtp timestamp to the value in
 * st20_tx_frame_meta(ST10_TIMESTAMP_FMT_MEDIA_CLK is used)
 */
#define ST20_TX_FLAG_USER_TIMESTAMP (MTL_BIT32(4))
/**
 * Flag bit in flags of struct st20_tx_ops.
 * If enabled, lib will pass ST_EVENT_VSYNC by the notify_event on every epoch start.
 */
#define ST20_TX_FLAG_ENABLE_VSYNC (MTL_BIT32(5))

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
  ST20_FMT_RGB_8BIT,          /**< 8-bit RGB */
  ST20_FMT_RGB_10BIT,         /**< 10-bit RGB */
  ST20_FMT_RGB_12BIT,         /**< 12-bit RGB */
  ST20_FMT_RGB_16BIT,         /**< 16-bit RGB */
  ST20_FMT_YUV_444_8BIT,      /**< 8-bit YUV 4:4:4 */
  ST20_FMT_YUV_444_10BIT,     /**< 10-bit YUV 4:4:4 */
  ST20_FMT_YUV_444_12BIT,     /**< 12-bit YUV 4:4:4 */
  ST20_FMT_YUV_444_16BIT,     /**< 16-bit YUV 4:4:4 */
  ST20_FMT_MAX,               /**< max value of this enum */
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
};

/**
 * Slice meta data of st2110-20(video) tx streaming
 */
struct st20_tx_slice_meta {
  /** Ready lines */
  uint16_t lines_ready;
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
  /** codestream_size for next_frame_idx, set by user */
  size_t codestream_size;
  /** Timestamp format, user can customize it if ST22_TX_FLAG_USER_PACING */
  enum st10_timestamp_fmt tfmt;
  /** Timestamp value, user can customize it if ST22_TX_FLAG_USER_PACING */
  uint64_t timestamp;
  /** epoch */
  uint64_t epoch;
};

/**
 * Frame meta data of st2110-22(video) rx streaming
 */
struct st22_rx_frame_meta {
  /** Frame timestamp format */
  enum st10_timestamp_fmt tfmt;
  /** Frame timestamp value */
  uint64_t timestamp;
  /** Frame total size */
  size_t frame_total_size;
  /** Frame status, complete or not */
  enum st_frame_status status;
};

/**
 * The Continuation bit shall be set to 1 if an additional Sample Row Data.
 * Header follows the current Sample Row Data Header in the RTP Payload
 * Header, which signals that the RTP packet is carrying data for more than one
 * sample row. The Continuation bit shall be set to 0 otherwise.
 */
#define ST20_SRD_OFFSET_CONTINUATION (0x1 << 15)
/**
 * The field identification bit shall be set to 1 if the payload comes from second
 * field.The field identification bit shall be set to 0 otherwise.
 */
#define ST20_SECOND_FIELD (0x1 << 15)

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
  /** Interlaced information */
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
 * The structure describing how to create a tx st2110-20(video) session.
 * Include the PCIE port and other required info.
 */
struct st20_tx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
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

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session streaming type, frame or RTP */
  enum st20_type type;
  /** Session packing mode */
  enum st20_packing packing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /**
   * Session linesize(stride) in bytes, 0 if not set
   * Valid linesize should be wider than width size
   */
  uint32_t linesize;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** interlace or not, false: non-interlaced: true: interlaced */
  bool interlaced;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** flags, value in ST20_TX_FLAG_* */
  uint32_t flags;
  /**
   * tx destination mac address.
   * Valid if ST20_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];
  /**
   * vrx buffer for tx.
   * Leave to zero if not know detail, lib will assign vrx(narrow) based on resolution and
   * timing.
   * Refer to st21 spec for the possible vrx value, lib will follow this VRX if it's not a
   * zero value, and also fine tune is required since network setup difference.
   */
  uint16_t vrx;

  /**
   * the frame buffer count requested for one st20 tx session,
   * should be in range [2, ST20_FB_MAX_COUNT],
   * only for ST20_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib require a new frame.
   * User should provide the next available frame index to next_frame_idx.
   * It implicit means the frame ownership will be transferred to lib.
   * only for ST20_TYPE_FRAME_LEVEL.
   * And only non-block method can be used within this callback, as it run from lcore
   * tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st20_tx_frame_meta* meta);
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * only for ST20_TYPE_FRAME_LEVEL.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st20_tx_frame_meta* meta);
  /**
   * ST20_TYPE_SLICE_LEVEL callback when lib requires new lines.
   * User should provide the ready lines number.
   */
  int (*query_frame_lines_ready)(void* priv, uint16_t frame_idx,
                                 struct st20_tx_slice_meta* meta);

  /**
   * rtp ring size, must be power of 2
   * only for ST20_TYPE_RTP_LEVEL
   */
  uint32_t rtp_ring_size;
  /**
   * total pkts in one rtp frame, ex: 4320 for 1080p,
   * only for ST20_TYPE_RTP_LEVEL
   */
  uint32_t rtp_frame_total_pkts;
  /**
   * size for each rtp pkt, both the data and rtp header,
   * must small than MTL_PKT_MAX_RTP_BYTES,
   * only for ST20_TYPE_RTP_LEVEL.
   */
  uint16_t rtp_pkt_size;
  /**
   * ST20_TYPE_RTP_LEVEL callback when lib consume one rtp packet,
   * only for ST20_TYPE_RTP_LEVEL,
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_rtp_done)(void* priv);

  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
};

/**
 * The structure describing how to create a tx st2110-22(compressed video) session.
 * Include the PCIE port and other required info.
 */
struct st22_tx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
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

  /** Session streaming type, frame or RTP */
  enum st22_type type;

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;
  /** flags, value in ST22_TX_FLAG_* */
  uint32_t flags;
  /**
   * tx destination mac address.
   * Valid if ST22_TX_FLAG_USER_P(R)_MAC is enabled
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /**
   * the frame buffer count requested for one st22 tx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   * only for ST22_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * max framebuffer size for one st22 tx session codestream,
   * usually ST22 use constant bitrate (CBR) mode.
   * lib will allocate all frame buffer with this size,
   * app can indicate the real codestream size later in get_next_frame query.
   * only for ST22_TYPE_FRAME_LEVEL.
   */
  size_t framebuff_max_size;
  /**
   * ST22_TYPE_FRAME_LEVEL callback when lib require a new frame.
   * User should provide the next available frame index to next_frame_idx.
   * It implicit means the frame ownership will be transferred to lib.
   * only for ST22_TYPE_FRAME_LEVEL.
   * And only non-block method can be used within this callback, as it run from lcore
   * tasklet routine.
   * next_frame_idx: next available frame index.
   * meta: meta for next_frame_idx.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx,
                        struct st22_tx_frame_meta* meta);
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * only for ST20_TYPE_FRAME_LEVEL.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx,
                           struct st22_tx_frame_meta* meta);

  /** rtp ring size, must be power of 2, for ST22_TYPE_RTP_LEVEL */
  uint32_t rtp_ring_size;
  /** total pkts in one rtp frame, for ST22_TYPE_RTP_LEVEL */
  uint32_t rtp_frame_total_pkts;
  /**
   * size for each rtp pkt, both the data and rtp header,
   * must small than MTL_PKT_MAX_RTP_BYTES.
   * for ST22_TYPE_RTP_LEVEL
   */
  uint16_t rtp_pkt_size;
  /**
   * callback when lib consume one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   * for ST22_TYPE_RTP_LEVEL
   */
  int (*notify_rtp_done)(void* priv);

  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
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
 * The structure describing how to create a rx st2110-20(video) session.
 * Include the PCIE port and other required info.
 */
struct st20_rx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** source IP address of sender */
  uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /** UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session streaming type, frame or RTP */
  enum st20_type type;
  /** Session packing mode */
  enum st20_packing packing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /**
   * Session linesize(stride) in bytes, 0 if not set
   * Valid linesize should be wider than width size
   */
  uint32_t linesize;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** flags, value in ST20_RX_FLAG_* */
  uint32_t flags;
  /** interlace or not, false: non-interlaced: true: interlaced */
  bool interlaced;

  /**
   * the ST20_TYPE_FRAME_LEVEL frame buffer count requested,
   * should be in range [2, ST20_FB_MAX_COUNT].
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * the ST20_TYPE_FRAME_LEVEL external frame buffer info array,
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   */
  struct st20_ext_frame* ext_frames;
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib receive one frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st20_rx_put_framebuff
   * to return the frame when it finish the handling
   *   < 0: the error code if app can't handle, lib will free the frame then.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void* priv, void* frame, struct st20_rx_frame_meta* meta);

  /**
   * Total size for user frame, lib will allocate frame with this value. When lib receive
   * a payload from network, it will call uframe_pg_callback to let user to handle the
   * pixel group data in the payload, the callback should convert the pixel group data to
   * the data format app required.
   * Zero means the user frame mode is disabled.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   */
  size_t uframe_size;
  /**
   * User frame callback when lib receive pixel group data from network.
   * frame: point to the address of the user frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the pixel group successfully.
   *   < 0: the error code if app can't handle.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*uframe_pg_callback)(void* priv, void* frame, struct st20_rx_uframe_pg_meta* meta);

  /** lines in one slice, for ST20_TYPE_SLICE_LEVEL */
  uint32_t slice_lines;
  /**
   * ST20_TYPE_SLICE_LEVEL callback when lib received slice info for one frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_slice_ready)(void* priv, void* frame, struct st20_rx_slice_meta* meta);

  /**
   * rtp ring size, must be power of 2.
   * Only for ST20_TYPE_RTP_LEVEL.
   */
  uint32_t rtp_ring_size;
  /**
   * ST20_TYPE_RTP_LEVEL callback when lib receive one rtp packet.
   * Only for ST20_TYPE_RTP_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
  /**
   * ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL callback when lib detected video format.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_detected)(void* priv, const struct st20_detect_meta* meta,
                         struct st20_detect_reply* reply);
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib query next external frame's data address.
   * Only for ST20_TYPE_FRAME_LEVEL with ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
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

/**
 * The structure describing how to create a rx st2110-22(compressed video) session.
 * Include the PCIE port and other required info.
 */
struct st22_rx_ops {
  /** name */
  const char* name;
  /** private data to the callback function */
  void* priv;
  /** source IP address of sender */
  uint8_t sip_addr[MTL_SESSION_PORT_MAX][MTL_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of mtl_init */
  char port[MTL_SESSION_PORT_MAX][MTL_PORT_MAX_LEN];
  /** UDP source port number, leave as 0 to use same port as dst */
  uint16_t udp_src_port[MTL_SESSION_PORT_MAX];
  /** UDP destination port number */
  uint16_t udp_port[MTL_SESSION_PORT_MAX];
  /** flags, value in ST22_RX_FLAG_* */
  uint32_t flags;

  /** Session streaming type, frame or RTP */
  enum st22_type type;

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** packetization modes define in RFC9134 */
  enum st22_pack_type pack_type;

  /**
   * the frame buffer count requested for one st22 rx session,
   * should be in range [2, ST22_FB_MAX_COUNT],
   * only for ST22_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * max framebuffer size for one st22 rx session,
   * usually ST22 use constant bitrate (CBR) mode.
   * lib will allocate all frame buffer with this size,
   * app can get the real codestream size later in notify_frame_ready callback.
   * only for ST22_TYPE_FRAME_LEVEL.
   */
  size_t framebuff_max_size;
  /**
   * ST22_TYPE_FRAME_LEVEL callback when lib receive one frame.
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

  /** rtp ring size, must be power of 2, for ST22_TYPE_RTP_LEVEL */
  uint32_t rtp_ring_size;
  /**
   * callback when lib receive one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   * For ST22_TYPE_RTP_LEVEL.
   */
  int (*notify_rtp_ready)(void* priv);

  /**
   * event callback, lib will call this when there is some event happened.
   * Only non-block method can be used in this callback as it run from lcore routine.
   * args point to the meta data of each event.
   * Ex, cast to struct st10_vsync_meta for ST_EVENT_VSYNC.
   */
  int (*notify_event)(void* priv, enum st_event event, void* args);
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

#if defined(__cplusplus)
}
#endif

#endif
