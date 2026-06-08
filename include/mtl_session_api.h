/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mtl_session_api.h
 *
 * Unified Polymorphic Session API for Media Transport Library (MTL).
 *
 * DESIGN GOAL - REDUCE CODE REPETITION:
 * =====================================
 *
 * Currently, MTL has separate APIs for each media type:
 *   - st20p_tx_get_frame() / st20p_tx_put_frame()
 *   - st22p_tx_get_frame() / st22p_tx_put_frame()
 *   - st30p_tx_get_frame() / st30p_tx_put_frame()
 *   - st40p_tx_get_frame() / st40p_tx_put_frame()
 *
 * This leads to code duplication in both the library and applications.
 *
 * THE POLYMORPHIC SOLUTION:
 * =========================
 *
 *   1. ONE session type: mtl_session_t (wraps st20p/st22p/st30p/st40p internally)
 *
 *   2. CREATION is type-specific (different configs needed):
 *      - mtl_video_session_create(mt, &video_config, &session)
 *      - mtl_audio_session_create(mt, &audio_config, &session)
 *      - mtl_ancillary_session_create(mt, &anc_config, &session)
 *
 *   3. ALL OTHER OPERATIONS ARE IDENTICAL - same function for any media:
 *      - mtl_session_buffer_get(session, &buffer, timeout)
 *      - mtl_session_buffer_put(session, buffer)
 *      - mtl_session_event_poll(session, &event, timeout)
 *      - mtl_session_start/stop/destroy(session)
 *
 * BENEFITS:
 *   - Applications can write generic media handling code
 *   - Library can share implementation across media types
 *   - Simpler API to learn - same pattern everywhere
 *   - Easier testing - one test framework for all session types
 *
 * INTERNAL IMPLEMENTATION:
 *   - mtl_session_t contains vtable pointer + wrapped session handle
 *   - vtable dispatches to appropriate st20/st22/st30/st40 functions
 *   - No performance penalty - vtable dispatch is just one indirect call
 */

#ifndef _MTL_SESSION_API_HEAD_H_
#define _MTL_SESSION_API_HEAD_H_

#include "mtl_api.h"
#include "st_api.h"
#include "st20_api.h"
#include "st30_api.h"
#include "st_pipeline_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*************************************************************************
 * Core Types
 *************************************************************************/

/** Session direction */
typedef enum {
  MTL_SESSION_TX = 0, /**< Transmitter */
  MTL_SESSION_RX = 1, /**< Receiver */
} mtl_session_dir_t;

/** Media type (for querying session type) */
typedef enum {
  MTL_TYPE_VIDEO = 0,     /**< Video (ST20/ST22) */
  MTL_TYPE_AUDIO = 1,     /**< Audio (ST30) */
  MTL_TYPE_ANCILLARY = 2, /**< Ancillary (ST40) */
  MTL_TYPE_FASTMETA = 3,  /**< Fast Metadata (ST41) */
} mtl_media_type_t;

/** Buffer ownership model */
typedef enum {
  MTL_BUFFER_LIBRARY_OWNED = 0, /**< Library manages buffers (default) */
  MTL_BUFFER_USER_OWNED = 1,    /**< App provides buffers (zero-copy) */
} mtl_buffer_ownership_t;

/** Video processing mode */
typedef enum {
  MTL_VIDEO_MODE_FRAME = 0, /**< Frame-level: full frames only */
  MTL_VIDEO_MODE_SLICE = 1, /**< Slice-level: line-by-line for ultra-low latency */
} mtl_video_mode_t;

/** Event types for polling */
typedef enum {
  MTL_EVENT_NONE = 0,
  MTL_EVENT_BUFFER_READY = 1,    /**< Buffer ready (RX: has data, TX: available) */
  MTL_EVENT_BUFFER_DONE = 2,     /**< Buffer processing complete */
  MTL_EVENT_ERROR = 3,           /**< Error occurred */
  MTL_EVENT_VSYNC = 4,           /**< Vertical sync (epoch boundary) */
  MTL_EVENT_FRAME_LATE = 5,      /**< TX: frame missed its epoch */
  MTL_EVENT_FORMAT_DETECTED = 6, /**< RX: video format auto-detected */
  MTL_EVENT_TIMING_REPORT = 7,   /**< RX: timing parser result (periodic) */
  MTL_EVENT_SLICE_READY = 8,     /**< Slice mode: lines ready (RX) or need more (TX) */
} mtl_event_type_t;

