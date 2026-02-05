/**
 * Sample: RX with Library-Owned Buffers
 * 
 * Simplest RX pattern - library manages buffer allocation.
 * Use mtl_session_buffer_get() to receive data, process it, put buffer back.
 */

#include <stdio.h>
#include "mtl_session_api_improved.h"

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    mtl_buffer_t* buffer = NULL;
    int err;

    /* Configure video RX session with library-owned buffers */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_RX,
            .ownership = MTL_BUFFER_LIBRARY_OWNED,
            .num_buffers = 4,
            .name = "video_rx_sample",
        },
        .rx_port = {
            .ip_addr = {239, 168, 1, 100},  /* Multicast group */
            .port = {20000},
            .payload_type = 112,
        },
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
        .frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .transport_fmt = ST20_FMT_YUV_422_10BIT,
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

    /* Main loop: get received buffer, process, return */
    while (1) {
        /* Get buffer with received frame (blocks up to 1000ms) */
        err = mtl_session_buffer_get(session, &buffer, 1000);
        if (err == -ETIMEDOUT) {
            continue;  /* No frame received yet, keep waiting */
        }
        if (err < 0) {
            printf("buffer_get error: %d\n", err);
            break;
        }

        /* Process received video frame */
        printf("Received frame: %p, size=%zu, timestamp=%lu\n",
               buffer->data, buffer->data_size, buffer->timestamp);
        
        /* Check for incomplete frame (packet loss) */
        if (buffer->flags & MTL_BUF_FLAG_INCOMPLETE) {
            printf("Warning: incomplete frame\n");
        }

        /* Access video-specific fields */
        printf("  Resolution: %ux%u\n", 
               buffer->video.width, buffer->video.height);

        /* Return buffer to library for reuse */
        err = mtl_session_buffer_put(session, buffer);
        if (err < 0) {
            printf("buffer_put error: %d\n", err);
            break;
        }
    }

cleanup:
    mtl_session_shutdown(session);
    mtl_session_destroy(session);
    return 0;
}
