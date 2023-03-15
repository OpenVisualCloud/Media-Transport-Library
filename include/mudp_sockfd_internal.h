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
/** Marco for re-inculde protect */
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
  /** shared queue mode */
  bool shared_queue;
  /** lcore mode */
  bool lcore_mode;
};

/**
 * Commit the runtime parameters of mufd instance.
 *
 * @param p
 *   The pointer to the rt parameters.
 * @return
 *   - >=0: Success.
 *   - <0: Error code.
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
};

/**
 * Commit the runtime parameters of mufd instance.
 *
 * @param p
 *   The pointer to the mufd init parameters.
 * @return
 *   - >=0: Success.
 *   - <0: Error code.
 */
int mufd_commit_init_params(struct mufd_init_params* p);

/**
 * Get the session nb of mufd context.
 *
 * @return
 *   - >=0: Success, the number.
 *   - <0: Error code.
 */
int mufd_get_sessions_max_nb(void);

/**
 * Init mufd context with json config from MUFD_CFG_ENV_NAME env.
 *
 * @return
 *   - >=0: Success.
 *   - <0: Error code.
 */
int mufd_init_context(void);

/**
 * Get the base fd of mufd context.
 *
 * @return
 *   - >=0: Success, the base fd.
 *   - <0: Error code.
 */
int mufd_base_fd(void);

#if defined(__cplusplus)
}
#endif

#endif
