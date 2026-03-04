/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: Fabrics target setup
 */

#ifndef POC_FABRICS_TARGET_H
#define POC_FABRICS_TARGET_H

#include "poc_types.h"
#include "poc_mxl_sink.h"
#include <mxl/fabrics.h>

typedef struct {
    mxlFabricsInstance  fab_instance;
    mxlFabricsTarget    target;
    mxlFabricsTargetInfo target_info;
    mxlFabricsRegions   regions;
    char               *target_info_str;  /* serialized (owned) */
    size_t              target_info_len;
} poc_fabrics_target_t;

/* Create fabrics target bound to the sink's flow writer.
 * Prints serialized targetInfo to stdout for the sender. */
int poc_fabrics_target_init(poc_fabrics_target_t *ft,
                            poc_mxl_sink_t *sink,
                            const poc_config_t *cfg);

/* Destroy fabrics target objects. */
void poc_fabrics_target_destroy(poc_fabrics_target_t *ft);

#endif /* POC_FABRICS_TARGET_H */
