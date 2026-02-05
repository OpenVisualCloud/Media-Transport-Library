/**
 * Example: Video TX with Slice Mode (Ultra-Low Latency)
 * 
 * Demonstrates line-by-line transmission for sub-frame latency.
 * Library transmits lines as they become available, not waiting
 * for the full frame.
 */

#include <stdio.h>
#include "mtl_session_api_improved.h"

#define HEIGHT 1080

struct app_context {
    uint16_t current_line;
};

/* Callback: library queries how many lines are ready */
static int query_lines_ready(void* priv, uint16_t frame_idx, uint16_t* lines_ready) {
    struct app_context* ctx = priv;
    *lines_ready = ctx->current_line;
    return 0;
}

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    mtl_buffer_t* buffer = NULL;
    struct app_context ctx = {0};
    int err;
    
    /* Configure video TX session with SLICE mode for ultra-low latency */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_TX,
            .ownership = MTL_BUFFER_LIBRARY_OWNED,
            .num_buffers = 3,
            .name = "slice_tx",
            .priv = &ctx,
        },
        .tx_port = {
            .dip_addr = {239, 168, 85, 20},
            .port = {20000},
            .payload_type = 112,
        },
        .width = 1920,
        .height = HEIGHT,
        .fps = ST_FPS_P59_94,
        .frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .transport_fmt = ST20_FMT_YUV_422_10BIT,
        .pacing = ST21_PACING_NARROW,

        /* Slice mode: line-by-line transmission */
        .mode = MTL_VIDEO_MODE_SLICE,
        .query_lines_ready = query_lines_ready,
    };

    err = mtl_video_session_create(mt, &config, &session);
    if (err < 0) {
        printf("Failed to create session: %d\n", err);
        return err;
    }

    err = mtl_session_start(session);
    if (err < 0) {
        printf("Failed to start session: %d\n", err);
        goto cleanup;
    }

    /* Main loop: fill buffer line-by-line, signal progress */
    while (1) {
        err = mtl_session_buffer_get(session, &buffer, 1000);
        if (err == -ETIMEDOUT) continue;
        if (err < 0) break;

        ctx.current_line = 0;

        /* Fill lines progressively - library transmits as they're ready */
        for (uint16_t line = 0; line < HEIGHT; line++) {
            uint8_t* line_ptr = (uint8_t*)buffer->data + line * config.linesize;
            fill_video_line(line_ptr, line);

            ctx.current_line = line + 1;
            mtl_session_slice_ready(session, buffer, line + 1);
        }

        err = mtl_session_buffer_put(session, buffer);
        if (err < 0) break;
    }

cleanup:
    mtl_session_stop(session);
    mtl_session_destroy(session);
    return 0;
}
