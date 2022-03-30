/*
 * Copyright (C) 2021 Intel Corporation.
 *
 * This software and the related documents are Intel copyrighted materials,
 * and your use of them is governed by the express license under which they
 * were provided to you ("License").
 * Unless the License provides otherwise, you may not use, modify, copy,
 * publish, distribute, disclose or transmit this software or the related
 * documents without Intel's prior written permission.
 *
 * This software and the related documents are provided as is, with no
 * express or implied warranties, other than those that are expressly stated
 * in the License.
 *
 */

/**
 * @file st_dpdk_api.h
 *
 * Interfaces to Intel(R) Media Streaming Library
 *
 * This header define the public interfaces of Intel(R) Media Streaming Library
 *
 */

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef _ST_DPDK_API_HEAD_H_
#define _ST_DPDK_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Major version number of Media Streaming Library
 */
#define ST_VERSION_MAJOR (22)
/**
 * Minor version number of Media Streaming Library
 */
#define ST_VERSION_MINOR (4)
/**
 * Last version number of Media Streaming Library
 */
#define ST_VERSION_LAST (0)
/**
 * Macro to compute a version number usable for comparisons
 */
#define ST_VERSION_NUM(a, b, c) ((a) << 16 | (b) << 8 | (c))
/**
 * All version numbers in one to compare with ST_VERSION_NUM()
 */
#define ST_VERSION ST_VERSION_NUM(ST_VERSION_MAJOR, ST_VERSION_MINOR, ST_VERSION_LAST)

/**
 * Max length of a DPDK port name
 */
#define ST_PORT_MAX_LEN (64)
/**
 * Length of a IPV4 address
 */
#define ST_IP_ADDR_LEN (4)
/**
 * Defined if current platform is little endian
 */
#define ST_LITTLE_ENDIAN /* x86 use little endian */

/**
 * Max bytes in one RTP packet, include payload and header
 * standard UDP is 1460 bytes, and UDP headers are 8 bytes
 */
#define ST_PKT_MAX_RTP_BYTES (1460 - 8)

/**
 * Max allowed number of video(st20) frame buffers
 */
#define ST20_FB_MAX_COUNT (8)

/**
 * Max allowed number of dma devs
 */
#define ST_DMA_DEV_MAX (8)

/**
 * Handle to media streaming device context
 */
typedef void* st_handle;
/**
 * Handle to tx st2110-20(video) session
 */
typedef void* st20_tx_handle;
/**
 * Handle to tx st2110-22(compressed video) session
 */
typedef void* st22_tx_handle;
/**
 * Handle to tx st2110-30(audio) session
 */
typedef void* st30_tx_handle;
/**
 * Handle to tx st2110-40(ancillary) session
 */
typedef void* st40_tx_handle;
/**
 * Handle to rx st2110-20(video) session
 */
typedef void* st20_rx_handle;
/**
 * Handle to rx st2110-22(compressed video) session
 */
typedef void* st22_rx_handle;
/**
 * Handle to rx st2110-30(audio) session
 */
typedef void* st30_rx_handle;
/**
 * Handle to rx st2110-40(ancillary) session
 */
typedef void* st40_rx_handle;

/**
 * Handle to st user dma device
 */
typedef void* st_udma_handle;

/**
 * IO virtual address type.
 */
typedef uint64_t st_iova_t;
/**
 * Bad IOVA address
 */
#define ST_BAD_IOVA ((st_iova_t)-1)

/**
 * Port logical type
 */
enum st_port {
  ST_PORT_P = 0, /**< primary port */
  ST_PORT_R,     /**< redundant port */
  ST_PORT_MAX,   /**< max value of this enum */
};

/**
 * Log level type to media context
 */
enum st_log_level {
  ST_LOG_LEVEL_DEBUG = 0, /**< debug log level */
  ST_LOG_LEVEL_INFO,      /**< info log level */
  ST_LOG_LEVEL_WARNING,   /**< warning log level */
  ST_LOG_LEVEL_ERROR,     /**< error log level */
  ST_LOG_LEVEL_MAX,       /**< max value of this enum */
};

/**
 * SIMD level type
 */
enum st_simd_level {
  ST_SIMD_LEVEL_NONE = 0,     /**< Scalar */
  ST_SIMD_LEVEL_AVX2,         /**< AVX2 */
  ST_SIMD_LEVEL_AVX512,       /**< AVX512 */
  ST_SIMD_LEVEL_AVX512_VBMI2, /**< AVX512 VBMI2 */
  ST_SIMD_LEVEL_MAX,          /**< max value of this enum */
};

/**
 * Timestamp type of st2110-10
 */
