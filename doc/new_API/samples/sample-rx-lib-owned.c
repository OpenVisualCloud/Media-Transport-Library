/**
 * Sample: RX with Library-Owned Buffers
 * 
 * Simplest RX pattern - library manages buffer allocation.
 * Use mtl_session_buffer_get() to receive data, process it, put buffer back.
 */

#include <stdio.h>
#include "mtl_session_api_improved.h"

#define MAX_FRAMES 100

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    mtl_buffer_t* buffer = NULL;
    int err;
    int frame_count = 0;

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

    /* Receive MAX_FRAMES frames then exit */
    printf("Receiving %d frames...\n", MAX_FRAMES);
    while (frame_count < MAX_FRAMES) {
        err = mtl_session_buffer_get(session, &buffer, 1000);
        if (err == -ETIMEDOUT) {
            continue;
        }
        if (err < 0) {
            printf("buffer_get error: %d\n", err);
            break;
        }

        printf("Frame %d: %p, size=%zu, ts=%lu\n",
               frame_count, buffer->data, buffer->data_size, buffer->timestamp);

        if (buffer->flags & MTL_BUF_FLAG_INCOMPLETE) {
            printf("  Warning: incomplete frame\n");
        }

        err = mtl_session_buffer_put(session, buffer);
        if (err < 0) {
            printf("buffer_put error: %d\n", err);
            break;
        }
        frame_count++;
    }

    printf("Received %d frames.\n", frame_count);

cleanup:
    mtl_session_stop(session);
    mtl_session_destroy(session);
    return 0;
}
