/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mtl_rdma_api.h
 *
 * Interfaces for RDMA transport.
 *
 */

#ifndef _MTL_RDMA_API_HEAD_H_
#define _MTL_RDMA_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Handle to MTL RDMA transport context.
 */
typedef struct mt_rdma_impl* mtl_rdma_handle;

/** Handle to RDMA tx session of lib. */
typedef struct mt_rdma_tx_ctx* mtl_rdma_tx_handle;
/** Handle to RDMA rx session of lib. */
typedef struct mt_rdma_rx_ctx* mtl_rdma_rx_handle;

/** The structure info for buffer meta. */
struct mtl_rdma_buffer {
  /** Buffer address */
  void* addr;
  /** Buffer size */
  size_t size;
  /** Buffer sequence number */
  uint32_t seq_num;
  /** Buffer timestamp */
  uint64_t timestamp;
  /** User metadata */
  void* user_meta;
  /** User metadata size */
  size_t user_meta_size;
};

/** The structure describing how to create a tx session. */
struct mtl_rdma_tx_ops {
  /** RDMA server ip. */
  char* ip;
  /** RDMA server port. */
  char* port;
  /** The number of buffers. */
  uint16_t num_buffers;
  /** Buffers addresses. */
  void** buffers;
  /** The size of each buffer, all buffers should have the same size. */
  size_t buffer_size;

  /**
   * Callback function to notify the buffer is done.
   * Implement with non-blocking function as it runs in the polling thread.
   */
  int (*notify_buffer_done)(void* priv, struct mtl_rdma_buffer* buffer);
};

/**
 * Create one RDMA tx session.
 *
 * @param mrh
 *   The handle to the media transport RDMA context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 *  session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx session.
 */
mtl_rdma_tx_handle mtl_rdma_tx_create(mtl_rdma_handle mrh, struct mtl_rdma_tx_ops* ops);

/**
 * Free the tx session.
 *
 * @param handle
 *   The handle to the tx session.
 * @return
 *   - 0: Success, tx session freed.
 *   - <0: Error code of the tx session free.
 */
int mtl_rdma_tx_free(mtl_rdma_tx_handle handle);

/**
 * Get one tx buffer from the tx session.
 * Call mtl_rdma_tx_put_buffer to return the buffer to session.
 *
 * @param handle
 *   The handle to the tx session.
 * @return
 *   - NULL if no available buffer in the session.
 *   - Otherwise, the buffer pointer.
 */
struct mtl_rdma_buffer* mtl_rdma_tx_get_buffer(mtl_rdma_tx_handle handle);

/**
 * Put back the buffer which get by mtl_rdma_tx_get_buffer to the tx
 * session.
 *
 * @param handle
 *   The handle to the tx session.
 * @param buffer
 *   the buffer pointer by mtl_rdma_tx_get_buffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int mtl_rdma_tx_put_buffer(mtl_rdma_tx_handle handle, struct mtl_rdma_buffer* buffer);

/**
 * Create one RDMA tx session.
 *
 * @param mrh
 *   The handle to the media transport RDMA context.
 * @param ops

/** The structure describing how to create a rx session. */
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
  /** The size of each buffer, all buffers should have the same size. */
  size_t buffer_size;

  /**
   * Callback function to notify the buffer is ready.
   * Implement with non-blocking function as it runs in the polling thread.
   */
  int (*notify_buffer_ready)(void* priv, struct mtl_rdma_buffer* buffer);
};

/**
 * Create one RDMA rx session.
 *
 * @param mrh
 *   The handle to the media transport RDMA context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 *  session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx session.
 */
mtl_rdma_rx_handle mtl_rdma_rx_create(mtl_rdma_handle mrh, struct mtl_rdma_rx_ops* ops);

/**
 * Free the rx session.
 *
 * @param handle
 *   The handle to the rx session.
 * @return
 *   - 0: Success, rx session freed.
 *   - <0: Error code of the rx session free.
 */
int mtl_rdma_rx_free(mtl_rdma_rx_handle handle);

/**
 * Get one rx buffer from the rx session.
 * Call mtl_rdma_rx_put_buffer to return the buffer to session.
 *
 * @param handle
 *   The handle to the rx session.
 * @return
 *   - NULL if no available buffer in the session.
 *   - Otherwise, the buffer pointer.
 */
struct mtl_rdma_buffer* mtl_rdma_rx_get_buffer(mtl_rdma_rx_handle handle);

/**
 * Put back the buffer which get by mtl_rdma_rx_get_buffer to the rx
 * session.
 *
 * @param handle
 *   The handle to the rx session.
 * @param buffer
 *   the buffer pointer by mtl_rdma_rx_get_buffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int mtl_rdma_rx_put_buffer(mtl_rdma_rx_handle handle, struct mtl_rdma_buffer* buffer);

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