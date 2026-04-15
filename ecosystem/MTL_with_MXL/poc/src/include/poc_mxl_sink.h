/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: MXL FlowWriter sink creation
 */

#ifndef POC_MXL_SINK_H
#define POC_MXL_SINK_H

#include <mxl/flow.h>
#include <mxl/flowinfo.h>
#include <mxl/mxl.h>

#include "poc_types.h"

typedef struct {
  mxlInstance instance;
  mxlFlowWriter writer;
  mxlFlowReader reader; /* optional: for downstream local readers */
  mxlFlowConfigInfo config_info;
  bool created;
  char* flow_json; /* loaded flow def (owned) */
} poc_mxl_sink_t;

/* Create MXL instance + flow writer from the flow JSON file.
 * Populates sink->config_info with grainCount, sliceSizes, etc.
 * Returns 0 on success. */
int poc_mxl_sink_init(poc_mxl_sink_t* sink, const poc_config_t* cfg);

/* Tear down MXL objects. */
void poc_mxl_sink_destroy(poc_mxl_sink_t* sink);

#endif /* POC_MXL_SINK_H */
