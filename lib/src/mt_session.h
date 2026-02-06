/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_session.h
 *
 * Internal definitions for unified session API implementation.
 * NOT part of public API - for library implementation only.
 *
 * Architecture Note:
 * -----------------
 * This new unified API wraps the low-level session structures:
 *   - st_tx_video_session_impl / st_rx_video_session_impl (from st_header.h)
 *   - st_frame_trans (the actual frame buffer structure)
 *
 * The pipeline layer (st20p_*, st_frame, etc.) is kept for backward compatibility
 * but new code should use the unified session API.
 */

#ifndef _MT_LIB_SESSION_H_
#define _MT_LIB_SESSION_H_

#include "mtl_session_api.h"

/* Internal MTL headers */
#include "mt_main.h"
#include "st2110/st_header.h"

#include <rte_atomic.h>
#include <rte_ring.h>
#include <rte_spinlock.h>

#if defined(__cplusplus)
extern "C" {
#endif

/*************************************************************************
 * Forward Declarations
 *************************************************************************/

struct mtl_session_impl;
struct mtl_buffer_impl;

/*************************************************************************
 * VTable - Polymorphic Dispatch
 *
 * Each media type (video/audio/ancillary) implements these functions.
 * The unified API dispatches through this table.
 *************************************************************************/

typedef struct mtl_session_vtable {
  /* Lifecycle */
  int (*start)(struct mtl_session_impl* s);
  int (*stop)(struct mtl_session_impl* s);
  void (*destroy)(struct mtl_session_impl* s);

  /* Buffer operations */
  int (*buffer_get)(struct mtl_session_impl* s, mtl_buffer_t** buf, uint32_t timeout_ms);
  int (*buffer_put)(struct mtl_session_impl* s, mtl_buffer_t* buf);
  int (*buffer_post)(struct mtl_session_impl* s, void* data, size_t size, void* ctx);
  int (*buffer_flush)(struct mtl_session_impl* s, uint32_t timeout_ms);

  /* Memory registration */
  int (*mem_register)(struct mtl_session_impl* s, void* addr, size_t size,
                      mtl_dma_mem_t** handle);
  int (*mem_unregister)(struct mtl_session_impl* s, mtl_dma_mem_t* handle);

  /* Events */
  int (*event_poll)(struct mtl_session_impl* s, mtl_event_t* event, uint32_t timeout_ms);
  int (*get_event_fd)(struct mtl_session_impl* s);

  /* Stats */
  int (*stats_get)(struct mtl_session_impl* s, mtl_session_stats_t* stats);
  int (*stats_reset)(struct mtl_session_impl* s);

  /* Frame size query */
  size_t (*get_frame_size)(struct mtl_session_impl* s);

  /* IO stats (per-port detailed stats) */
  int (*io_stats_get)(struct mtl_session_impl* s, void* stats, size_t stats_size);
  int (*io_stats_reset)(struct mtl_session_impl* s);

  /* Pcap dump (RX only) */
  int (*pcap_dump)(struct mtl_session_impl* s, uint32_t max_pkts, bool sync,
                   struct st_pcap_dump_meta* meta);

  /* Online updates */
  int (*update_destination)(struct mtl_session_impl* s,
                            const struct st_tx_dest_info* dst);
  int (*update_source)(struct mtl_session_impl* s, const struct st_rx_source_info* src);

  /* Slice mode (video only, NULL for audio/ancillary) */
  int (*slice_ready)(struct mtl_session_impl* s, mtl_buffer_t* buf, uint16_t lines);
  int (*slice_query)(struct mtl_session_impl* s, mtl_buffer_t* buf, uint16_t* lines);

  /* Plugin info (ST22 only, NULL otherwise) */
  int (*get_plugin_info)(struct mtl_session_impl* s, mtl_plugin_info_t* info);

  /* Queue meta (for DATA_PATH_ONLY) */
  int (*get_queue_meta)(struct mtl_session_impl* s, struct st_queue_meta* meta);

} mtl_session_vtable_t;

/*************************************************************************
 * Session State
 *************************************************************************/

typedef enum mtl_session_state {
  MTL_SESSION_STATE_CREATED = 0,
  MTL_SESSION_STATE_STARTED,
  MTL_SESSION_STATE_STOPPED, /**< stop() called - buffer_get returns -EAGAIN */
  MTL_SESSION_STATE_ERROR,
} mtl_session_state_t;

/*************************************************************************
 * Internal Buffer Implementation
 *
 * Wraps st_frame_trans (the actual frame buffer from st_header.h)
 *************************************************************************/

struct mtl_buffer_impl {
  /* Public view (returned to user) */
  mtl_buffer_t pub;

  /* Internal linkage */
  struct mtl_session_impl* session;
  uint32_t idx; /**< Buffer index in pool */

  /*
   * The ACTUAL frame buffer from low-level session.
   * st_frame_trans contains: addr, iova, refcnt, flags, metadata, etc.
   * This is NOT st_frame from pipeline - that's the old API.
   */
  struct st_frame_trans* frame_trans;

  /* For user-owned mode */
  void* user_ctx;  /**< User context for completion */
  bool user_owned; /**< true if app-owned buffer */
};

/*************************************************************************
 * Internal Session Implementation
 *
 * Contains pointer to ACTUAL low-level session impl from st_header.h.
 *************************************************************************/

struct mtl_session_impl {
  /* VTable for polymorphic dispatch - MUST BE FIRST */
  const mtl_session_vtable_t* vt;

  /* Type identification */
  uint32_t magic;              /**< Magic number for validation */
  mtl_media_type_t type;       /**< VIDEO, AUDIO, ANCILLARY */
  mtl_session_dir_t direction; /**< TX or RX */

  /* Parent context */
  struct mtl_main_impl* parent; /**< MTL instance (internal type) */
  int idx;                      /**< Session index (for logging) */
  int socket_id;                /**< NUMA socket */

  /* State */
  mtl_session_state_t state;
  rte_spinlock_t state_lock;

  /* Set by stop(), checked by buffer_get/event_poll to return -EAGAIN */
  volatile bool stopped;

  /* Configuration (copied from create) */
  char name[ST_MAX_NAME_LEN];
  uint32_t flags;
  mtl_buffer_ownership_t ownership;

  /*
   * Pointer to the ACTUAL low-level session implementation.
   * These are the real session structs from st_header.h that contain
   * all the frame management, pacing, stats, etc.
   */
  union {
    /* Video - direct to low-level impl */
    struct st_tx_video_session_impl* video_tx;
    struct st_rx_video_session_impl* video_rx;

    /* Audio - direct to low-level impl */
    struct st_tx_audio_session_impl* audio_tx;
    struct st_rx_audio_session_impl* audio_rx;

    /* Ancillary - direct to low-level impl */
    struct st_tx_ancillary_session_impl* anc_tx;
    struct st_rx_ancillary_session_impl* anc_rx;
  } inner;

  /*
   * Frame buffer management.
   * For library-owned mode, we manage mtl_buffer_impl wrappers.
   * The actual frame memory is in inner->st20_frames (st_frame_trans array).
   */
  uint32_t buffer_count;
  struct mtl_buffer_impl* buffers; /**< Buffer wrapper pool */

  /* Event queue */
  struct rte_ring* event_ring; /**< Pending events */
  int event_fd;                /**< For epoll integration */

  /* Statistics - aggregated view of inner session stats */
  mtl_session_stats_t stats;
  rte_spinlock_t stats_lock;

  /* Callbacks (optional, for low-latency notification) */
  int (*notify_buffer_ready)(void* priv);
  void* notify_priv;

  /*
   * Type-specific cached config.
   * Actual config is in the inner session impl.
   */
  union {
    struct {
      bool compressed;             /**< ST22 mode */
      mtl_video_mode_t mode;       /**< FRAME or SLICE */
      enum st_frame_fmt frame_fmt; /**< App pixel format (may differ from transport) */
      bool derive;                 /**< true if frame_fmt == transport_fmt (no conversion) */
    } video;
    struct {
      uint32_t channels;
    } audio;
  };
};

/*************************************************************************
 * Magic Numbers for Handle Validation
 *************************************************************************/

#define MTL_SESSION_MAGIC_VIDEO_TX 0x4D564458 /* "MVTX" */
#define MTL_SESSION_MAGIC_VIDEO_RX 0x4D565258 /* "MVRX" */
#define MTL_SESSION_MAGIC_AUDIO_TX 0x4D415458 /* "MATX" */
#define MTL_SESSION_MAGIC_AUDIO_RX 0x4D415258 /* "MARX" */
#define MTL_SESSION_MAGIC_ANC_TX 0x4D4E5458   /* "MNTX" */
#define MTL_SESSION_MAGIC_ANC_RX 0x4D4E5258   /* "MNRX" */

/*************************************************************************
 * Internal Helper Macros
 *************************************************************************/

/** Validate session handle */
#define MTL_SESSION_VALID(s)                                                         \
  ((s) && ((s)->magic == MTL_SESSION_MAGIC_VIDEO_TX ||                               \
           (s)->magic == MTL_SESSION_MAGIC_VIDEO_RX ||                               \
           (s)->magic == MTL_SESSION_MAGIC_AUDIO_TX ||                               \
           (s)->magic == MTL_SESSION_MAGIC_AUDIO_RX ||                               \
           (s)->magic == MTL_SESSION_MAGIC_ANC_TX || (s)->magic == MTL_SESSION_MAGIC_ANC_RX))

/** Get implementation from public handle */
#define MTL_SESSION_IMPL(pub) ((struct mtl_session_impl*)(pub))

/** Get public handle from implementation */
#define MTL_SESSION_PUB(impl) ((mtl_session_t*)(impl))

/** Get buffer implementation from public handle */
#define MTL_BUFFER_IMPL(_pub) \
  ((struct mtl_buffer_impl*)((char*)(_pub) - offsetof(struct mtl_buffer_impl, pub)))

/*************************************************************************
 * Internal Functions - Video Session
 *
 * These create/init the actual st_tx/rx_video_session_impl and
 * attach to the session manager, similar to current st20_tx_create().
 *************************************************************************/

/** Create video TX session */
int mtl_video_tx_session_init(struct mtl_session_impl* s, struct mtl_main_impl* impl,
                              const mtl_video_config_t* config);

/** Create video RX session */
int mtl_video_rx_session_init(struct mtl_session_impl* s, struct mtl_main_impl* impl,
                              const mtl_video_config_t* config);

/** Cleanup video TX session */
void mtl_video_tx_session_uinit(struct mtl_session_impl* s);

/** Cleanup video RX session */
void mtl_video_rx_session_uinit(struct mtl_session_impl* s);

/** Video session vtables */
extern const mtl_session_vtable_t mtl_video_tx_vtable;
extern const mtl_session_vtable_t mtl_video_rx_vtable;

/*************************************************************************
 * Internal Functions - Audio Session (stub for future)
 *************************************************************************/

/** Audio session vtables */
extern const mtl_session_vtable_t mtl_audio_tx_vtable;
extern const mtl_session_vtable_t mtl_audio_rx_vtable;

/*************************************************************************
 * Internal Functions - Ancillary Session (stub for future)
 *************************************************************************/

/** Ancillary session vtables */
extern const mtl_session_vtable_t mtl_ancillary_tx_vtable;
extern const mtl_session_vtable_t mtl_ancillary_rx_vtable;

/*************************************************************************
 * Internal Utilities
 *************************************************************************/

/** Allocate session structure */
struct mtl_session_impl* mtl_session_alloc(struct mtl_main_impl* impl, int socket_id);

/** Free session structure */
void mtl_session_free(struct mtl_session_impl* s);

/** Initialize buffer wrapper pool (library-owned mode) */
int mtl_session_buffers_init(struct mtl_session_impl* s, uint32_t count);

/** Cleanup buffer wrapper pool */
void mtl_session_buffers_uinit(struct mtl_session_impl* s);

/** Initialize event ring */
int mtl_session_events_init(struct mtl_session_impl* s);

/** Cleanup event ring */
void mtl_session_events_uinit(struct mtl_session_impl* s);

/** Post event to session */
int mtl_session_event_post(struct mtl_session_impl* s, const mtl_event_t* event);

/**
 * Populate mtl_buffer public fields from st_frame_trans.
 * Called when getting a buffer to fill the user-visible fields.
 */
void mtl_buffer_fill_from_frame_trans(struct mtl_buffer_impl* b,
                                      struct st_frame_trans* ft,
                                      mtl_media_type_t type);

/**
 * Get a free st_frame_trans from the session's frame pool.
 * Uses refcnt == 0 to find free frame, then increments refcnt.
 */
struct st_frame_trans* mtl_session_get_frame_trans(struct mtl_session_impl* s);

/**
 * Release st_frame_trans back to pool (decrement refcnt).
 */
void mtl_session_put_frame_trans(struct st_frame_trans* ft);

/*************************************************************************
 * Stop/Start Helpers
 *
 * stop() sets stopped=true, buffer_get/event_poll return -EAGAIN
 * start() clears stopped, blocking calls work again
 *************************************************************************/

/**
 * Check if session is stopped.
 * Call this at the start of any blocking operation.
 */
static inline bool mtl_session_check_stopped(struct mtl_session_impl* s) {
  return s->stopped;
}

/**
 * Set stopped flag. Called by mtl_session_stop().
 */
static inline void mtl_session_set_stopped(struct mtl_session_impl* s) {
  s->stopped = true;
  s->state = MTL_SESSION_STATE_STOPPED;
}

/**
 * Clear stopped flag. Called by mtl_session_start().
 */
static inline void mtl_session_clear_stopped(struct mtl_session_impl* s) {
  s->stopped = false;
  s->state = MTL_SESSION_STATE_STARTED;
}

/*************************************************************************
 * Logging Helpers
 *************************************************************************/

#define MTL_SESSION_LOG(level, s, fmt, ...) \
  level("%s(%d), " fmt, __func__, (s)->idx, ##__VA_ARGS__)

#define MTL_SESSION_DBG(s, fmt, ...) MTL_SESSION_LOG(dbg, s, fmt, ##__VA_ARGS__)
#define MTL_SESSION_INFO(s, fmt, ...) MTL_SESSION_LOG(info, s, fmt, ##__VA_ARGS__)
#define MTL_SESSION_WARN(s, fmt, ...) MTL_SESSION_LOG(warn, s, fmt, ##__VA_ARGS__)
#define MTL_SESSION_ERR(s, fmt, ...) MTL_SESSION_LOG(err, s, fmt, ##__VA_ARGS__)

#if defined(__cplusplus)
}
#endif

#endif /* _MT_LIB_SESSION_H_ */
