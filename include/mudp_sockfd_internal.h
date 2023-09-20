/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2023 Intel Corporation
 */

/**
 * @file mudp_sockfd_internal.h
 *
 * Internal interfaces to udp transport context.
 * For debug/test usage only
 *
 */

#ifndef _MUDP_SOCKFD_INTERNAL_HEAD_H_
/** Marco for re-include protect */
#define _MUDP_SOCKFD_INTERNAL_HEAD_H_

#include "mtl_api.h"

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * All config should be parsed from MUFD_CFG_ENV_NAME json config file.
 * But we still need some runtime args(ex: log level) for debug usage.
 */
struct mufd_override_params {
  /** log level */
  enum mtl_log_level log_level;
  /** shared tx queue mode */
  bool shared_tx_queue;
  /** shared rx queue mode */
  bool shared_rx_queue;
  /** rss mode */
  enum mtl_rss_mode rss_mode;
  /** lcore mode */
  bool lcore_mode;
};

#define MUFD_FLAG_BIND_ADDRESS_CHECK (MTL_BIT64(0))

/**
 * Commit the runtime parameters of mufd instance.
 *
 * @param p
 *   The pointer to the rt parameters.
 * @return
 *   - >=0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_commit_override_params(struct mufd_override_params* p);

/**
 * All init params should be parsed from MUFD_CFG_ENV_NAME json config file.
 * But we still need a runtime entry for debug/test usage.
 */
struct mufd_init_params {
  /** mtl init params */
  struct mtl_init_params mt_params;
  /** max nb of udp sockets supported */
  int slots_nb_max;
  /** fd base of udp sockets */
  int fd_base;
  /** bit per sec for q */
  uint64_t txq_bps;
  /** rx ring count */
  unsigned int rx_ring_count;
  /** wakeup when rte_ring_count(s->rx_ring) reach this threshold */
  unsigned int wake_thresh_count;
  /** wakeup when timeout with last wakeup */
  unsigned int wake_timeout_us;
  /** rx poll sleep time */
  unsigned int rx_poll_sleep_us;
  /** flags, value with MUFD_FLAG_* */
  uint64_t flags;
};

/**
 * Commit the runtime parameters of mufd instance.
 *
 * @param p
 *   The pointer to the mufd init parameters.
 * @return
 *   - >=0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_commit_init_params(struct mufd_init_params* p);

/**
 * Get the session nb of mufd context.
 *
 * @return
 *   - >=0: Success, the number.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_get_sessions_max_nb(void);

/**
 * Init mufd context with json config from MUFD_CFG_ENV_NAME env.
 *
 * @return
 *   - >=0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_init_context(void);

/**
 * Get the base fd of mufd context.
 *
 * @return
 *   - >=0: Success, the base fd.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_base_fd(void);

/**
 * Get the log level of mufd context.
 *
 * @return
 *   - log level
 */
enum mtl_log_level mufd_log_level(void);

/**
 * Set private opaque data on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param pri
 *   The opaque data pointer.
 *
 * @return
 *   - >=0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_set_opaque(int sockfd, void* pri);

/**
 * Get private opaque data on the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 *
 * @return
 *   - The opaque data pointer.
 */
void* mufd_get_opaque(int sockfd);

/**
 * Get IP address of the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param ip
 *   The pointer to the IP address buffer.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_get_sip(int sockfd, uint8_t ip[MTL_IP_ADDR_LEN]);

/**
 * Check if the dst ip is reach by the udp transport socket.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param dip
 *   The pointer to the dst IP address buffer.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_tx_valid_ip(int sockfd, uint8_t dip[MTL_IP_ADDR_LEN]);

/**
 * Register stat dump callback fuction.
 *
 * @param sockfd
 *   the sockfd by mufd_socket.
 * @param dump
 *   The dump callback fuction.
 * @param priv
 *   The priv data to the callback fuction.
 * @return
 *   - 0: Success.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_register_stat_dump_cb(int sockfd, int (*dump)(void* priv), void* priv);

/**
 * Allocate memory from the huge-page area of memory. The memory is not cleared.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 * Note the memory is mmap to IOVA already, use mtl_hp_virt2iova to get the iova.
 *
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the pointer to the allocated memory.
 */
void* mufd_hp_malloc(size_t size, enum mtl_port port);

/**
 * Allocate zero'ed memory from the huge-page area of memory.
 * Equivalent to mufd_hp_malloc() except that the memory zone is cleared with zero.
 * In NUMA systems, the memory allocated from the same NUMA socket of the port.
 * Note the memory is mmap to IOVA already, use mtl_hp_virt2iova to get the iova.
 *
 * @param size
 *   Size (in bytes) to be allocated.
 * @param port
 *   Port for the memory to be allocated.
 * @return
 *   - NULL on error. Not enough memory, or invalid arguments
 *   - Otherwise, the virtual address pointer to the allocated memory.
 */
void* mufd_hp_zmalloc(size_t size, enum mtl_port port);

/**
 * Frees the memory pointed by the pointer.
 *
 * This pointer must have been returned by a previous call to
 * mufd_hp_malloc(), mufd_hp_malloc().
 * The behaviour is undefined if the pointer does not match this requirement.
 *
 * @param ptr
 *   The virtual address pointer to memory to be freed.
 */
void mufd_hp_free(void* ptr);

/**
 * Check if the socket type is support or not by mufd.
 *
 * @param domain
 *   A communication domain, only AF_INET(IPv4) now.
 * @param type
 *   Which specifies the communication semantics, only SOCK_DGRAM now.
 * @param protocol
 *   Specifies a particular protocol to be used with the socket, only zero now.
 * @return
 *   - 0: Yes, support.
 *   - <0: Fail, not support. -1 is returned, and errno is set appropriately.
 */
int mufd_socket_check(int domain, int type, int protocol);

/**
 * Poll the udp transport socket, blocks until one of the events occurs.
 * Only support POLLIN now.
 *
 * @param fds
 *   Point to pollfd request, fd is by mufd_socket.
 * @param nfds
 *   The number of request in fds.
 * @param timeout
 *   timeout value in ms.
 * @param query
 *   query callback, return > 0 means it has ready data on the query.
 * @param priv
 *   priv data to the query callback.
 * @return
 *   - > 0: Success, returns a nonnegative value which is the number of elements
 *          in the fds whose revents fields have been set to a nonzero value.
 *   - =0: Timeosockfd.
 *   - <0: Error code. -1 is returned, and errno is set appropriately.
 */
int mufd_poll_query(struct pollfd* fds, nfds_t nfds, int timeout,
                    int (*query)(void* priv), void* priv);

#if defined(__cplusplus)
}
#endif

#endif
