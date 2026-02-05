/**
 * Example: ST22 Compressed Video TX with Plugin
 * 
 * Demonstrates using ST22 codec plugins (JPEGXS, H264, etc.)
 * for compressed video transmission.
 */

#include <stdio.h>
#include "mtl_session_api_improved.h"

#define MAX_FRAMES 100

/* Placeholder function */
static inline void generate_video_frame(void* data, uint32_t w, uint32_t h) { (void)data; (void)w; (void)h; }

int main(void) {
    mtl_handle mt = NULL;  /* Assume created via mtl_init() */
    mtl_session_t* session = NULL;
    mtl_buffer_t* buffer = NULL;
    int err;
    int frame_count = 0;

    /*
     * Note: ST22 plugins must be registered BEFORE creating sessions.
     * st22_encoder_register(mt, &encoder_dev);
     */
    
    /* Configure ST22 compressed video TX session */
    mtl_video_config_t config = {
        .base = {
            .direction = MTL_SESSION_TX,
            .ownership = MTL_BUFFER_LIBRARY_OWNED,
            .num_buffers = 3,
            .name = "st22_tx",
        },
        .tx_port = {
            .dip_addr = {239, 168, 85, 22},
            .port = {20022},
            .payload_type = 114,
        },
        .width = 1920,
        .height = 1080,
        .fps = ST_FPS_P59_94,
        .frame_fmt = ST_FRAME_FMT_YUV422PLANAR10LE,

        /* ST22 compression settings */
        .compressed = true,
        .codec = ST22_CODEC_JPEGXS,
        .codestream_size = 2 * 1024 * 1024,  /* 2MB/frame */
        .plugin_device = ST_PLUGIN_DEVICE_AUTO,
        .quality = ST22_QUALITY_MODE_QUALITY,
        .codec_thread_cnt = 0,  /* Auto-detect */
    };
    
    err = mtl_video_session_create(mt, &config, &session);
    if (err < 0) {
        printf("Failed to create ST22 session: %d\n", err);
        return err;
    }

    /* Optional: Query plugin info */
    mtl_plugin_info_t plugin_info;
    if (mtl_session_get_plugin_info(session, &plugin_info) == 0) {
        printf("Using plugin: %s (%s)\n", plugin_info.name,
               plugin_info.device == ST_PLUGIN_DEVICE_GPU ? "GPU" : "CPU");
    }

    err = mtl_session_start(session);
    if (err < 0) {
        printf("Failed to start session: %d\n", err);
        goto cleanup;
    }

    /* Transmit MAX_FRAMES frames then exit */
    printf("Transmitting %d ST22 compressed frames...\n", MAX_FRAMES);
    while (frame_count < MAX_FRAMES) {
        err = mtl_session_buffer_get(session, &buffer, 1000);
        if (err == -ETIMEDOUT) continue;
        if (err < 0) break;

        /* Fill with uncompressed video - library encodes via plugin */
        generate_video_frame(buffer->data, buffer->video.width, buffer->video.height);

        err = mtl_session_buffer_put(session, buffer);
        if (err < 0) break;
        
        frame_count++;
    }

    printf("Transmitted %d frames.\n", frame_count);

cleanup:
    mtl_session_stop(session);
    mtl_session_destroy(session);
    return 0;
}