enum st10_timestamp_fmt {
  /**
   * the raw media clock value defined in ST2110-10, whose units vary by essence
   * sampling rate(90k for video, 48K for audio).
   */
  ST10_TIMESTAMP_FMT_MEDIA_CLK = 0,
  /** the media clock time in nanoseconds since the TAI epoch */
  ST10_TIMESTAMP_FMT_TAI,
  /** max value of this enum */
  ST10_TIMESTAMP_FMT_MAX,
};

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
 * FPS type of media streaming, Frame per second or Field per second
 */
enum st_fps {
  ST_FPS_P59_94 = 0, /**< 59.94 fps */
  ST_FPS_P50,        /**< 50 fps */
  ST_FPS_P29_97,     /**< 29.97 fps */
  ST_FPS_P25,        /**< 25 fps*/
  ST_FPS_MAX,        /**< max value of this enum */
};

/**
 * FILED type of the media buffer
 */
enum st_field {
  FIRST_FIELD = 0,
  SECOND_FIELD = 1,
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
 * Frame status type of st2110-20(video) rx streaming
 */
enum st20_frame_status {
  /** All pixels of the frame were received */
  ST20_FRAME_STATUS_COMPLETE = 0,
  /**
   * There was some packet loss, but the complete frame was reconstructed using packets
   * from primary and redundant streams
   */
  ST20_FRAME_STATUS_RECONSTRUCTED,
  /** Packets were lost */
  ST20_FRAME_STATUS_CORRUPTED,
  /** Max value of this enum */
  ST20_FRAME_STATUS_MAX,
};

/**
 * Inline function to check the st2110-20 rx frame is a completed frame.
 * @param status
 *   The input frame status.
 * @return
 *     Complete or not.
 */
static inline bool st20_is_frame_complete(enum st20_frame_status status) {
  if ((status == ST20_FRAME_STATUS_COMPLETE) ||
      (status == ST20_FRAME_STATUS_RECONSTRUCTED))
    return true;
  else
    return false;
}

/**
 * Session packing mode of st2110-20(video) streaming
 */
enum st20_packing {
  ST20_PACKING_GPM_SL = 0, /**< general packing mode, single scan line */
  ST20_PACKING_BPM,        /**< block packing mode */
  ST20_PACKING_GPM,        /**< general packing mode */
  ST20_PACKING_MAX,        /**< max value of this enum */
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
};

/**
 * Frame meta data of st2110-20(video) rx streaming
 */
struct st20_frame_meta {
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
  enum st20_frame_status status;
  /** Frame total size */
  size_t frame_total_size;
  /** The total size for user frame */
  size_t uframe_total_size;
  /** field type indicate */
  enum st_field field;
  /**
   * The actual received size for current frame, user can inspect frame_recv_size
   * against frame_total_size to check the signal integrity for incomplete frame.
   */
  size_t frame_recv_size;
};

/**
 * Slice meta data of st2110-20(video) rx streaming
 */
struct st20_slice_meta {
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
  /** field type indicate */
  enum st_field field;
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
struct st20_uframe_pg_meta {
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
};

/**
 * A structure describing rfc3550 rtp header, size: 12
 */
struct st_rfc3550_rtp_hdr {
#ifdef ST_LITTLE_ENDIAN
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
#else
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
#endif
  /** sequence number */
  uint16_t seq_number;
  /** timestamp */
  uint32_t tmstamp;
  /** synchronization source */
  uint32_t ssrc;
} __attribute__((__packed__));

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
struct st20_rfc4175_rtp_hdr {
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
} __attribute__((__packed__));

/**
 * A structure describing a st2110-20(video) rfc4175 rtp additional header.
 * if Continuation bit is set in struct st20_rfc4175_rtp_hdr. size: 6
 */
struct st20_rfc4175_extra_rtp_hdr {
  /** Number of octets of data included from this scan line */
  uint16_t row_length;
  /** Scan line number */
  uint16_t row_number;
  /** Offset of the first pixel of the payload data within the scan line */
  uint16_t row_offset;
} __attribute__((__packed__));

/** Pixel Group describing two image pixels in YUV 4:2:2 10-bit format */
struct st20_rfc4175_422_10_pg2_be {
  uint8_t Cb00; /**< First 8 bit Blue */
#ifdef ST_LITTLE_ENDIAN
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
#else
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
#endif
  uint8_t Y01_; /**< Second 2 bits Luminance for Y1 */
} __attribute__((__packed__));

/** Pixel Group describing two image pixels in YUV 4:2:2 10-bit format */
struct st20_rfc4175_422_10_pg2_le {
  uint8_t Cb00; /**< First 8 bit Blue */
#ifdef ST_LITTLE_ENDIAN
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
#else
  uint8_t Y00 : 6;   /**< First 6 bits Luminance for Y0 */
  uint8_t Cb00_ : 2; /**< Second 2 bit Blue */
  uint8_t Cr00 : 4;  /**< First 4 bit Red */
  uint8_t Y00_ : 4;  /**< Second 4 bits Luminance for Y0 */
  uint8_t Y01 : 2;   /**< First 2 bits Luminance for Y1 */
  uint8_t Cr00_ : 6; /**< Second 6 bit Red */
#endif
  uint8_t Y01_; /**< Second 2 bits Luminance for Y1 */
} __attribute__((__packed__));

/** Pixel Group describing two image pixels in YUV 4:2:2 8-bit format */
struct st20_rfc4175_422_8_pg2_le {
  uint8_t Cb00; /**< 8 bit Blue */
  uint8_t Y00;  /**< 8 bit Y0 */
  uint8_t Cr00; /**< 8 bit Red */
  uint8_t Y01;  /**< 8 bit Y1 */
} __attribute__((__packed__));

/**
 * A structure describing a st2110-40(ancillary) rfc8331 rtp header
 */
struct st40_rfc8331_rtp_hdr {
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
} __attribute__((__packed__));

/**
 * A structure describing a st2110-40(ancillary) rfc8331 payload header
 */
struct st40_rfc8331_payload_hdr {
#ifdef ST_LITTLE_ENDIAN
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
#else
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
#endif
} __attribute__((__packed__));

/**
 * Flag bit in flags of struct st_init_params.
 * If set, lib will call numa_bind to bind app thread and memory to NIC socket also.
 */
#define ST_FLAG_BIND_NUMA (0x1 << 0)
/**
 * Flag bit in flags of struct st_init_params.
 * Enable built-in PTP implementation, only for PF now.
 * If not enable, it will use system time as the PTP source.
 */
#define ST_FLAG_PTP_ENABLE (0x1 << 1)
/**
 * Flag bit in flags of struct st_init_params.
 * Separated lcore for RX video(st2110-20/st2110-22) session.
 */
#define ST_FLAG_RX_SEPARATE_VIDEO_LCORE (0x1 << 2)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * dedicate thread for cni message
 */
#define ST_FLAG_CNI_THREAD (0x1 << 16)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Enable video rx ebu check
 */
#define ST_FLAG_RX_VIDEO_EBU (0x1 << 17)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * P TX destination mac assigned by user
 */
#define ST_FLAG_USER_P_TX_MAC (0x1 << 18)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * R TX destination mac assigned by user
 */
#define ST_FLAG_USER_R_TX_MAC (0x1 << 19)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Enable NIC promiscuous mode for RX
 */
#define ST_FLAG_NIC_RX_PROMISCUOUS (0x1 << 20)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * use unicast address for ptp PTP_DELAY_REQ message
 */
#define ST_FLAG_PTP_UNICAST_ADDR (0x1 << 21)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Mono memory pool for all rx queue
 */
#define ST_FLAG_RX_QUEUE_MONO_POOL (0x1 << 22)
/**
 * Flag bit in flags of struct st_init_params, debug usage only.
 * Use TSC pacing instead of the RL
 */
#define ST_FLAG_TSC_PACING (0x1 << 23)

/**
 * The structure describing how to init the streaming dpdk context.
 * Include the PCIE port and other required info.
 */
struct st_init_params {
  /** Pcie BDF path like 0000:af:00.0 */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** number of pcie ports, 1 or 2 */
  uint8_t num_ports;
  /** source IP of courrent port */
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /**
   * max tx sessions(st20, st22, st30, st40) requested the lib to support,
   * use st_get_cap to query the actual count.
   * dpdk context will allocate the hw resources(queues, memory) based on this number.
   */
  uint16_t tx_sessions_cnt_max;
  /**
   * max rx sessions(st20, st22, st30, st40) requested the lib to support,
   * use st_get_cap to query the actual count.
   * dpdk context will allocate the hw resources(queues, memory) based on this number.
   */
  uint16_t rx_sessions_cnt_max;
  /**
   * logical cores list can be used, e.g. "28,29,30,31".
   * NULL means determined by system itself
   */
  char* lcores;
  /** dma(CBDMA or DSA) dev Pcie BDF path like 0000:80:04.0 */
  char dma_dev_port[ST_DMA_DEV_MAX][ST_PORT_MAX_LEN];
  /** number of dma dev ports in dma_dev_port, leave to zero if no dma dev */
  uint8_t num_dma_dev_port;
  /** log level */
  enum st_log_level log_level;
  /** flags, value in ST_FLAG_* */
  uint64_t flags;
  /** private data to the callback function */
  void* priv;
  /**
   * Function to acquire current ptp time(in nanoseconds) from user.
   * if NULL, ST instance will get from built-in ptp source(NIC) or system time instead.
   */
  uint64_t (*ptp_get_time_fn)(void* priv);
  /** stats dump peroid in seconds, 0 means determined by lib */
  uint16_t dump_period_s;
  /** stats dump callabck in every dump_period_s */
  void (*stat_dump_cb_fn)(void* priv);
  /** data quota for each lcore, 0 means determined by lib */
  uint32_t data_quota_mbs_per_sch;
  /**
   * number of transmit descriptors for each NIC TX queue, 0 means determined by lib.
   * It will affect the memory usage and the performance.
   */
  uint16_t nb_tx_desc;
  /**
   * number of receive descriptors for each NIC RX queue, 0 means determined by lib.
   * It will affect the memory usage and the performance.
   */
  uint16_t nb_rx_desc;
  /**
   * tx destination mac address, debug usage only.
   * Valid if ST_FLAG_USER_P(R)_TX_MAC is enabled
   */
  uint8_t tx_dst_mac[ST_PORT_MAX][6];
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
  uint8_t dip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];

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
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** interlace or not false: non-interlaced: true: interlaced*/
  bool interlaced;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /**
   * the frame buffer count requested for one st20 tx session,
   * should be in range [2, ST20_FB_MAX_COUNT],
   * only for ST20_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib require a new frame.
   * User should provide the next avaiable frame index to next_frame_idx.
   * It implicit means the frame ownership will be transferred to lib.
   * only for ST20_TYPE_FRAME_LEVEL.
   * And only non-block method can be used within this callback, as it run from lcore
   * tasklet routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx, bool* second_field);
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * only for ST20_TYPE_FRAME_LEVEL.
   * And only non-block method can be used within this callback as it run from lcore
   * tasklet routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx);
  /**
   * ST20_TYPE_SLICE_LEVEL callback when lib requires new lines.
   * User should provide the ready lines number.
   */
  int (*query_frame_lines_ready)(void* priv, uint16_t frame_idx, uint16_t* lines_ready);

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
   * must small than ST_PKT_MAX_RTP_BYTES,
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
  uint8_t dip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /** rtp ring size, must be power of 2 */
  uint32_t rtp_ring_size;
  /** total pkts in one rtp frame */
  uint32_t rtp_frame_total_pkts;
  /**
   * size for each rtp pkt, both the data and rtp header,
   * must small than ST_PKT_MAX_RTP_BYTES.
   */
  uint16_t rtp_pkt_size;
  /**
   * callback when lib consume one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_done)(void* priv);
};

/**
 * PCM type of st2110-30(audio) streaming
 */
enum st30_fmt {
  ST30_FMT_PCM8 = 0, /**< 8 bits per channel */
  ST30_FMT_PCM16,    /**< 16 bits per channel */
  ST30_FMT_PCM24,    /**< 24 bits per channel */
  ST30_FMT_MAX,      /**< max value of this enum */
};

/**
 * Sampling type of st2110-30(audio) streaming
 */
enum st30_sampling {
  ST30_SAMPLING_48K = 0, /**< media Clock rate of 48kHz */
  ST30_SAMPLING_96K,     /**< media Clock rate of 96kHz */
  ST30_SAMPLING_MAX,     /**< max value of this enum */
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
 * Frame meta data of st2110-30(audio) rx streaming
 */
struct st30_frame_meta {
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