/** Frame/buffer status */
typedef enum {
  MTL_FRAME_STATUS_COMPLETE = 0,   /**< Complete frame received */
  MTL_FRAME_STATUS_INCOMPLETE = 1, /**< Missing packets */
  MTL_FRAME_STATUS_CORRUPTED = 2,  /**< Detected corruption */
} mtl_frame_status_t;

/** Session flags (used in base config flags field) */
#define MTL_SESSION_FLAG_EXT_BUFFER (1 << 0)      /**< Use external/user-owned buffers */
#define MTL_SESSION_FLAG_USER_PACING (1 << 1)      /**< User controls TX pacing */
#define MTL_SESSION_FLAG_USER_TIMESTAMP (1 << 2)   /**< User provides timestamps */
#define MTL_SESSION_FLAG_ENABLE_VSYNC (1 << 3)     /**< Enable VSYNC events */
#define MTL_SESSION_FLAG_ENABLE_RTCP (1 << 4)      /**< Enable RTCP feedback */
#define MTL_SESSION_FLAG_FORCE_NUMA (1 << 5)       /**< Force NUMA socket affinity */
#define MTL_SESSION_FLAG_DATA_PATH_ONLY (1 << 6)   /**< App manages flow rules */
#define MTL_SESSION_FLAG_RECEIVE_INCOMPLETE_FRAME (1 << 7) /**< Accept incomplete frames */
#define MTL_SESSION_FLAG_DMA_OFFLOAD (1 << 8)      /**< DMA copy offload */
#define MTL_SESSION_FLAG_HDR_SPLIT (1 << 9)        /**< Header split mode */
#define MTL_SESSION_FLAG_BLOCK_GET (1 << 10)       /**< Blocking buffer get mode */
#define MTL_SESSION_FLAG_USER_P_MAC (1 << 11)      /**< TX: User-provided primary MAC */
#define MTL_SESSION_FLAG_USER_R_MAC (1 << 12)      /**< TX: User-provided redundant MAC */
#define MTL_SESSION_FLAG_EXACT_USER_PACING (1 << 13) /**< TX: Exact user pacing */
#define MTL_SESSION_FLAG_RTP_TIMESTAMP_EPOCH (1 << 14) /**< TX: RTP timestamp epoch mode */
#define MTL_SESSION_FLAG_DISABLE_BULK (1 << 15)    /**< TX: Disable bulk enqueue */
#define MTL_SESSION_FLAG_STATIC_PAD_P (1 << 16)    /**< TX: Static padding for primary */
#define MTL_SESSION_FLAG_USE_MULTI_THREADS (1 << 17) /**< RX: Multi-thread processing */

/*************************************************************************
 * Opaque Handles
 *************************************************************************/

/**
 * Unified session handle - THE POLYMORPHIC TYPE
 * Works for video, audio, ancillary, metadata - all the same handle type!
 */
typedef struct mtl_session mtl_session_t;

/** DMA memory handle for user-owned buffers */
typedef struct mtl_dma_mem mtl_dma_mem_t;

/*************************************************************************
 * Buffer Structure - UNIFIED FOR ALL MEDIA TYPES
 *************************************************************************/

/**
 * Unified buffer returned by mtl_session_buffer_get().
 * Same structure for video, audio, ancillary - polymorphic design.
 *
 * For basic usage, just use: data, size, timestamp
 * For type-specific fields, use the union after checking media type.
 */
