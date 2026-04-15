/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host A: MTL ST20 RX session
 */

#ifndef POC_MTL_RX_H
#define POC_MTL_RX_H

#include <mtl_api.h>
#include <st20_api.h>

#include "poc_frame_queue.h"
#include "poc_types.h"

typedef struct {
  mtl_handle mtl;
  st20_rx_handle rx_handle;
  poc_frame_queue_t* queue; /* shared with bridge worker */
  poc_stats_t* stats;
  bool accept_incomplete;
  /* Zero-copy: bridge context for query_ext_frame dispatch */
  void* ext_frame_ctx;
  int (*ext_frame_fn)(void*, struct st20_ext_frame*, struct st20_rx_frame_meta*);
} poc_mtl_rx_t;

/* Phase 1: Initialise MTL transport (DPDK init, port setup).
 * Does NOT create the ST20 RX session yet.
 * Returns 0 on success. */
int poc_mtl_rx_init_transport(poc_mtl_rx_t* rx, const poc_config_t* cfg);

/* Phase 2: Create the ST20 RX session with zero-copy ext_frame
 * callback provided by the bridge.
 * bridge->query_ext_frame is used as the dynamic ext_frame provider.
 * Returns 0 on success. */
int poc_mtl_rx_create_session(poc_mtl_rx_t* rx, const poc_config_t* cfg,
                              poc_frame_queue_t* queue, poc_stats_t* stats,
                              void* ext_frame_priv,
                              int (*query_ext_frame)(void*, struct st20_ext_frame*,
                                                     struct st20_rx_frame_meta*));

/* Tear down MTL session and handle. */
void poc_mtl_rx_destroy(poc_mtl_rx_t* rx);

#endif /* POC_MTL_RX_H */
