#include <stdio.h>
#include "media-lib-api.h"

#define TIMEOUT_MS 1000

int main(void) {
    // Assume instance is created by the library initialization routine.
    mtl_handle* instance = /* ... */ NULL;
    media_lib_video_session_config_t tx_config = {0};
    media_lib_session_t* session = NULL;
    media_lib_buffer_t* buffer = NULL;
    int err;

    /* Configure a transmitter session in library-owned mode */
    tx_config.base.type = MEDIA_LIB_SESSION_TRANSMITTER;
    tx_config.base.ownership = MEDIA_LIB_BUFFER_LIBRARY_OWNED;
    tx_config.base.buffer_size = 1024;
    tx_config.base.num_buffers = 4;
    tx_config.base.address = "192.168.1.103";
    tx_config.base.port = 1237;
    tx_config.base.timeout_ms = TIMEOUT_MS;
    tx_config.width = 640;
    tx_config.height = 480;
    tx_config.framerate = 30;
    tx_config.format = /* e.g., V_FMT_YUV420P */ 0;

    err = media_lib_video_session_create(instance, &tx_config, &session);
    if (err != MEDIA_LIB_SUCCESS) {
        printf("Failed to create transmitter session\n");
        return -1;
    }

    /* Loop to acquire a buffer, fill it with data, and transmit it */
    while (1) {
        err = media_lib_buffer_get(session, &buffer, TIMEOUT_MS);
        if (err == MEDIA_LIB_SUCCESS) {
            printf("Acquired lib-owned buffer for transmission (size: %zu)\n", buffer->size);
            /* Fill buffer->data with the media data to transmit */

            err = media_lib_buffer_put(session, buffer);
            if (err != MEDIA_LIB_SUCCESS) {
                printf("Failed to transmit buffer\n");
            }
            /* After transmission the buffer will be returned to the library, usually via an event */
        }
    }

    media_lib_session_shutdown(session);
    media_lib_session_destroy(session);
    return 0;
}