  /** Session PCM format */
  enum st30_fmt fmt;
  /** Session channel number */
  uint16_t channel;
  /** Session sampling format */
  enum st30_sampling sampling;
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
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx);
  /**
   * ST30_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * only for ST30_TYPE_FRAME_LEVEL,
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx);

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
 * Session type of st2110-40(ancillary) streaming
 */
enum st40_type {
  ST40_TYPE_FRAME_LEVEL = 0, /**< app interface lib based on frame level */
  ST40_TYPE_RTP_LEVEL,       /**< app interface lib based on RTP level */
  ST40_TYPE_MAX,             /**< max value of this enum */
};

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
 * The structure describing how to create a tx st2110-40(ancillary) session.
 * Include the PCIE port and other required info.
 */
struct st40_tx_ops {
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
  /** Session streaming type, frame or RTP */
  enum st40_type type;
  /** Session fps */
  enum st_fps fps;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /**
   * the frame buffer count requested for one st40 tx session,
   * only for ST40_TYPE_FRAME_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * ST40_TYPE_FRAME_LEVEL callback when lib require a new frame.
   * User should provide the next avaiable frame index to next_frame_idx.
   * It implicit means the frame ownership will be transferred to lib,
   * only for ST40_TYPE_FRAME_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*get_next_frame)(void* priv, uint16_t* next_frame_idx);
  /**
   * ST30_TYPE_FRAME_LEVEL callback when lib finish current frame.
   * frame_idx indicate the frame which finish the transmit.
   * It implicit means the frame ownership is transferred to app.
   * only for ST40_TYPE_FRAME_LEVEL,
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_done)(void* priv, uint16_t frame_idx);

  /**
   * rtp ring size, must be power of 2.
   * only for ST40_TYPE_RTP_LEVEL
   */
  uint32_t rtp_ring_size;
  /**
   * ST40_TYPE_RTP_LEVEL callback when lib consume one rtp packet,
   * only for ST40_TYPE_RTP_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_done)(void* priv);
};

/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
 * If set, lib will pass the incomplete frame to app also by notify_frame_ready.
 * User can check st20_frame_meta data for the frame integrity
 */
#define ST20_RX_FLAG_RECEIVE_INCOMPLETE_FRAME (0x1 << 0)
/**
 * Flag bit in flags of struct st20_rx_ops.
 * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
 * If set, lib will try to allocate DMA memory copy offload from
 * dma_dev_port(st_init_params) list.
 * Pls note it could fallback to CPU if no DMA device is available.
 */
#define ST20_RX_FLAG_DMA_OFFLOAD (0x1 << 1)

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
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];

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
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** flags, value in ST20_RX_FLAG_* */
  uint32_t flags;
  /** interlace or not false: non-interlaced: true: interlaced*/
  bool interlaced;