typedef struct mtl_buffer {
  /* Common fields - sufficient for most use cases */
  void* data;                /**< Buffer data pointer */
  mtl_iova_t iova;           /**< DMA address (if applicable) */
  size_t size;               /**< Total buffer size */
  size_t data_size;          /**< Valid data size (may be < size) */
  uint64_t timestamp;        /**< Presentation timestamp (TAI ns) */
  uint64_t epoch;            /**< Epoch info for the frame */
  uint32_t rtp_timestamp;    /**< RTP timestamp */
  uint32_t flags;            /**< Buffer flags */
  mtl_frame_status_t status; /**< Frame completeness status */
  void* priv;                /**< Library private - DO NOT TOUCH */
  void* user_data;           /**< Application context (opaque from ext_frame) */

  /** User metadata (TX: set before put, RX: read after get) */
  void* user_meta;           /**< User metadata pointer */
  size_t user_meta_size;     /**< User metadata size in bytes */
  enum st10_timestamp_fmt tfmt; /**< Timestamp format (TAI, MEDIA_CLK, etc.) */

  /* Type-specific extended fields (optional to use) */
  union {
    struct {
      void* planes[ST_MAX_PLANES]; /**< Plane pointers for planar formats */
      size_t linesize[ST_MAX_PLANES]; /**< Linesize (stride) per plane */
      uint32_t width;
      uint32_t height;
      enum st_frame_fmt fmt; /**< Frame format (st_frame_fmt) */
      bool interlaced;       /**< Interlaced mode */
      bool second_field;     /**< Second field for interlaced */
      uint32_t pkts_total;   /**< Total packets expected */
      uint32_t pkts_recv[2]; /**< Packets received per port */
    } video;

    struct {
      uint32_t samples;           /**< Number of samples */
      uint16_t channels;
      enum st30_fmt fmt;          /**< Audio format */
      enum st30_sampling sampling;
      enum st30_ptime ptime;
      size_t frame_recv_size;     /**< Actual received size */
    } audio;

    struct {
      uint16_t line_number;
      uint8_t did, sdid;
      uint32_t meta_num;   /**< Number of ANC packets in frame */
      bool second_field;   /**< For interlaced */
    } ancillary;

    uint8_t _reserved[96];
  };
} mtl_buffer_t;

/** Buffer flags */
#define MTL_BUF_FLAG_EXT (1 << 0)        /**< External (user-owned) buffer */
#define MTL_BUF_FLAG_INCOMPLETE (1 << 1)  /**< Incomplete frame (RX) */

/*************************************************************************
 * Event Structure
 *************************************************************************/

/** Event from mtl_session_event_poll() */
typedef struct mtl_event {
  mtl_event_type_t type;
  int status;       /**< Error code if type == MTL_EVENT_ERROR */
  uint64_t timestamp; /**< Event timestamp (TAI ns) */
  void* ctx;        /**< Buffer ptr (lib-owned) or user ctx (user-owned) */

  /* Event-specific data */
  union {
    /** For MTL_EVENT_VSYNC */
    struct {
      uint64_t epoch;    /**< Current epoch */
      uint64_t ptp_time; /**< PTP time at vsync */
    } vsync;

    /** For MTL_EVENT_FRAME_LATE */
    struct {
      uint64_t epoch_skipped; /**< The epoch that was missed */
    } frame_late;

    /** For MTL_EVENT_FORMAT_DETECTED (video auto-detect) */
    struct {
      uint32_t width;
      uint32_t height;
      enum st_fps fps;
      enum st20_packing packing;
      bool interlaced;
    } format_detected;

    /** For MTL_EVENT_TIMING_REPORT */
    struct {
      enum st_rx_tp_compliant compliant;
      int32_t vrx_max, vrx_min;
      int32_t ipt_max, ipt_min; /**< Inter-packet time (ns) */
      int32_t latency;          /**< ns */
      uint32_t pkts_cnt;
    } timing;

    /** For MTL_EVENT_SLICE_READY (slice mode) */
    struct {
      uint16_t lines_ready; /**< RX: lines received so far */
      uint16_t lines_total; /**< Total lines in frame */
      void* buffer;         /**< Current frame buffer */
    } slice;

    /** For MTL_EVENT_ERROR */
    struct {
      int code; /**< Error code */
    } error;

    /** For MTL_EVENT_BUFFER_READY / MTL_EVENT_BUFFER_DONE */
    struct {
      struct mtl_buffer* buf; /**< Related buffer */
    } buffer;

    uint8_t _reserved[64];
  };
} mtl_event_t;

/*************************************************************************
 * Configuration Structures
 *************************************************************************/

/**
 * Base configuration - embedded in all type-specific configs.
 * Contains fields common to all media types.
 */
