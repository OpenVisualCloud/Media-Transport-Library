/**
 * Sample: TX with User-Owned Buffers (Zero-Copy)
 * 
 * Advanced TX pattern - application provides its own buffers.
 * Use mtl_session_mem_register() + mtl_session_buffer_post() for zero-copy TX.
 * Poll for completion events to know when buffers can be reused.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "mtl_session_api_improved.h"

#define NUM_BUFFERS  4
#define FRAME_SIZE   (1920 * 1080 * 2)  /* YUV422 */

/* Application's buffer tracking */
typedef struct {
    void* data;
    size_t size;
    int id;
    volatile int in_use;  /* 1 = submitted to library */
} app_buffer_t;

static mtl_session_t* g_session = NULL;
static app_buffer_t g_buffers[NUM_BUFFERS];
static volatile int g_running = 1;

/* Producer thread: generates frames and posts them */
void* producer_thread(void* arg) {
    int next_buf = 0;
    int err;

    while (g_running) {
        app_buffer_t* buf = &g_buffers[next_buf];
        
        /* Wait for buffer to be free */
        while (buf->in_use && g_running) {
            usleep(1000);
        }
        if (!g_running) break;

        /* Fill buffer with frame data */
        memset(buf->data, 0x80, buf->size);
        printf("Producer: filled buffer %d\n", buf->id);

        /* Post buffer for transmission */
        buf->in_use = 1;
        err = mtl_session_buffer_post(g_session, buf->data, buf->size, buf);
        if (err < 0) {
            printf("buffer_post failed: %d\n", err);
            buf->in_use = 0;
        }

        next_buf = (next_buf + 1) % NUM_BUFFERS;
        usleep(16666);  /* ~60fps */
    }
    return NULL;
}

/* Event thread: handles completion events */
void* event_thread(void* arg) {
    mtl_event_t event;
    int err;

    while (g_running) {
        err = mtl_session_event_poll(g_session, &event, 100);
        if (err == -ETIMEDOUT) {
            continue;
        }
        if (err < 0) {
            printf("event_poll error: %d\n", err);
            break;
        }

        if (event.type == MTL_EVENT_BUFFER_DONE) {
            /* Buffer transmission complete - can reuse */
            app_buffer_t* buf = (app_buffer_t*)event.ctx;
            printf("Event: buffer %d transmitted\n", buf->id);
            buf->in_use = 0;
        } else if (event.type == MTL_EVENT_ERROR) {
            printf("Event: error %d\n", event.status);
        }
    }
    return NULL;
}

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_dma_mem_t* dma_handle = NULL;
    void* buffer_region = NULL;
    pthread_t prod_tid, event_tid;
    int err;

    /* Configure video TX with user-owned buffers */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_TX,
            .ownership = MTL_BUFFER_USER_OWNED,
            .num_buffers = NUM_BUFFERS,
            .name = "video_tx_zerocopy",
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

    err = mtl_video_session_create(mt, &config, &g_session);
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

    err = mtl_session_mem_register(g_session, buffer_region, 
                                    NUM_BUFFERS * FRAME_SIZE, &dma_handle);
    if (err < 0) {
        printf("Failed to register memory: %d\n", err);
        goto cleanup_mem;
    }

    /* Initialize app buffer tracking */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_buffers[i].data = (char*)buffer_region + i * FRAME_SIZE;
        g_buffers[i].size = FRAME_SIZE;
        g_buffers[i].id = i;
        g_buffers[i].in_use = 0;
    }

    err = mtl_session_start(g_session);
    if (err < 0) {
        printf("Failed to start session: %d\n", err);
        goto cleanup_dma;
    }

    /* Start worker threads */
    pthread_create(&prod_tid, NULL, producer_thread, NULL);
    pthread_create(&event_tid, NULL, event_thread, NULL);

    /* Run for 10 seconds */
    sleep(10);
    g_running = 0;

    pthread_join(prod_tid, NULL);
    pthread_join(event_tid, NULL);

cleanup_dma:
    mtl_session_mem_unregister(g_session, dma_handle);
cleanup_mem:
    free(buffer_region);
cleanup_session:
    mtl_session_shutdown(g_session);
    mtl_session_destroy(g_session);
    return 0;
}
