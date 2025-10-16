#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "media-lib-api.h"

#define TIMEOUT_MS   1000
#define NUM_BUFFERS  4
#define BUFFER_SIZE  1024

// Application-defined simple buffer structure.
typedef struct {
    uint8_t* data;   // Pointer to the buffer data.
    size_t size;     // Size of the buffer.
    int id;          // Buffer identifier.
} app_buffer_t;

int main(void) {
    // Assume instance is created by the library initialization routine.
    mtl_handle* instance = /* ... */ NULL;
    media_lib_video_session_config_t rx_config = {0};
    media_lib_session_t* session = NULL;
    uint8_t* registered_memory = NULL;
    mtl_dma_mem_handle* dma_mem = NULL;
    app_buffer_t* buffers[NUM_BUFFERS];
    media_lib_event_t event;
    int err;

    // Configure a receiver session in user-owned (zero-copy) mode.
    rx_config.base.type = MEDIA_LIB_SESSION_RECEIVER;
    rx_config.base.ownership = MEDIA_LIB_BUFFER_USER_OWNED;
    rx_config.base.buffer_size = BUFFER_SIZE;
    rx_config.base.num_buffers = NUM_BUFFERS;
    rx_config.base.address = "192.168.1.100";
    rx_config.base.port = 1234;
    rx_config.base.timeout_ms = TIMEOUT_MS;
    rx_config.width = 640;
    rx_config.height = 480;
    rx_config.framerate = 30;
    rx_config.format = 0; // e.g., V_FMT_YUV420P

    err = media_lib_video_session_create(instance, &rx_config, &session);
    if (err != MEDIA_LIB_SUCCESS) {
        printf("Failed to create receiver session\n");
        return -1;
    }

    // Allocate a contiguous block of memory for all buffers.
    registered_memory = malloc(NUM_BUFFERS * BUFFER_SIZE);
    if (!registered_memory) {
        printf("Failed to allocate memory\n");
        return -1;
    }
    err = media_lib_mem_register(session, registered_memory, NUM_BUFFERS * BUFFER_SIZE, &dma_mem);
    if (err != MEDIA_LIB_SUCCESS) {
        printf("Failed to register memory\n");
        free(registered_memory);
        return -1;
    }

    // Create an array of application-defined buffers.
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = malloc(sizeof(app_buffer_t));
        if (!buffers[i]) {
            printf("Failed to allocate app_buffer_t for buffer %d\n", i);
            // Cleanup omitted for brevity.
            return -1;
        }
        buffers[i]->data = registered_memory + i * BUFFER_SIZE;
        buffers[i]->size = BUFFER_SIZE;
        buffers[i]->id = i;
        // Post the buffer to the library.
        // The app_buffer_t pointer is passed as the context.
        err = media_lib_buffer_post(session, buffers[i]->data, buffers[i]->size, (void*)buffers[i]);
        if (err != MEDIA_LIB_SUCCESS) {
            printf("Failed to post rx buffer %d\n", i);
        }
    }

    // Polling loop: process received buffers and repost them.
    while (1) {
        err = media_lib_event_poll(session, &event, TIMEOUT_MS);
        if (err == MEDIA_LIB_SUCCESS) {
            if (event.type == MEDIA_LIB_EVENT_BUFFER_RECEIVED) {
                // event.ctx is the application context.
                app_buffer_t* buf = (app_buffer_t*)event.ctx;
                printf("Received buffer id %d, size %ld\n", buf->id, buf->size);
                // Process the data in buf->data as needed...
                
                // After processing, repost the buffer back to the library.
                err = media_lib_buffer_post(session, buf->data, buf->size, (void*)buf);
                if (err != MEDIA_LIB_SUCCESS) {
                    printf("Failed to repost rx buffer for id %d\n", buf->id);
                }
            }
        }
    }

    // Cleanup (never reached in this sample).
    media_lib_mem_unregister(session, dma_mem);
    media_lib_session_shutdown(session);
    media_lib_session_destroy(session);
    free(registered_memory);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free(buffers[i]);
    }
    return 0;
}
