/**
 * Sample: Signal Handler Shutdown Pattern
 * 
 * Demonstrates proper graceful shutdown when using signal handlers
 * (Ctrl+C / SIGINT / SIGTERM) with MTL sessions.
 * 
 * Key points:
 * - mtl_session_stop() is async-signal-safe
 * - After stop(), buffer_get/event_poll return -EAGAIN
 * - Always wait for worker threads before destroy()
 * 
 * For basic API usage without signal handling, see other samples.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include "mtl_session_api_improved.h"

/*
 * Global context for signal handler access.
 * In production, you might use thread-local storage or other patterns.
 */
static mtl_session_t* g_session = NULL;
static volatile int g_running = 1;

/**
 * Signal handler - called on Ctrl+C or kill signal.
 * 
 * IMPORTANT: This runs in signal context - only async-signal-safe
 * functions can be called. mtl_session_stop() is designed to be safe here.
 */
void signal_handler(int sig) {
    const char* sig_name = (sig == SIGINT) ? "SIGINT" : "SIGTERM";
    
    /* write() is async-signal-safe, printf() is NOT */
    write(STDOUT_FILENO, "\nReceived signal, stopping...\n", 30);
    (void)sig_name;
    
    g_running = 0;
    
    /* 
     * Stop the session - this causes any blocked buffer_get() or 
     * event_poll() calls to return -EAGAIN immediately.
     */
    if (g_session) {
        mtl_session_stop(g_session);
    }
}

/**
 * RX worker thread - processes incoming frames.
 */
void* rx_worker(void* arg) {
    mtl_session_t* session = (mtl_session_t*)arg;
    mtl_buffer_t* buffer = NULL;
    int err;
    int frame_count = 0;
    
    printf("RX worker started\n");
    
    while (g_running) {
        /* 
         * This blocks until a frame arrives or timeout.
         * After mtl_session_stop(), returns -EAGAIN immediately.
         */
        err = mtl_session_buffer_get(session, &buffer, 1000);
        
        if (err == -EAGAIN) {
            /* Session stopped - exit cleanly */
            printf("RX worker: session stopped, exiting\n");
            break;
        }
        if (err == -ETIMEDOUT) {
            /* Normal timeout - check running flag and retry */
            continue;
        }
        if (err < 0) {
            printf("RX worker: error %d\n", err);
            break;
        }
        
        /* Process the received frame */
        frame_count++;
        printf("Received frame %d, size=%zu, timestamp=%lu\n",
               frame_count, buffer->data_size, buffer->timestamp);
        
        /* Return buffer to library */
        mtl_session_buffer_put(session, buffer);
    }
    
    printf("RX worker exiting after %d frames\n", frame_count);
    return NULL;
}

/**
 * TX worker thread - generates and transmits frames.
 */
void* tx_worker(void* arg) {
    mtl_session_t* session = (mtl_session_t*)arg;
    mtl_buffer_t* buffer = NULL;
    int err;
    int frame_count = 0;
    
    printf("TX worker started\n");
    
    while (g_running) {
        /* 
         * Get a free buffer. Blocks if all buffers are in-flight.
         * After mtl_session_stop(), returns -EAGAIN immediately.
         */
        err = mtl_session_buffer_get(session, &buffer, 1000);
        
        if (err == -EAGAIN) {
            /* Session stopped - exit cleanly */
            printf("TX worker: session stopped, exiting\n");
            break;
        }
        if (err == -ETIMEDOUT) {
            /* All buffers busy or timeout - check running and retry */
            continue;
        }
        if (err < 0) {
            printf("TX worker: error %d\n", err);
            break;
        }
        
        /* Fill frame with test pattern */
        memset(buffer->data, 0x80, buffer->data_size);
        frame_count++;
        
        /* Submit for transmission */
        err = mtl_session_buffer_put(session, buffer);
        if (err < 0) {
            printf("TX worker: buffer_put error %d\n", err);
            break;
        }
        
        if (frame_count % 100 == 0) {
            printf("Transmitted %d frames\n", frame_count);
        }
    }
    
    printf("TX worker exiting after %d frames\n", frame_count);
    return NULL;
}

int main(int argc, char** argv) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    pthread_t worker_tid;
    int is_tx = (argc > 1 && strcmp(argv[1], "tx") == 0);
    int err;
    
    printf("=== Signal Shutdown Demo (%s mode) ===\n", is_tx ? "TX" : "RX");
    printf("Press Ctrl+C to test graceful shutdown.\n\n");
    
    /* Install signal handlers BEFORE creating session */
    struct sigaction sa = {
        .sa_handler = signal_handler,
        .sa_flags = 0,
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    
    /* Create session based on mode */
    if (is_tx) {
        mtl_video_config_t config = {
            .base = {
                .direction = MTL_SESSION_TX,
                .ownership = MTL_BUFFER_LIBRARY_OWNED,
                .num_buffers = 3,
                .name = "shutdown_demo_tx",
            },
            .tx_port = {
                .dip_addr = {239, 168, 1, 100},
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
    } else {
        mtl_video_config_t config = {
            .base = {
                .direction = MTL_SESSION_RX,
                .ownership = MTL_BUFFER_LIBRARY_OWNED,
                .num_buffers = 3,
                .name = "shutdown_demo_rx",
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
        
        err = mtl_video_session_create(mt, &config, &g_session);
    }
    
    if (err < 0) {
        printf("Failed to create session: %d\n", err);
        return err;
    }
    
    err = mtl_session_start(g_session);
    if (err < 0) {
        printf("Failed to start session: %d\n", err);
        goto cleanup;
    }
    
    /* Start worker thread */
    if (is_tx) {
        pthread_create(&worker_tid, NULL, tx_worker, g_session);
    } else {
        pthread_create(&worker_tid, NULL, rx_worker, g_session);
    }
    
    /* 
     * Main thread waits for worker.
     * Worker will exit when signal handler calls mtl_session_stop().
     */
    pthread_join(worker_tid, NULL);
    
    printf("\nWorker thread joined. Cleaning up...\n");

cleanup:
    /*
     * Shutdown sequence:
     * 1. stop() already called by signal handler
     * 2. Worker thread already exited
     * 3. Now safe to destroy
     */
    mtl_session_destroy(g_session);
    
    printf("Shutdown complete.\n");
    return 0;
}
