/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mtl_rdma_api.h
 *
 * Interfaces for RDMA transport.
 *
 */

#include <stddef.h>

#ifndef _MTL_RDMA_API_HEAD_H_
#define _MTL_RDMA_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Get the uint64_t value for a specified bit set(0 to 63).
 */
#define MTL_RDMA_BIT64(nr) (UINT64_C(1) << (nr))

/**
 * Handle to MTL RDMA transport context.
 */
typedef struct mt_rdma_impl* mtl_rdma_handle;

/** Handle to RDMA TX session of lib. */
typedef struct mt_rdma_tx_ctx* mtl_rdma_tx_handle;
/** Handle to RDMA RX session of lib. */
typedef struct mt_rdma_rx_ctx* mtl_rdma_rx_handle;

/**
 * Log level type to MTL RDMA transport context
 */
enum mtl_rdma_log_level {
  /** debug log level */
  MTL_RDMA_LOG_LEVEL_DEBUG = 0,
  /** info log level */
  MTL_RDMA_LOG_LEVEL_INFO,
  /** notice log level */
  MTL_RDMA_LOG_LEVEL_NOTICE,
  /** warning log level */
  MTL_RDMA_LOG_LEVEL_WARNING,
  /** error log level */
  MTL_RDMA_LOG_LEVEL_ERR,
  /** critical log level */
  MTL_RDMA_LOG_LEVEL_CRIT,
  /** max value of this enum */
  MTL_RDMA_LOG_LEVEL_MAX,
};

/** The structure info for buffer meta. */
struct mtl_rdma_buffer {
  /** Buffer address, immutable at runtime */
  void* addr;
  /** Buffer data capacity, immutable at runtime */
  size_t capacity;
  /** Buffer valid data offset, mutable at runtime */
  size_t offset;
  /** Buffer valid data size, mutable at runtime */
  size_t size;
  /** Buffer sequence number */
  uint32_t seq_num;
  /** Buffer timestamp, use nanoseconds in lib */
  uint64_t timestamp;

  /** User metadata */
  void* user_meta;
  /** User metadata size */
  size_t user_meta_size;
};

/** The structure describing how to create a TX session. */
struct mtl_rdma_tx_ops {
  /** RDMA server ip. */
  char* ip;
  /** RDMA server port. */
  char* port;
  /** The number of buffers. */
  uint16_t num_buffers;
  /** Buffers addresses. */
  void** buffers;
  /** The max size of each buffer, all buffers should have the same capacity. */
  size_t buffer_capacity;

  /** Session name */
  const char* name;
  /** Optional. Private data to the callback function */
  void* priv;
  /**
   * Optional. Callback function to notify the buffer is sent by local side.
   * Implement with non-blocking function as it runs in the polling thread.
   */
  int (*notify_buffer_sent)(void* priv, struct mtl_rdma_buffer* buffer);
  /**
   * Optional. Callback function to notify the buffer is consumed by remote side.
   * Implement with non-blocking function as it runs in the polling thread.
   */
  int (*notify_buffer_done)(void* priv, struct mtl_rdma_buffer* buffer);
};

/**
 * Create one RDMA TX session.
 *
 * @param mrh
 *   The handle to the RDMA transport context.
 * @param ops
 *   The pointer to the structure describing how to create a TX
 *  session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the TX session.
 */
mtl_rdma_tx_handle mtl_rdma_tx_create(mtl_rdma_handle mrh, struct mtl_rdma_tx_ops* ops);

/**
 * Free the TX session.
 *
 * @param handle
 *   The handle to the TX session.
 * @return
 *   - 0: Success, TX session freed.
 *   - <0: Error code of the TX session free.
 */
int mtl_rdma_tx_free(mtl_rdma_tx_handle handle);

/**
 * Get one TX buffer from the TX session.
 * Call mtl_rdma_tx_put_buffer to return the buffer to session.
 *
 * @param handle
 *   The handle to the TX session.
 * @return
 *   - NULL if no available buffer in the session.
 *   - Otherwise, the buffer pointer.
 */
struct mtl_rdma_buffer* mtl_rdma_tx_get_buffer(mtl_rdma_tx_handle handle);

