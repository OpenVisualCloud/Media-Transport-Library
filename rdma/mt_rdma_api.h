/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2024 Intel Corporation
 */

/**
 * @file mt_rdma_api.h
 *
 * Interfaces for RDMA transport.
 *
 */

#ifndef _MT_RDMA_API_HEAD_H_
#define _MT_RDMA_API_HEAD_H_

#if defined(__cplusplus)
extern "C" {
#endif

/** Handle to rdma tx session of lib */
typedef struct mt_rdma_tx_ctx* mt_rdma_tx_handle;
/** Handle to rdma rx session of lib */
typedef struct mt_rdma_rx_ctx* mt_rdma_rx_handle;

/** The structure describing how to create a tx session. */
struct mt_rdma_tx_ops {
  /** rdma server ip */
  char* ip;
  /** rdma server port */
  uint16_t port;
  /** buffers addresses */
  void** buffers;
  /** The number of buffers. */
  size_t num_buffers;
  /** The size of each buffer, all buffers should have the same size. */
  size_t buffer_size;
};

/**
 * Create one rdma tx session.
 *
 * @param mtr
 *   The handle to the media transport rdma context.
 * @param ops
 *   The pointer to the structure describing how to create a tx
 *  session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the tx session.
 */
mt_rdma_tx_handle mt_rdma_tx_create(mtl_rdma_handle mtr, struct mt_rdma_tx_ops* ops);

/**
 * Free the tx session.
 *
 * @param handle
 *   The handle to the tx session.
 * @return
 *   - 0: Success, tx session freed.
 *   - <0: Error code of the tx session free.
 */
int mt_rdma_tx_free(mt_rdma_tx_handle handle);

/**
 * Get one tx buffer from the tx session.
 * Call mt_rdma_tx_put_buffer to return the buffer to session.
 *
 * @param handle
 *   The handle to the tx session.
 * @return
 *   - NULL if no available buffer in the session.
 *   - Otherwise, the buffer pointer.
 */
void* mt_rdma_tx_get_buffer(mt_rdma_tx_handle handle);

/**
 * Put back the buffer which get by mt_rdma_tx_get_buffer to the tx
 * session.
 *
 * @param handle
 *   The handle to the tx session.
 * @param buffer
 *   the buffer pointer by mt_rdma_tx_get_buffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int mt_rdma_tx_put_buffer(mt_rdma_tx_handle handle, void* buffer);

/**
 * Create one rdma tx session.
 *
 * @param mtr
 *   The handle to the media transport rdma context.
 * @param ops

/** The structure describing how to create a rx session. */
struct mt_rdma_rx_ops {
  /** local rdma device ip */
  char* local_ip;
  /** rdma server ip */
  char* ip;
  /** rdma server port */
  uint16_t port;
  /** buffers addresses */
  void** buffers;
  /** The number of buffers. */
  size_t num_buffers;
  /** The size of each buffer, all buffers should have the same size. */
  size_t buffer_size;
};

/**
 * Create one rdma rx session.
 *
 * @param mtr
 *   The handle to the media transport rdma context.
 * @param ops
 *   The pointer to the structure describing how to create a rx
 *  session.
 * @return
 *   - NULL on error.
 *   - Otherwise, the handle to the rx session.
 */
mt_rdma_rx_handle mt_rdma_rx_create(mtl_rdma_handle mtr, struct mt_rdma_rx_ops* ops);

/**
 * Free the rx session.
 *
 * @param handle
 *   The handle to the rx session.
 * @return
 *   - 0: Success, rx session freed.
 *   - <0: Error code of the rx session free.
 */
int mt_rdma_rx_free(mt_rdma_rx_handle handle);

/**
 * Get one rx buffer from the rx session.
 * Call mt_rdma_rx_put_buffer to return the buffer to session.
 *
 * @param handle
 *   The handle to the rx session.
 * @return
 *   - NULL if no available buffer in the session.
 *   - Otherwise, the buffer pointer.
 */
void* mt_rdma_rx_get_buffer(mt_rdma_rx_handle handle);

/**
 * Put back the buffer which get by mt_rdma_rx_get_buffer to the rx
 * session.
 *
 * @param handle
 *   The handle to the rx session.
 * @param buffer
 *   the buffer pointer by mt_rdma_rx_get_buffer.
 * @return
 *   - 0 if successful.
 *   - <0: Error code if put fail.
 */
int mt_rdma_rx_put_buffer(mt_rdma_rx_handle handle, void* buffer);

#if defined(__cplusplus)
}
#endif

#endif /* _MT_RDMA_API_HEAD_H_ */