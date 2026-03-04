/* SPDX-License-Identifier: BSD-3-Clause
 * MTL-MXL POC — Host B: MXL FlowWriter sink implementation
 */

#include "poc_mxl_sink.h"
#include "poc_json_utils.h"
#include "poc_v210_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int poc_mxl_sink_init(poc_mxl_sink_t *sink, const poc_config_t *cfg)
{
    memset(sink, 0, sizeof(*sink));

    /* ── Load flow definition JSON ── */
    size_t json_len = 0;
    sink->flow_json = poc_load_file(cfg->flow_json_path, &json_len);
    if (!sink->flow_json) {
        fprintf(stderr, "[SINK] Failed to load flow JSON: %s\n", cfg->flow_json_path);
        return -1;
    }
    printf("[SINK] Loaded flow JSON (%zu bytes) from %s\n", json_len, cfg->flow_json_path);

    /* ── Create MXL instance ── */
    sink->instance = mxlCreateInstance(cfg->mxl_domain, "");
    if (!sink->instance) {
        fprintf(stderr, "[SINK] mxlCreateInstance failed (domain=%s)\n", cfg->mxl_domain);
        goto err;
    }

    /* ── Create FlowWriter (allocates the ring buffer) ── */
    mxlStatus st = mxlCreateFlowWriter(sink->instance, sink->flow_json, NULL,
                                        &sink->writer, &sink->config_info, &sink->created);
    if (st != MXL_STATUS_OK) {
        fprintf(stderr, "[SINK] mxlCreateFlowWriter failed: %d\n", st);
        goto err;
    }

    printf("[SINK] FlowWriter %s (grainCount=%u, sliceSize[0]=%u)\n",
           sink->created ? "created new flow" : "opened existing flow",
           sink->config_info.discrete.grainCount,
           sink->config_info.discrete.sliceSizes[0]);

    /* ── Verify stride consistency ──
     * Note: MXL flow uses V210 stride for grain sizing, but the actual
     * data in the grain buffers is RFC 4175 (from st20_rx raw API).
     * RFC 4175 data packs tighter (width*5/2 bytes/line) and fits within
     * the V210-sized grain buffers. */
    uint32_t v210_stride = poc_v210_line_stride(cfg->video.width);
    uint32_t rfc4175_stride = (cfg->video.width * 5) / 2;
    printf("[SINK] Grain stride=%u  (V210=%u, RFC4175 data stride=%u)\n",
           sink->config_info.discrete.sliceSizes[0], v210_stride, rfc4175_stride);

    /* ── Optionally create a FlowReader for local consumption ── */
    /* We extract the flow ID from the config info for the reader */
    {
        char flow_id_str[64];
        /* Format UUID from the 16-byte config info id */
        uint8_t *id = sink->config_info.common.id;
        snprintf(flow_id_str, sizeof(flow_id_str),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 id[0],id[1],id[2],id[3], id[4],id[5], id[6],id[7],
                 id[8],id[9], id[10],id[11],id[12],id[13],id[14],id[15]);

        st = mxlCreateFlowReader(sink->instance, flow_id_str, "", &sink->reader);
        if (st != MXL_STATUS_OK) {
            printf("[SINK] Note: mxlCreateFlowReader failed (%d), continuing without reader\n", st);
            sink->reader = NULL;
        }
    }

    return 0;

err:
    poc_mxl_sink_destroy(sink);
    return -1;
}

void poc_mxl_sink_destroy(poc_mxl_sink_t *sink)
{
    if (sink->reader && sink->instance) {
        mxlReleaseFlowReader(sink->instance, sink->reader);
        sink->reader = NULL;
    }
    if (sink->writer && sink->instance) {
        mxlReleaseFlowWriter(sink->instance, sink->writer);
        sink->writer = NULL;
    }
    if (sink->instance) {
        mxlDestroyInstance(sink->instance);
        sink->instance = NULL;
    }
    if (sink->flow_json) {
        free(sink->flow_json);
        sink->flow_json = NULL;
    }
}
