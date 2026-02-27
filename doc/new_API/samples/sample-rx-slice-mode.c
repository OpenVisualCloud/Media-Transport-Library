/**
 * Example: Video RX with Slice Mode (Ultra-Low Latency)
 * 
 * Demonstrates receiving and processing video line-by-line
 * as packets arrive, achieving sub-frame latency.
 */

#include <stdio.h>
#include "mtl_session_api_improved.h"

#define HEIGHT 1080
#define MAX_FRAMES 100

/* Placeholder functions */
static inline size_t get_linesize(void) { return 1920 * 2; }
static inline void process_video_line(uint8_t* line, uint16_t line_num) { (void)line; (void)line_num; }

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    int err;
    int frame_count = 0;
    
    /* Configure video RX session with SLICE mode for ultra-low latency */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_RX,
            .ownership = MTL_BUFFER_LIBRARY_OWNED,
            .num_buffers = 3,
            .name = "slice_rx",
            .flags = MTL_SESSION_FLAG_RECEIVE_INCOMPLETE_FRAME,  /* Required! */
        },
        .rx_port = {
            .ip_addr = {239, 168, 85, 20},
            .port = {20000},
            .payload_type = 112,
        },
        .width = 1920,
        .height = HEIGHT,
        .fps = ST_FPS_P59_94,
        .frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,
        .transport_fmt = ST20_FMT_YUV_422_10BIT,

        /* Slice mode: get events as lines arrive */
        .mode = MTL_VIDEO_MODE_SLICE,
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
    
    /* Event-driven loop: process lines as they arrive */
    mtl_buffer_t* current_buf = NULL;
    uint16_t last_lines_processed = 0;
    mtl_event_t event;
    
    printf("Receiving %d frames (slice mode)...\n", MAX_FRAMES);

    while (frame_count < MAX_FRAMES) {
        err = mtl_session_event_poll(session, &event, 1000);
        if (err == -ETIMEDOUT) continue;
        if (err < 0) break;

        switch (event.type) {
        case MTL_EVENT_BUFFER_READY:
            current_buf = event.buffer.buf;
            last_lines_processed = 0;
            break;

        case MTL_EVENT_SLICE_READY:
            for (uint16_t line = last_lines_processed; line < event.slice.lines_ready; line++) {
                uint8_t* line_ptr = (uint8_t*)event.slice.buffer + line * get_linesize();
                process_video_line(line_ptr, line);
            }
            last_lines_processed = event.slice.lines_ready;
            break;

        case MTL_EVENT_BUFFER_DONE:
            if (current_buf) {
                mtl_session_buffer_put(session, current_buf);
                current_buf = NULL;
                frame_count++;
            }
            break;

        case MTL_EVENT_ERROR:
            printf("Error: %d\n", event.error.code);
            break;

        default:
            break;
        }
    }

    printf("Received %d frames.\n", frame_count);

cleanup:
    mtl_session_stop(session);
    mtl_session_destroy(session);
    return 0;
}

/* Alternative: Polling-based slice processing */
void polling_example(mtl_session_t* session) {
    mtl_buffer_t* buf;
    mtl_session_buffer_get(session, &buf, 1000);

    uint16_t last_lines = 0;
    while (last_lines < HEIGHT) {
        uint16_t lines_ready;
        mtl_session_slice_query(session, buf, &lines_ready);

        for (uint16_t line = last_lines; line < lines_ready; line++) {
            uint8_t* line_ptr = (uint8_t*)buf->data + line * get_linesize();
            process_video_line(line_ptr, line);
        }
        last_lines = lines_ready;
        usleep(10);
    }

    mtl_session_buffer_put(session, buf);
}
