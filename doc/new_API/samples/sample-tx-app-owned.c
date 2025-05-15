#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>      // For usleep()
#include <pthread.h>
#include "media-lib-api.h"

#define TIMEOUT_MS   1000
#define NUM_BUFFERS  4
#define BUFFER_SIZE  1024  // Size of each buffer in bytes

// Application-defined simple buffer structure
typedef struct {
    uint8_t* data;  // Pointer to the buffer data (slice of registered memory)
    size_t size;    // Size of the buffer in bytes
    int id;         // Simple identifier (for logging or debugging)
} app_buffer_t;

// A simple circular queue to hold free app buffers
typedef struct {
    app_buffer_t* buffers[NUM_BUFFERS];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} buffer_queue_t;

buffer_queue_t free_queue;

void init_queue(buffer_queue_t* q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
}

void enqueue(buffer_queue_t* q, app_buffer_t* buffer) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == NUM_BUFFERS) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    q->buffers[q->tail] = buffer;
    q->tail = (q->tail + 1) % NUM_BUFFERS;
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

app_buffer_t* dequeue(buffer_queue_t* q) {
    app_buffer_t* buffer = NULL;
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0) {
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    buffer = q->buffers[q->head];
    q->head = (q->head + 1) % NUM_BUFFERS;
    q->count--;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return buffer;
}

// Global session pointer (assumed to be created successfully)
media_lib_session_t* session = NULL;
// Pointer to the registered memory block
uint8_t* registered_memory = NULL;
mtl_dma_mem_handle* dma_mem = NULL;

// Producer thread: simulates video frame generation and posts frames for transmission.
void* frame_generator_thread(void* arg) {
    app_buffer_t* buf;
    int err;

    while (1) {
        // Get a free buffer from the queue
        buf = dequeue(&free_queue);
        if (!buf)
            continue;

        // Simulate frame generation: fill the buffer with dummy data.
        memset(buf->data, 0xAB, buf->size);
        // (Set additional fields if needed, e.g., frame timestamp)

        // Post the buffer for transmission.
        // Note: We pass buf->data and buf->size, and use the app buffer as the context.
        err = media_lib_buffer_post(session, buf->data, buf->size, (void*)buf);
        if (err != MEDIA_LIB_SUCCESS) {
            printf("Failed to post tx buffer (id: %d)\n", buf->id);
            // If posting fails, return the buffer back to the free queue.
            enqueue(&free_queue, buf);
        } else {
            printf("Frame generated and posted for transmission (id: %d)\n", buf->id);
        }

        // Simulate frame rate (e.g., 30fps ~33ms per frame)
        usleep(33000);
    }
    return NULL;
}

// Poller thread: waits for transmitted events and recycles buffers.
void* event_handler_thread(void* arg) {
    media_lib_event_t event;
    int err;
    app_buffer_t* buf;

    while (1) {
        err = media_lib_event_poll(session, &event, TIMEOUT_MS);
        if (err == MEDIA_LIB_SUCCESS) {
            if (event.type == MEDIA_LIB_EVENT_BUFFER_TRANSMITTED) {
                // The library returns the transmitted buffer's app context in event.buffer.
                // Cast it back to our app_buffer_t pointer.
                buf = (app_buffer_t*)event.ctx;
                if (buf) {
                    printf("Buffer transmitted successfully (id: %d)\n", buf->id);
                    // Return the buffer to the free queue for reuse.
                    enqueue(&free_queue, buf);
                }
            }
        } else {
            // Poll timeout or error; can log or handle as needed.
        }
    }
    return NULL;
}

int main(void) {
    // Assume instance is obtained from your library initialization.
    mtl_handle* instance = /* e.g., media_lib_instance_create() */ NULL;
    media_lib_video_session_config_t tx_config = {0};
    pthread_t producer_thread, poller_thread;
    app_buffer_t* buf;
    int err;

    // Configure a transmitter session in app-owned (zero-copy) mode.
    tx_config.base.type = MEDIA_LIB_SESSION_TRANSMITTER;
    tx_config.base.ownership = MEDIA_LIB_BUFFER_USER_OWNED;
    tx_config.base.buffer_size = BUFFER_SIZE;
    tx_config.base.num_buffers = NUM_BUFFERS;
    tx_config.base.address = "192.168.1.101";
    tx_config.base.port = 1235;
    tx_config.base.timeout_ms = TIMEOUT_MS;
    tx_config.width = 640;
    tx_config.height = 480;
    tx_config.framerate = 30;
    tx_config.format = 0; // e.g., V_FMT_YUV420P

    err = media_lib_video_session_create(instance, &tx_config, &session);
    if (err != MEDIA_LIB_SUCCESS) {
        printf("Failed to create transmitter session\n");
        return -1;
    }

    // Allocate and register a contiguous memory block for all buffers.
    registered_memory = malloc(NUM_BUFFERS * BUFFER_SIZE);
    if (!registered_memory) {
        printf("Failed to allocate registered memory\n");
        return -1;
    }
    err = media_lib_mem_register(session, registered_memory, NUM_BUFFERS * BUFFER_SIZE, &dma_mem);
    if (err != MEDIA_LIB_SUCCESS) {
        printf("Failed to register memory\n");
        return -1;
    }

    // Initialize the free buffer queue.
    init_queue(&free_queue);

    // Create app-defined buffer structures and enqueue them.
    for (int i = 0; i < NUM_BUFFERS; i++) {
        buf = malloc(sizeof(app_buffer_t));
        if (!buf) {
            printf("Failed to allocate app buffer structure\n");
            return -1;
        }
        buf->data = registered_memory + i * BUFFER_SIZE;
        buf->size = BUFFER_SIZE;
        buf->id = i;
        enqueue(&free_queue, buf);
    }

    // Create threads: one for frame generation, one for event polling.
    pthread_create(&producer_thread, NULL, frame_generator_thread, NULL);
    pthread_create(&poller_thread, NULL, event_handler_thread, NULL);

    // In a real application, you would add proper termination handling.
    pthread_join(producer_thread, NULL);
    pthread_join(poller_thread, NULL);

    // Cleanup (not reached in this sample).
    media_lib_mem_unregister(session, dma_mem);
    media_lib_session_shutdown(session);
    media_lib_session_destroy(session);
    free(registered_memory);
    return 0;
}