/**
 * Put back the buffer which get by mtl_rdma_tx_get_buffer to the TX
 * session.
 *
 * @param handle
 *   The handle to the TX session.
 * @param buffer
 *   the buffer pointer by mtl_rdma_tx_get_buffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int mtl_rdma_tx_put_buffer(mtl_rdma_tx_handle handle, struct mtl_rdma_buffer* buffer);

/** The structure describing how to create an RX session. */
struct mtl_rdma_rx_ops {
  /** Local RDMA interface ip */
  char* local_ip;
  /** RDMA server ip */
  char* ip;
  /** RDMA server port */
  char* port;
  /** The number of buffers. */
  uint16_t num_buffers;
  /** buffers addresses */
  void** buffers;
  /** The max size of each buffer, all buffers should have the same capacity. */
  size_t buffer_capacity;

  /** Session name */
  const char* name;
  /** Optional. Private data to the callback function */
  void* priv;
  /**
   * Callback function to notify the buffer is ready to consume.
   * Implement with non-blocking function as it runs in the polling thread.
   */
  int (*notify_buffer_ready)(void* priv, struct mtl_rdma_buffer* buffer);
};

/**
 * Create one RDMA RX session.
 *
 * @param mrh
 *   The handle to the RDMA transport context.
 * @param ops
 *   The pointer to the structure describing how to create a RX
 *  session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the RX session.
 */
mtl_rdma_rx_handle mtl_rdma_rx_create(mtl_rdma_handle mrh, struct mtl_rdma_rx_ops* ops);

/**
 * Free the RX session.
 *
 * @param handle
 *   The handle to the RX session.
 * @return
 *   - 0: Success, RX session freed.
 *   - <0: Error code of the RX session free.
 */
int mtl_rdma_rx_free(mtl_rdma_rx_handle handle);

/**
 * Get one RX buffer from the RX session.
 * Call mtl_rdma_rx_put_buffer to return the buffer to session.
 *
 * @param handle
 *   The handle to the RX session.
 * @return
 *   - NULL if no available buffer in the session.
 *   - Otherwise, the buffer pointer.
 */
struct mtl_rdma_buffer* mtl_rdma_rx_get_buffer(mtl_rdma_rx_handle handle);

/**
 * Put back the buffer which get by mtl_rdma_rx_get_buffer to the RX
 * session.
 *
 * @param handle
 *   The handle to the RX session.
 * @param buffer
 *   the buffer pointer by mtl_rdma_rx_get_buffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int mtl_rdma_rx_put_buffer(mtl_rdma_rx_handle handle, struct mtl_rdma_buffer* buffer);

/** MTL RDMA init flag */
enum mtl_rdma_init_flag {
  /** Lib will bind app thread and memory to RDMA device numa node.*/
  MTL_RDMA_FLAG_BIND_NUMA = (MTL_RDMA_BIT64(0)),
  /**
   * Enable low latency mode for buffer transport.
   * The TX and RX will poll for work completions.
   * It will cause extra CPU usage.
   */
  MTL_RDMA_FLAG_LOW_LATENCY = (MTL_RDMA_BIT64(1)),
  /** Enable shared receive queue for all sessions. */
  MTL_RDMA_FLAG_ENABLE_SRQ = (MTL_RDMA_BIT64(2)),
  /** Enable shared completion queue for all sessions. */
  MTL_RDMA_FLAG_SHARED_CQ = (MTL_RDMA_BIT64(3)),
};

/**
 * The structure describing how to initialize RDMA transport.
 */
struct mtl_rdma_init_params {
  /** Number of RDMA devices. (reserved for future) */
  uint32_t num_devices;
  /** RDMA devices names. (reserved for future) */
  char** devices;
  /** RDMA flags. (reserved for future) */
  uint64_t flags;
  /** Log Level */
  enum mtl_rdma_log_level log_level;
};

/**
 * Initialize RDMA transport.
 *
 * @param p
 *   The pointer to the structure describing how to initialize
 *   RDMA transport.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the RDMA transport context.
 */
mtl_rdma_handle mtl_rdma_init(struct mtl_rdma_init_params* p);

/**
 * Uninitialize RDMA transport.
 *
 * @param mrh
 *   The handle to the RDMA transport context.
 * @return
 *   - 0: Success, RDMA transport uninitialized.
 *   - <0: Error code of the RDMA transport uninit.
 */
int mtl_rdma_uinit(mtl_rdma_handle mrh);

#if defined(__cplusplus)
}
#endif

#endif /* _MTL_RDMA_API_HEAD_H_ */