typedef struct mtl_session_base_config {
  mtl_session_dir_t direction;      /**< TX or RX */
  mtl_buffer_ownership_t ownership; /**< Who owns buffers */
  uint16_t num_buffers;             /**< Buffer count */
  const char* name;                 /**< Session name (debug) */
  void* priv;                       /**< App context for callbacks */
  uint32_t flags;                   /**< Session flags (MTL_SESSION_FLAG_*) */

  /** NUMA socket to use (-1 for auto based on NIC) */
  int socket_id;

  /**
   * Optional callbacks (alternative to polling).
   * NOTE: callbacks run from library thread, must be non-blocking!
   */
  int (*notify_buffer_ready)(void* priv);
  int (*notify_event)(void* priv, mtl_event_t* event);

  /**
   * For user-owned/ext_frame mode: query callback to get external frame.
   * Library calls this when it needs a buffer.
   * Return 0 and fill ext_frame on success.
   */
  int (*query_ext_frame)(void* priv, struct st_ext_frame* ext_frame,
                         struct mtl_buffer* frame_meta);
} mtl_session_base_config_t;

/**
 * Video session configuration.
 * For ST20 (uncompressed) and ST22 (compressed) video.
 */
typedef struct mtl_video_config {
  mtl_session_base_config_t base;

  /* Network - use existing MTL port structures */
  union {
    struct st_tx_port tx_port; /**< For TX sessions */
    struct st_rx_port rx_port; /**< For RX sessions */
  };

  /* Video format */
  uint32_t width;
  uint32_t height;
  enum st_fps fps;
  bool interlaced;
  enum st_frame_fmt frame_fmt;  /**< App pixel format */
  enum st20_fmt transport_fmt;  /**< Wire format */

  /* Pacing/packing (TX) */
  enum st21_pacing pacing;
  enum st20_packing packing;
  uint32_t linesize; /**< Line stride, 0 = no padding */

  /*************************************************************************
   * Slice Mode (ultra-low latency)
   *************************************************************************/

  /**
   * Video processing mode: FRAME (default) or SLICE.
   *
   * SLICE mode enables line-by-line processing for ultra-low latency:
   * - TX: Use mtl_session_slice_ready() to signal lines are ready
   * - RX: Get MTL_EVENT_SLICE_READY events as lines arrive
   *
   * Note: Always enable RECEIVE_INCOMPLETE_FRAME flag with slice mode.
   */
  mtl_video_mode_t mode;

  /**
   * Slice mode TX only: callback when lib needs to know ready lines.
   * Return the number of lines ready for transmission.
   * Non-blocking, called from library thread.
   */
  int (*query_lines_ready)(void* priv, uint16_t frame_idx, uint16_t* lines_ready);

  /*************************************************************************
   * ST22 Compression / Plugins
   *************************************************************************/

  /** Enable ST22 compressed video (requires codec plugin) */
  bool compressed;

  /** ST22 codec type (JPEGXS, H264, H265, etc.) */
  enum st22_codec codec;

  /** Target codestream size for ST22 (CBR mode) */
  size_t codestream_size;

  /**
   * Plugin device preference (CPU, GPU, FPGA, AUTO).
   * Library selects appropriate registered plugin.
   */
  enum st_plugin_device plugin_device;

  /** Encode quality vs speed tradeoff */
  enum st22_quality_mode quality;

  /** Number of codec threads (0 = auto) */
  uint32_t codec_thread_cnt;

  /*************************************************************************
   * Advanced Options
   *************************************************************************/

  /**
   * TX only: User-provided destination MAC addresses.
   * Used when MTL_SESSION_FLAG_USER_P_MAC / MTL_SESSION_FLAG_USER_R_MAC is set.
   */
  uint8_t tx_dst_mac[MTL_SESSION_PORT_MAX][MTL_MAC_ADDR_LEN];

  /** TX only: start VRX value for pacing (0 = library default) */
  uint32_t start_vrx;

  /** TX only: pad interval for pacing (0 = library default) */
  uint32_t pad_interval;

  /** TX only: RTP timestamp delta in microseconds (0 = auto) */
  int32_t rtp_timestamp_delta_us;

  /** RX only: burst size for packet receive (0 = default) */
  uint32_t rx_burst_size;

  /**
   * RX only: Enable timing parser analysis.
   * Results delivered via MTL_EVENT_TIMING_REPORT events.
   */
  bool enable_timing_parser;

  /**
   * RX only: Enable auto-detect of video format.
   * If enabled, width/height/fps can be left as 0.
   * Format detection delivered via MTL_EVENT_FORMAT_DETECTED.
   */
  bool enable_auto_detect;

} mtl_video_config_t;

