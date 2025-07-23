#include <stdio.h>
#include "media-lib-api.h"

#define TIMEOUT_MS 1000

int main(void) {
    // Assume instance is created by the library initialization routine.
    mtl_handle* instance = /* ... */ NULL;
    media_lib_video_session_config_t rx_config = {0};
    media_lib_session_t* session = NULL;
    media_lib_buffer_t* buffer = NULL;
    int err;

    /* Configure a receiver session in library-owned mode */
    rx_config.base.type = MEDIA_LIB_SESSION_RECEIVER;
    rx_config.base.ownership = MEDIA_LIB_BUFFER_LIBRARY_OWNED;
    rx_config.base.buffer_size = 1024;
    rx_config.base.num_buffers = 4;
    rx_config.base.address = "192.168.1.102";
    rx_config.base.port = 1236;
    rx_config.base.timeout_ms = TIMEOUT_MS;
    rx_config.width = 640;
    rx_config.height = 480;
    rx_config.framerate = 30;
    rx_config.format = /* e.g., V_FMT_YUV420P */ 0;

    err = media_lib_video_session_create(instance, &rx_config, &session);
    if (err != MEDIA_LIB_SUCCESS) {
        printf("Failed to create receiver session\n");
        return -1;
    }

    /* Loop to receive and process buffers */
    while (1) {
        err = media_lib_buffer_get(session, &buffer, TIMEOUT_MS);
        if (err == MEDIA_LIB_SUCCESS) {
            printf("Received lib-owned buffer (size: %zu)\n", buffer->size);
            /* Process the received data here... */

            /* Return the buffer back to the library */
            err = media_lib_buffer_put(session, buffer);
            if (err != MEDIA_LIB_SUCCESS) {
                printf("Failed to release buffer\n");
            }
        }
    }

    media_lib_session_shutdown(session);
    media_lib_session_destroy(session);
    return 0;
}
