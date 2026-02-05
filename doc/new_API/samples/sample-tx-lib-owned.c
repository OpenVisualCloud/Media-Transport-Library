/**
 * Sample: TX with Library-Owned Buffers
 * 
 * Simplest TX pattern - library manages buffer allocation.
 * Use mtl_session_buffer_get() to get empty buffer, fill it, put it back.
 */

#include <stdio.h>
#include <string.h>
#include "mtl_session_api_improved.h"

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    mtl_buffer_t* buffer = NULL;
    int err;

    /* Configure video TX session with library-owned buffers */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_TX,
            .ownership = MTL_BUFFER_LIBRARY_OWNED,
            .num_buffers = 4,
            .name = "video_tx_sample",
        },
        .tx_port = {
            .dip_addr = {192, 168, 1, 100},
            .port = {20000},
            .payload_type = 112,
        },
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
        .frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .transport_fmt = ST20_FMT_YUV_422_10BIT,
        .pacing = ST21_PACING_NARROW,
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

    /* Main loop: get buffer, fill, put */
    while (1) {
        /* Get empty buffer from library (blocks up to 1000ms) */
        err = mtl_session_buffer_get(session, &buffer, 1000);
        if (err == -ETIMEDOUT) {
            continue;  /* No buffer available, try again */
        }
        if (err < 0) {
            printf("buffer_get error: %d\n", err);
            break;
        }

        /* Fill buffer with video data */
        /* For video, can use buffer->video.planes[] for planar formats */
        printf("Got buffer: %p, size=%zu, planes[0]=%p\n", 
               buffer->data, buffer->size, buffer->video.planes[0]);
        
        /* Example: fill with test pattern */
        memset(buffer->data, 0x80, buffer->size);

        /* Submit filled buffer for transmission */
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