/**
 * Audio session configuration (ST30).
 */
typedef struct mtl_audio_config {
  mtl_session_base_config_t base;

  union {
    struct st_tx_port tx_port;
    struct st_rx_port rx_port;
  };

  enum st30_fmt fmt;
  uint16_t channels;
  enum st30_sampling sampling;
  enum st30_ptime ptime;
  uint32_t framebuff_size;

  /** TX pacing method */
  enum st30_tx_pacing_way pacing_way;

  /** Enable timing parser (RX) - results via MTL_EVENT_TIMING_REPORT */
  bool enable_timing_parser;

} mtl_audio_config_t;

/**
 * Ancillary session configuration (ST40).
 */
typedef struct mtl_ancillary_config {
  mtl_session_base_config_t base;

  union {
    struct st_tx_port tx_port;
    struct st_rx_port rx_port;
  };

  enum st_fps fps;
  bool interlaced;
  uint32_t framebuff_size;

} mtl_ancillary_config_t;

/*************************************************************************
 * Session Creation - TYPE-SPECIFIC (only part that differs)
 *************************************************************************/

/**
 * Create a video session (ST20 or ST22).
 * After creation, use generic mtl_session_* functions.
 */
int mtl_video_session_create(mtl_handle mt, const mtl_video_config_t* config,
                             mtl_session_t** session);

/**
 * Create an audio session (ST30).
 */
int mtl_audio_session_create(mtl_handle mt, const mtl_audio_config_t* config,
                             mtl_session_t** session);

/**
 * Create an ancillary session (ST40).
 */
int mtl_ancillary_session_create(mtl_handle mt, const mtl_ancillary_config_t* config,
                                 mtl_session_t** session);

/*************************************************************************
 * Session Operations - POLYMORPHIC (same for ALL media types!)
 *
 * This is the key API: same functions work for video, audio, ancillary.
 * No need for separate st20p_tx_get_frame, st30p_tx_get_frame, etc.
 *************************************************************************/

/**
 * Start session processing.
 * @return 0 on success, negative errno on error.
 */
int mtl_session_start(mtl_session_t* session);

/**
 * Stop session.
 *
 * After this call:
 * - Session enters "stopped" state
 * - mtl_session_buffer_get() returns -EAGAIN immediately (no blocking)
 * - mtl_session_event_poll() returns -EAGAIN immediately (no blocking)
 * - Application threads can detect -EAGAIN, check their stop flag, exit cleanly
 *
 * Can be restarted with mtl_session_start() (clears stopped state).
 * Thread-safe: can be called from any thread (signal handler, main thread, etc.)
 *
 * Typical shutdown sequence:
 *   app->stop = true;                  // Your app flag
 *   mtl_session_stop(session);         // Make buffer_get() return -EAGAIN
 *   pthread_join(app->worker, NULL);   // Wait for worker to exit
 *   mtl_session_destroy(session);      // Now safe to destroy
 *
 * @param session Session handle
 * @return 0 on success, negative errno on error.
 */
int mtl_session_stop(mtl_session_t* session);

/**
 * Check if session is stopped.
 *
 * @param session Session handle
 * @return true if stop() was called (and start() not called after), false otherwise
 */
bool mtl_session_is_stopped(mtl_session_t* session);

/**
 * Destroy session and free all resources.
 *
 * PRECONDITION: All application threads must have stopped using this session.
 * Call mtl_session_stop() and join your threads first.
 *
 * @param session Session handle (invalid after return)
 * @return 0 on success, negative errno on error
 */
int mtl_session_destroy(mtl_session_t* session);

/**
 * Get media type of session.
 */
mtl_media_type_t mtl_session_get_type(mtl_session_t* session);

/*************************************************************************
 * Buffer Operations - POLYMORPHIC (same for ALL media types!)
 *************************************************************************/

