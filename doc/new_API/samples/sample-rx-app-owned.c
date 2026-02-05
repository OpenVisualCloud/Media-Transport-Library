/**
 * Sample: RX with User-Owned Buffers (Zero-Copy)
 * 
 * Advanced RX pattern - application provides its own buffers.
 * Post buffers to library, poll for received data, repost after processing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtl_session_api_improved.h"

#define NUM_BUFFERS  4
#define FRAME_SIZE   (1920 * 1080 * 2)  /* YUV422 */

/* Application's buffer tracking */
typedef struct {
    void* data;
    size_t size;
    int id;
} app_buffer_t;

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    mtl_dma_mem_t* dma_handle = NULL;
    void* buffer_region = NULL;
    app_buffer_t buffers[NUM_BUFFERS];
    mtl_event_t event;
    int err;

    /* Configure video RX with user-owned buffers */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_RX,
            .ownership = MTL_BUFFER_USER_OWNED,
            .num_buffers = NUM_BUFFERS,
            .name = "video_rx_zerocopy",
        },
        .rx_port = {
            .ip_addr = {239, 168, 1, 100},
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

    /* Allocate and register DMA-capable memory region */
    buffer_region = aligned_alloc(4096, NUM_BUFFERS * FRAME_SIZE);
    if (!buffer_region) {
        printf("Failed to allocate buffer memory\n");
        goto cleanup_session;
    }

    err = mtl_session_mem_register(session, buffer_region,
                                    NUM_BUFFERS * FRAME_SIZE, &dma_handle);
    if (err < 0) {
        printf("Failed to register memory: %d\n", err);
        goto cleanup_mem;
    }

    /* Initialize buffers and post them all to library */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i].data = (char*)buffer_region + i * FRAME_SIZE;
        buffers[i].size = FRAME_SIZE;
        buffers[i].id = i;

        /* Post buffer - library will fill it with received data */
        err = mtl_session_buffer_post(session, buffers[i].data, 
                                       buffers[i].size, &buffers[i]);
        if (err < 0) {
            printf("Failed to post buffer %d: %d\n", i, err);
        }
    }

    err = mtl_session_start(session);
    if (err < 0) {
        printf("Failed to start session: %d\n", err);
        goto cleanup_dma;
    }

    /* Main loop: poll for events, process data, repost buffers */
    printf("Receiving frames...\n");
    while (1) {
        err = mtl_session_event_poll(session, &event, 1000);
        if (err == -ETIMEDOUT) {
            continue;
        }
        if (err < 0) {
            printf("event_poll error: %d\n", err);
            break;
        }

        if (event.type == MTL_EVENT_BUFFER_READY) {
            /* Buffer contains received frame */
            app_buffer_t* buf = (app_buffer_t*)event.ctx;
            printf("Received frame in buffer %d, timestamp=%lu\n",
                   buf->id, event.timestamp);

            /* Process the received data in buf->data */
            /* ... */

            /* Repost buffer back to library for next frame */
            err = mtl_session_buffer_post(session, buf->data, buf->size, buf);
            if (err < 0) {
                printf("Failed to repost buffer %d: %d\n", buf->id, err);
            }
        } else if (event.type == MTL_EVENT_ERROR) {
            printf("Error event: %d\n", event.status);
        }
    }

cleanup_dma:
    mtl_session_mem_unregister(session, dma_handle);
cleanup_mem:
    free(buffer_region);
cleanup_session:
    mtl_session_shutdown(session);
    mtl_session_destroy(session);
    return 0;
}
