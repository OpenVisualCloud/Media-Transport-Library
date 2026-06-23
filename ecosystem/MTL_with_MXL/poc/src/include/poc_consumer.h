/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: Consumer / display stub
 */

#ifndef POC_CONSUMER_H
#define POC_CONSUMER_H

#include "poc_fabrics_target.h"
#include "poc_mxl_sink.h"
#include "poc_types.h"

/* Forward-declare multi-instance thumbnail context for poc_14 */
struct poc14_thumb_ctx;

typedef struct {
  poc_mxl_sink_t* sink;
  poc_fabrics_target_t* ft;
  poc_stats_t* stats;
  volatile bool* running;
  /* Video params for thumbnail generation */
  uint32_t src_width;
  uint32_t src_height;
  const char* thumb_dir;           /* NULL = no thumbnail (single-instance) */
  struct poc14_thumb_ctx* thumb16; /* NULL = no multi-instance thumbnail */
} poc_consumer_ctx_t;

/* Run the consumer loop: poll fabrics target for events, read grain data,
 * validate payload (CRC), and optionally dump frames.
 * Blocks until *running becomes false.  Call from a dedicated thread. */
void poc_consumer_run(poc_consumer_ctx_t* ctx);

#endif /* POC_CONSUMER_H */