/**
 * Get buffer from session (library-owned mode).
 *
 * For TX: returns empty buffer to fill with data
 * For RX: returns buffer containing received data
 *
 * Works the same whether session is video, audio, or ancillary!
 *
 * @param session Any session type (video/audio/ancillary)
 * @param buffer Output buffer pointer
 * @param timeout_ms Timeout (0 = non-blocking)
 * @return 0 success, -ETIMEDOUT, -EAGAIN if stopped, or other negative errno
 */
int mtl_session_buffer_get(mtl_session_t* session, mtl_buffer_t** buffer,
                           uint32_t timeout_ms);

/**
 * Return buffer to session (library-owned mode).
 *
 * For TX: submits filled buffer for transmission
 * For RX: returns processed buffer for reuse
 *
 * @param session Any session type
 * @param buffer Buffer from mtl_session_buffer_get()
 */
int mtl_session_buffer_put(mtl_session_t* session, mtl_buffer_t* buffer);

/**
 * Post user-owned buffer (zero-copy mode).
 *
 * For TX: submits user buffer for transmission
 * For RX: provides user buffer to receive into
 *
 * Buffer must be from registered memory region.
 * Completion via mtl_session_event_poll().
 *
 * @param session Any session type
 * @param data Buffer data pointer (from registered region)
 * @param size Buffer size
 * @param user_ctx Returned in completion event
 */
int mtl_session_buffer_post(mtl_session_t* session, void* data, size_t size,
                            void* user_ctx);

/**
 * Flush pending buffers.
 */
int mtl_session_buffer_flush(mtl_session_t* session, uint32_t timeout_ms);

/*************************************************************************
 * Memory Registration (for user-owned/zero-copy mode)
 *************************************************************************/

/**
 * Register memory region for DMA.
 * Required before posting buffers from this region.
 */
int mtl_session_mem_register(mtl_session_t* session, void* addr, size_t size,
                             mtl_dma_mem_t** handle);

/**
 * Unregister memory region.
 */
int mtl_session_mem_unregister(mtl_session_t* session, mtl_dma_mem_t* handle);

/*************************************************************************
 * Event Polling - POLYMORPHIC
 *************************************************************************/

/**
 * Poll for events.
 *
 * @param session Any session type
 * @param event Output event
 * @param timeout_ms Timeout (0 = non-blocking)
 * @return 0 if event available, -ETIMEDOUT, -EAGAIN if stopped, or error
 */
int mtl_session_event_poll(mtl_session_t* session, mtl_event_t* event,
                           uint32_t timeout_ms);

/*************************************************************************
 * Statistics - POLYMORPHIC
 *************************************************************************/

typedef struct mtl_session_stats {
  uint64_t buffers_processed;
  uint64_t bytes_processed;
  uint64_t buffers_dropped;
  uint32_t buffers_free;
  uint32_t buffers_in_use;
  /* TX specific */
  uint64_t epochs_missed; /**< Frames that missed their epoch */
  /* RX specific */
  uint64_t pkts_received;
  uint64_t pkts_redundant; /**< Redundant path packets */
} mtl_session_stats_t;

int mtl_session_stats_get(mtl_session_t* session, mtl_session_stats_t* stats);
int mtl_session_stats_reset(mtl_session_t* session);

/**
 * Get the frame (buffer) size for the session in bytes.
 * For TX, this is the transport frame size. For RX with conversion,
 * this is the converted output frame size.
 *
 * @param session Session handle
 * @return Frame size in bytes, or 0 on error
 */
size_t mtl_session_get_frame_size(mtl_session_t* session);

/**
 * Get detailed per-port IO statistics for the session.
 * Wraps the underlying st20_tx/rx_get_session_stats().
 * The stats struct is type-specific (pass appropriate struct).
 *
 * @param session Session handle
 * @param stats Output buffer (cast to st20_tx_user_stats or st20_rx_user_stats)
 * @param stats_size Size of the stats buffer (for validation)
 * @return 0 on success, negative errno on error
 */
int mtl_session_io_stats_get(mtl_session_t* session, void* stats, size_t stats_size);

/**
 * Reset per-port IO statistics.
 * @return 0 on success, negative errno on error
 */
int mtl_session_io_stats_reset(mtl_session_t* session);

/**
 * Trigger pcap dump for an RX session (debug tool).
 *
 * @param session RX session handle
 * @param max_dump_packets Maximum number of packets to dump
 * @param sync If true, block until dump is complete
 * @param meta Optional output metadata (file paths, dumped counts). NULL if not needed.
 * @return 0 on success, negative errno on error
 */