  /**
   * the ST20_TYPE_FRAME_LEVEL frame buffer count requested,
   * should be in range [2, ST20_FB_MAX_COUNT].
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   */
  uint16_t framebuff_cnt;
  /**
   * ST20_TYPE_FRAME_LEVEL callback when lib receive one frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the frame successful. App should call st20_rx_put_framebuff
   * to return the frame when it finish the handling
   *   < 0: the error code if app cann't handle, lib will free the frame then.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_frame_ready)(void* priv, void* frame, struct st20_frame_meta* meta);

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
   * User frame callback when lib receive pixel group datas from network.
   * frame: point to the address of the user frame buf.
   * meta: point to the meta data.
   * return:
   *   - 0: if app consume the pixel group successfully.
   *   < 0: the error code if app cann't handle.
   * Only for ST20_TYPE_FRAME_LEVEL/ST20_TYPE_SLICE_LEVEL.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*uframe_pg_callback)(void* priv, void* frame, struct st20_uframe_pg_meta* meta);

  /** lines in one slice, for ST20_TYPE_SLICE_LEVEL */
  uint32_t slice_lines;
  /**
   * ST20_TYPE_SLICE_LEVEL callback when lib received slice info for one frame.
   * frame: point to the address of the frame buf.
   * meta: point to the meta data.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_slice_ready)(void* priv, void* frame, struct st20_slice_meta* meta);

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
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** 1 or 2, num of ports this session attached to */
  uint8_t num_port;
  /** Pcie BDF path like 0000:af:00.0, should align to BDF of st_init */
  char port[ST_PORT_MAX][ST_PORT_MAX_LEN];
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];