int mtl_session_pcap_dump(mtl_session_t* session, uint32_t max_dump_packets,
                          bool sync, struct st_pcap_dump_meta* meta);

/*************************************************************************
 * Online Session Updates
 *************************************************************************/

/**
 * Update TX session destination (for stream switching).
 * Allows changing destination IP/port without recreating session.
 */
int mtl_session_update_destination(mtl_session_t* session,
                                   const struct st_tx_dest_info* dst);

/**
 * Update RX session source (for stream switching).
 * Allows changing source filter without recreating session.
 */
int mtl_session_update_source(mtl_session_t* session,
                              const struct st_rx_source_info* src);

/*************************************************************************
 * Slice-Level API (ultra-low latency video)
 *
 * These functions are only valid for video sessions with
 * mode = MTL_VIDEO_MODE_SLICE.
 *************************************************************************/

/**
 * TX Slice Mode: Notify library that lines are ready for transmission.
 *
 * In slice mode, application fills frame buffer line-by-line and
 * calls this function to signal progress. Library transmits lines
 * as they become available, achieving sub-frame latency.
 *
 * @param session Video TX session with mode=MTL_VIDEO_MODE_SLICE
 * @param buffer Current frame buffer (from buffer_get)
 * @param lines_ready Number of lines now ready (cumulative from top)
 * @return 0 on success, -EINVAL if not slice mode
 */
int mtl_session_slice_ready(mtl_session_t* session, mtl_buffer_t* buffer,
                            uint16_t lines_ready);

/**
 * RX Slice Mode: Query how many lines have been received.
 *
 * Alternative to event-driven: application can poll for line progress.
 * Useful when processing each line as it arrives.
 *
 * @param session Video RX session with mode=MTL_VIDEO_MODE_SLICE
 * @param buffer Current frame buffer
 * @param[out] lines_ready Number of lines received so far
 * @return 0 on success, -EINVAL if not slice mode
 */
int mtl_session_slice_query(mtl_session_t* session, mtl_buffer_t* buffer,
                            uint16_t* lines_ready);

/*************************************************************************
 * Plugin Information
 *************************************************************************/

/**
 * Plugin capability info (returned by mtl_session_get_plugin_info)
 */
typedef struct mtl_plugin_info {
  char name[64];                /**< Plugin name */
  char version[32];             /**< Plugin version string */
  enum st_plugin_device device; /**< CPU, GPU, FPGA */
  enum st22_codec codec;        /**< Codec type (for ST22) */
  bool supports_interlaced;     /**< Can handle interlaced */
  uint32_t max_threads;         /**< Max codec threads */
} mtl_plugin_info_t;

/**
 * Get info about the plugin used by this session.
 *
 * Only valid for ST22 compressed video sessions.
 *
 * @param session Video session using codec plugin
 * @param[out] info Plugin information
 * @return 0 on success, -ENOENT if no plugin, -EINVAL if not applicable
 */
int mtl_session_get_plugin_info(mtl_session_t* session, mtl_plugin_info_t* info);

/*************************************************************************
 * Queue Meta (for DATA_PATH_ONLY mode)
 *************************************************************************/

/**
 * Get queue metadata for DATA_PATH_ONLY mode.
 * Application manages flow rules when this mode is enabled.
 */
int mtl_session_get_queue_meta(mtl_session_t* session, struct st_queue_meta* meta);

/*************************************************************************
 * Event FD for epoll/select integration
 *************************************************************************/

/**
 * Get file descriptor for event notification.
 * Can be used with epoll/select to wait for events.
 * @return fd >= 0 on success, negative errno on error
 */
int mtl_session_get_event_fd(mtl_session_t* session);

/*************************************************************************
 * Blocking behavior configuration
 *************************************************************************/

/**
 * Set timeout for blocking buffer_get operations.
 * Only applies when BLOCK_GET flag is set.
 * @param timeout_us Timeout in microseconds
 */
int mtl_session_set_block_timeout(mtl_session_t* session, uint64_t timeout_us);

#if defined(__cplusplus)
}
#endif

#endif /* _MTL_SESSION_API_HEAD_H_ */