  /** Sender pacing type */
  enum st21_pacing pacing;
  /** Session resolution width */
  uint32_t width;
  /** Session resolution height */
  uint32_t height;
  /** Session resolution fps */
  enum st_fps fps;
  /** Session resolution format */
  enum st20_fmt fmt;
  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;

  /** rtp ring size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * callback when lib receive one rtp packet.
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
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

  /** Session PCM format */
  enum st30_fmt fmt;
  /** Session channel number */
  uint16_t channel;
  /** Session sampling format */
  enum st30_sampling sampling;
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
  int (*notify_frame_ready)(void* priv, void* frame, struct st30_frame_meta* meta);

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
 * The structure describing how to create a rx st2110-40(ancillary) session.
 * Include the PCIE port and other required info
 */
struct st40_rx_ops {
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

  /** 7 bits payload type define in RFC3550 */
  uint8_t payload_type;
  /** rtp ring size, must be power of 2 */
  uint32_t rtp_ring_size;
  /**
   * callback when lib consume current rtp packet
   * And only non-block method can be used in this callback as it run from lcore tasklet
   * routine.
   */
  int (*notify_rtp_ready)(void* priv);
};

/**
 * The structure describing the source address(ip addr and port) info for RX.
 * Leave redundant info to zero if the session only has primary port.
 */
struct st_rx_source_info {
  /** source IP address of sender */
  uint8_t sip_addr[ST_PORT_MAX][ST_IP_ADDR_LEN];
  /** UDP port number */
  uint16_t udp_port[ST_PORT_MAX];
};

/**
 * A structure used to retrieve capacity for an ST instance.
 */
struct st_cap {
  /** max tx session count for current straming context */
  uint16_t tx_sessions_cnt_max;
  /** max rx session count for current straming context */
  uint16_t rx_sessions_cnt_max;
  /** max dma dev count for current straming context */
  uint8_t dma_dev_cnt_max;
};

/**
 * A structure used to retrieve state for an ST instance.
 */
struct st_stats {
  /** st20 tx session count in current straming context */
  uint16_t st20_tx_sessions_cnt;
  /** st22 tx session count in current straming context */
  uint16_t st22_tx_sessions_cnt;
  /** st30 tx session count in current straming context */
  uint16_t st30_tx_sessions_cnt;
  /** st40 tx session count in current straming context */
  uint16_t st40_tx_sessions_cnt;
  /** st20 rx session count in current straming context */
  uint16_t st20_rx_sessions_cnt;
  /** st22 rx session count in current straming context */
  uint16_t st22_rx_sessions_cnt;
  /** st30 rx session count in current straming context */
  uint16_t st30_rx_sessions_cnt;
  /** st40 rx session count in current straming context */
  uint16_t st40_rx_sessions_cnt;
  /** active scheduler count in current straming context */
  uint8_t sch_cnt;
  /** active lcore count in current straming context */
  uint8_t lcore_cnt;
  /** active dma dev count for current straming context */
  uint8_t dma_dev_cnt;
  /** if straming device is started(st_start) */
  uint8_t dev_started;
};

/**
 * Inline function returning primary port pointer from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Primary port name pointer
 */
static inline char* st_p_port(struct st_init_params* p) { return p->port[ST_PORT_P]; }

/**
 * Inline function returning redundant port pointer from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Redundant port name pointer
 */
static inline char* st_r_port(struct st_init_params* p) { return p->port[ST_PORT_R]; }

/**
 * Inline helper function returning primary port source IP address pointer
 * from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Primary port IP address pointer
 */
static inline uint8_t* st_p_sip_addr(struct st_init_params* p) {
  return p->sip_addr[ST_PORT_P];
}

/**
 * Inline helper function returning redundant port source IP address pointer
 * from st_init_params
 * @param p
 *   The pointer to the init parameters.
 * @return
 *     Redundant port IP address pointer
 */
static inline uint8_t* st_r_sip_addr(struct st_init_params* p) {
  return p->sip_addr[ST_PORT_R];
}

/**
 * Function returning version string
 * @return
 *     ST version string
 */
const char* st_version(void);

/**
 * Initialize the media streaming device context which based on DPDK.
 *
 * @param p
 *   The pointer to the init parameters.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the media streaming device context.
 */
st_handle st_init(struct st_init_params* p);

/**
 * Un-initialize the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device un-initialized.
 *   - <0: Error code of the device un-initialize.
 */
int st_uninit(st_handle st);

/**
 * Start the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device started.
 *   - <0: Error code of the device start.
 */
int st_start(st_handle st);

/**
 * Stop the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device stopped.
 *   - <0: Error code of the device stop.
 */
int st_stop(st_handle st);

/**
 * Abort the media streaming device context.
 * Usually called in the exception case, e.g CTRL-C.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - 0: Success, device aborted.
 *   - <0: Error code of the device abort.
 */
int st_request_exit(st_handle st);

/**
 * Retrieve the capacity of the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param cap
 *   A pointer to a structure of type *st_cap* to be filled.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_cap(st_handle st, struct st_cap* cap);

/**
 * Retrieve the stat info of the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param stats
 *   A pointer to a structure of type *st_stats* to be filled.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_stats(st_handle st, struct st_stats* stats);

/**
 * Request one DPDK lcore from the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param lcore
 *   A pointer to the retured lcore number.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_get_lcore(st_handle st, unsigned int* lcore);

/**
 * Bind one thread to lcore.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param thread
 *   the thread wchich request the bind action.
 * @param lcore
 *   the DPDK lcore which requested by st_get_lcore.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_bind_to_lcore(st_handle st, pthread_t thread, unsigned int lcore);

/**
 * Put back the DPDK lcore which requested from the media streaming device context.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param lcore
 *   the DPDK lcore which requested by st_get_lcore.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st_put_lcore(st_handle st, unsigned int lcore);

/**
 * Performance optimized memcpy, e.g. AVX-512.
 *
 * @param dest
 *   Pointer to the destination of the data.
 * @param src
 *   Pointer to the source data.
 * @param n
 *   Number of bytes to copy..
 * @return
 *   - Pointer to the destination data.
 */
void* st_memcpy(void* dest, const void* src, size_t n);

/**
 * Allocate memory from the huge-page area of memory. The memory is not cleared.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the pointer to the allocated memory.
 */
void* st_hp_malloc(st_handle st, size_t size, enum st_port port);

/**
 * Allocate zero'ed memory from the huge-page area of memory.
 * Equivalent to st_hp_malloc() except that the memory zone is cleared with zero.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the pointer to the allocated memory.
 */
void* st_hp_zmalloc(st_handle st, size_t size, enum st_port port);

/**
 * Frees the memory pointed by the pointer.
 *
 * This pointer must have been returned by a previous call to
 * st_hp_malloc(), st_hp_zmalloc().
 * The behaviour is undefined if the pointer does not match this requirement.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ptr
 *   The pointer to memory to be freed.
 */
void st_hp_free(st_handle st, void* ptr);

/**
 * Return the IO address of a virtual address from st_hp_malloc/st_hp_zmalloc
 *
 * @param addr
 *   Address obtained from previous st_hp_malloc/st_hp_zmalloc call
 * @return
 *   ST_BAD_IOVA on error
 *   otherwise return an address suitable for IO
 */
st_iova_t st_hp_virt2iova(st_handle st, const void* addr);

/**
 * Allocate a user DMA dev from the dma_dev_port(st_init_params) list.
 * In NUMA systems, the dma dev allocated from the same NUMA socket of the port.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param size
 *   Number of descriptor for the user DMA device
 * @param port
 *   Port for the user DMA device to be allocated.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the st user dma dev.
 */
st_udma_handle st_udma_create(st_handle st, uint16_t nb_desc, enum st_port port);

/**
 * Free the st user dma dev.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @return
 *   - 0: Success.
 *   - <0: Error code of the free.
 */
int st_udma_free(st_udma_handle handle);

/**
 * Enqueue a copy operation onto the user dma dev.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The st_iova_t address of the destination buffer.
 *   Must be the memory address by st_hp_virt2iova.
 * @param src
 *   The st_iova_t address of the source buffer.
 *   Must be the memory address by st_hp_virt2iova.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
int st_udma_copy(st_udma_handle handle, st_iova_t dst, st_iova_t src, uint32_t length);

/**
 * Enqueue a fill operation onto the virtual DMA channel.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The st_iova_t address of the destination buffer.
 *   Must be the memory address by st_hp_virt2iova.
 * @param pattern
 *   The pattern(u64) to populate the destination buffer with.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
int st_udma_fill(st_udma_handle handle, st_iova_t dst, uint64_t pattern, uint32_t length);

/**
 * Enqueue a fill operation onto the virtual DMA channel.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param dst
 *   The st_iova_t address of the destination buffer.
 *   Must be the memory address by st_hp_virt2iova.
 * @param pattern
 *   The pattern(u8) to populate the destination buffer with.
 * @param length
 *   The length of the data to be copied.
 *
 * @return
 *   - 0..UINT16_MAX: index of enqueued job.
 *   - -ENOSPC: if no space left to enqueue.
 *   - other values < 0 on failure.
 */
static inline int st_udma_fill_u8(st_udma_handle handle, st_iova_t dst, uint8_t pattern,
                                  uint32_t length) {
  uint64_t pattern_u64;
  /* pattern to u64 */
  memset(&pattern_u64, pattern, sizeof(pattern_u64));
  return st_udma_fill(handle, dst, pattern_u64, length);
}

/**
 * Trigger hardware to begin performing enqueued operations.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @return
 *   - 0: Success.
 *   - <0: Error code of the submit.
 */
int st_udma_submit(st_udma_handle handle);

/**
 * Return the number of operations that have been successfully completed.
 *
 * @param handle
 *   The handle to the st user dma dev.
 * @param nb_cpls
 *   The maximum number of completed operations that can be processed.
 *
 * @return
 *   The number of operations that successfully completed. This return value
 *   must be less than or equal to the value of nb_cpls.
 */
uint16_t st_udma_completed(st_udma_handle handle, const uint16_t nb_cpls);

/**
 * Read current time from ptp source.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @return
 *   - The time in nanoseconds in current ptp system
 */
uint64_t st_ptp_read_time(st_handle st);

/**
 * Get SIMD level current cpu supported.
 *
 * @return
 *   - The simd level
 */
enum st_simd_level st_get_simd_level(void);

/**
 * Get name of CPU simd level
 *
 * @param level
 *     The simd level
 * @return
 *     simd level name
 */
const char* st_get_simd_level_name(enum st_simd_level level);

/**
 * Create one tx st2110-20(video) session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-20(video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-20(video) session.
 */
st20_tx_handle st20_tx_create(st_handle st, struct st20_tx_ops* ops);

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
 * Get the mbuf pointer and usrptr of the mbuf from the tx st2110-20(video) session.
 * For ST20_TYPE_RTP_LEVEL.
 * Must call st20_tx_put_mbuf to return the mbuf after rtp pack done.
 *
 * @param handle
 *   The handle to the tx st2110-20(video) session.
 * @param usrptr
 *   *usrptr will be point to the user data(rtp) area inside the mbuf.
 * @return
 *   - NULL if no avaiable mbuf in the ring.
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
 * @param bps
 *   A pointer to the return bit rate.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if fail.
 */
int st20_get_bandwidth_bps(int width, int height, enum st20_fmt fmt, enum st_fps fps,
                           uint64_t* bps);

/**
 * Inline function returning bandwidth(mega per second) for 1080 p59 yuv422 10bit
 * @return
 *     Bandwidth(mega per second)
 */
static inline uint64_t st20_1080p59_yuv422_10bit_bandwidth_mps(void) {
  uint64_t bps;
  st20_get_bandwidth_bps(1920, 1080, ST20_FMT_YUV_422_10BIT, ST_FPS_P59_94, &bps);
  return bps / 1000 / 1000;
}

/**
 * Create one tx st2110-22(compressed video) session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 * st2110-22(compressed video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-22(compressed video) session.
 */
st22_tx_handle st22_tx_create(st_handle st, struct st22_tx_ops* ops);

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
 *   - NULL if no avaiable mbuf in the ring.
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
 * Create one tx st2110-30(audio) session.
 *
 * @param st
 *   The handle to the media streaming device context.
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
 * Retrieve the sample data size from from st2110-30(audio) format.
 *
 * @param fmt
 *   The st2110-30(audio) format.
 * @param c
 *   The st2110-30(audio) channel.
 * @param s
 *   The st2110-30(audio) sampling.
 * @return
 *   - >0 the sample data size.
 *   - <0: Error code if fail.
 */
int st30_get_sample_size(enum st30_fmt fmt, uint16_t c, enum st30_sampling s);

/**
 * Create one tx st2110-40(ancillary) session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a tx st2110-40(ancillary)
 * session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx st2110-40(ancillary) session.
 */
st40_tx_handle st40_tx_create(st_handle st, struct st40_tx_ops* ops);

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
 *   - NULL if no avaiable mbuf in the ring.
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
 * Create one rx st2110-20(video) session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx st2110-20(video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-20(video) session.
 */
st20_rx_handle st20_rx_create(st_handle st, struct st20_rx_ops* ops);

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
 *   The handle to the rx st2110-20(compressed video) session.
 * @param max_dump_packets
 *   The max number of packets to be dumpped.
 * @return
 *   - 0: Success, rx st2110-20(video) session pcapng dump succ.
 *   - <0: Error code of the rx st2110-20(video) session pcapng dump.
 */
int st20_rx_pcapng_dump(st20_rx_handle handle, uint32_t max_dump_packets);

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
 * Put back the received buff get from notify_frame_ready.
 * For ST20_TYPE_FRAME_LEVEL.
 *
 * @param handle
 *   The handle to the rx st2110-20(video) session.
 * @param frame
 *   The framebuffer pointer.
 * @return
 *   - 0: Success, rx st2110-20(video) session freed.
 *   - <0: Error code of the rx st2110-20(video) session free.
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
 *   - NULL if no avaiable mbuf in the ring.
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
 * Create one rx st2110-22(compressed video) session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-22(compressed video) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-22(compressed video) session.
 */
st22_rx_handle st22_rx_create(st_handle st, struct st22_rx_ops* ops);

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
 *   - NULL if no avaiable mbuf in the ring.
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
 * Create one rx st2110-30(audio) session.
 *
 * @param st
 *   The handle to the media streaming device context.
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
 *   - 0: Success, rx st2110-30(audio) session freed.
 *   - <0: Error code of the rx st2110-30(audio) session free.
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
 * Create one rx st2110-40(ancillary) session.
 *
 * @param st
 *   The handle to the media streaming device context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 * st2110-40(ancillary) session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx st2110-40(ancillary) session.
 */
st40_rx_handle st40_rx_create(st_handle st, struct st40_rx_ops* ops);

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
 *   - NULL if no avaiable mbuf in the ring.
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

/**
 * Helper function returning accurate frame rate from enum st_fps
 * @param fps
 *   enum st_fps fps.
 * @return
 *   frame rate number
 */
double st_frame_rate(enum st_fps fps);

#if defined(__cplusplus)
}
#endif

#endif
