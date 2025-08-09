#ifndef MEDIA_LIB_API_H
#define MEDIA_LIB_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "buffers-api.h"

/*** Library Instance, the API for instance stays without changes ***/
typedef struct mtl_handle mtl_handle;

/*** Some Example Error Codes.. to be defined***/
typedef enum {
    MEDIA_LIB_SUCCESS = 0,
    MEDIA_LIB_ERROR_INVALID_PARAMETER = -1,
    // MEDIA_LIB_ERROR_NOT_INITIALIZED = -2,
    // MEDIA_LIB_ERROR_OUT_OF_MEMORY = -3,
    // MEDIA_LIB_ERROR_TIMEOUT = -4,
    // MEDIA_LIB_ERROR_CONNECTION_FAILED = -5,
    // MEDIA_LIB_ERROR_DISCONNECTED = -6,
    // MEDIA_LIB_ERROR_BUFFER_FULL = -7,
    // MEDIA_LIB_ERROR_BUFFER_EMPTY = -8,
    // MEDIA_LIB_ERROR_INVALID_STATE = -9,
    // MEDIA_LIB_ERROR_UNSUPPORTED = -10,
    // MEDIA_LIB_ERROR_UNKNOWN = -100
} media_lib_error_t;

/*** Media Type Enumeration ***/
typedef enum {
    MEDIA_LIB_TYPE_VIDEO = 0,
    MEDIA_LIB_TYPE_AUDIO = 1,
    MEDIA_LIB_TYPE_ANCILLARY = 2,
    MEDIA_LIB_TYPE_FAST_METADATA = 3,
} media_lib_type_t;

/*** Session Type Enumeration ***/
typedef enum {
    MEDIA_LIB_SESSION_RECEIVER = 0,
    MEDIA_LIB_SESSION_TRANSMITTER = 1
} media_lib_session_type_t;

/*** Buffer Ownership ***/
typedef enum {
    MEDIA_LIB_BUFFER_USER_OWNED = 0,
    MEDIA_LIB_BUFFER_LIBRARY_OWNED = 1
} media_lib_buffer_ownership_t;

/*** Event Types ***/
typedef enum {
    MEDIA_LIB_EVENT_NONE = 0,
    MEDIA_LIB_EVENT_BUFFER_RECEIVED = 1,       // A buffer was received
    MEDIA_LIB_EVENT_BUFFER_TRANSMITTED = 2,    // A buffer was transmitted
    MEDIA_LIB_EVENT_BUFFER_AVAILABLE = 3,      // A buffer is available for use
    MEDIA_LIB_EVENT_ERROR = 5                  // An error occurred
} media_lib_event_type_t;

/*** Event Structure ***/
// In library-owned mode, it is a pointer to a media_lib_buffer_t.
// In app-owned mode, it is the application context passed in post_rx/post_tx.
typedef struct {
    media_lib_event_type_t type;        // Type of event
    media_lib_error_t status;           // Status/error code
    uint64_t timestamp;                 // Event timestamp
    void* ctx;                          // Associated buffer or context pointer (if applicable)
} media_lib_event_t;


/*** Session ***/
typedef struct media_lib_session media_lib_session_t;

/*** Base Session Configuration ***/
typedef struct {
    media_lib_session_type_t type;          // Receiver or transmitter
    media_lib_buffer_ownership_t ownership; // Buffer ownership mode

    /* Buffer configuration */
    size_t   buffer_size;         // Size of each buffer (in bytes)
    uint32_t num_buffers;         // Total number of buffers in the pool
    
    /* Network configuration */
    //address port etc. this will be copied from current API

} media_lib_session_base_config_t;

/*** Video-specific Session Configuration ***/
typedef struct {
    media_lib_session_base_config_t base;  // Base configuration
    
    /* Video configuration */
    // uint32_t width;             // Video frame width (pixels)
    // uint32_t height;            // Video frame height (pixels)
    // uint32_t framerate;         // Frames per second
    // v_fmt_t format;             // Video format (e.g., "YUV420P")
    // etc.

} media_lib_video_session_config_t;

/*** Audio-specific Session Configuration ***/
typedef struct {
    media_lib_session_base_config_t base;  // Base configuration
    
    /* Audio configuration */
    // uint32_t sample_rate;       // Samples per second
    // uint32_t channels;          // Number of audio channels
    // uint32_t bits_per_sample;   // Bits per sample
    // a_fmt_t format;             // Audio format string (e.g., "PCM", "AAC")
    // etc.
} media_lib_audio_session_config_t;

/*** Session Interface (Function Table) ***/
typedef struct {
    /*
     * media_lib_session_t operations
     *
     * These function pointers define the basic operations on a media session.
     * The session is automatically activated upon creation, so the start() call 
     * is intended to restart it if it has been stopped. All operations return a
     * media_lib_error_t code to indicate success or failure.
     */

    /* 
     * start - Activate or resume the media session.
     * @session: Pointer to the media session instance.
     *
     * If the session has been stopped, this call will re-enable processing.
     * The session is auto-activated at creation, so start() is used primarily 
     * for explicit reactivation after a stop().
     */
    media_lib_error_t (*start)(media_lib_session_t *session);

    /*
     * stop - Temporarily halt media session processing.
     * @session: Pointer to the media session instance.
     *
     * This function stops the session without deallocating resources, allowing
     * for a later restart via start(). It is a clean, temporary shutdown of active
     * operations.
     */
    media_lib_error_t (*stop)(media_lib_session_t *session);

    /*
     * shutdown - Gracefully terminate the session's asynchronous operations.
     * @session: Pointer to the media session instance.
     *
     * This call is intended for an orderly shutdown of background tasks, ensuring
     * that no further processing occurs before the session is destroyed. It should
     * be invoked prior to destroy() to guarantee that all activities have ceased.
     */
    media_lib_error_t (*shutdown)(media_lib_session_t *session);

    /*
     * destroy - Free all resources associated with the media session.
     * @session: Pointer to the media session instance.
     *
     * This function deallocates the session's resources and should be called only
     * after a proper shutdown. Once destroy() is executed, the session pointer
     * becomes invalid.
     */
    media_lib_error_t (*destroy)(media_lib_session_t *session);
    
    /*
     * Buffer management - Library-owned mode.
     * get a buffer with a timeout. 'buffer_get' should block until a buffer is 
     * available or the timeout expires.
     */
    media_lib_error_t (*buffer_get)(media_lib_session_t *session,
                                    media_lib_buffer_t **buffer,
                                    uint32_t timeout_ms);

    /*
     * Buffer management - Library-owned mode.
     * buffer_put - Return a previously acquired buffer back to the library.
     * The caller relinquishes ownership of the buffer.
     */
    media_lib_error_t (*buffer_put)(media_lib_session_t *session,
                                    media_lib_buffer_t *buffer);
                                    
    /*
     * Zero-copy buffer operation - App-owned mode.
     * buffer_post - Post an app-owned buffer to the session for processing.
     * This allows for zero-copy transfers from the application to the library.
     */
    media_lib_error_t (*buffer_post)(media_lib_session_t *session,
                                    void *data,
                                    size_t size,
                                    void *app_ctx);

    /*
     * DMA memory management - App-owned mode.
     * mem_register - Register an app-owned memory region for DMA operations.
     * On success, a DMA memory handle is returned in dma_mem.
     */
    media_lib_error_t (*mem_register)(media_lib_session_t *session,
                                      void *data,
                                      size_t size, 
                                      mtl_dma_mem_handle **dma_mem);

    /*
     * mem_unregister - Unregister a previously registered DMA memory region.
     * The provided DMA memory handle is invalidated after this call.
     */
    media_lib_error_t (*mem_unregister)(media_lib_session_t *session,
                                        mtl_dma_mem_handle *dma_mem);

    /*
     * buffers_flush - Flush all buffers in the session.
     * @session: Pointer to the media session instance.
     * @timeout_ms: Timeout in milliseconds.
     *
     * This function waits till all buffers in the session are processed,
     * ensuring that no pending data remains.
     */
    media_lib_error_t (*buffers_flush)(media_lib_session_t *session, uint32_t timeout_ms);

    /*
     * Polling interface.
     * event_poll - Wait for an event on the media session with a timeout.
     * Should return quickly if an event is pending, or block until timeout.
     */
    media_lib_error_t (*event_poll)(media_lib_session_t *session,
                                    media_lib_event_t *event,
                                    uint32_t timeout_ms);
    
    /*
     * Statistics and monitoring.
     * stats_get - Retrieve current session statistics.
     * The stats pointer should be filled with data about current session performance.
     */
    media_lib_error_t (*stats_get)(media_lib_session_t *session,
                                   void *stats);
    
} media_lib_session_vtable_t;


/*** Session Statistics ***/
typedef struct {
    // uint64_t frames_processed;       // Total frames/buffers sent or received
    // uint64_t bytes_processed;        // Total bytes transmitted or received
    // uint64_t dropped_frames;         // Frames/buffers dropped due to errors
    // double current_rate;             // Current effective frame/sample rate
    // uint32_t total_buffers;          // Total number of buffers
    // uint32_t buffers_in_use;         // Number of buffers currently in use
    // uint64_t last_timestamp;         // Timestamp of the last processed frame/sample
    
    // /* Queue statistics */
    // uint32_t free_queue_depth;       // Number of buffers available for acquisition
    // uint32_t transmit_queue_depth;   // Number of buffers pending transmission
    
} media_lib_session_stats_t;

/*************************************************************************
 * Session Creation Functions
 *************************************************************************/

/* Create a video session */
media_lib_error_t media_lib_video_session_create(
    mtl_handle* instance,
    const media_lib_video_session_config_t* config,
    media_lib_session_t** session
);

/* Create an audio session */
media_lib_error_t media_lib_audio_session_create(
    mtl_handle* instance,
    const media_lib_audio_session_config_t* config,
    media_lib_session_t** session
);

/* Get the media type of a session */
media_lib_type_t media_lib_session_get_type(
    media_lib_session_t* session
);

/*************************************************************************
 * Polymorphic Interface Functions
 *************************************************************************/

/* Session lifecycle */
inline media_lib_error_t media_lib_session_start(media_lib_session_t* session) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->start(session);
}

inline media_lib_error_t media_lib_session_stop(media_lib_session_t* session) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->stop(session);
}

inline media_lib_error_t media_lib_session_shutdown(media_lib_session_t* session) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->shutdown(session);
}

inline media_lib_error_t media_lib_session_destroy(media_lib_session_t* session) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->destroy(session);
}

/* Buffer management - Library-owned mode */
inline media_lib_error_t media_lib_buffer_get(
    media_lib_session_t* session,
    media_lib_buffer_t** buffer,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->buffer_get(session, buffer, timeout_ms);
}

inline media_lib_error_t media_lib_buffer_put(
    media_lib_session_t* session,
    media_lib_buffer_t* buffer
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->buffer_put(session, buffer);
}

/* DMA memory management - User-owned mode */
inline media_lib_error_t media_lib_mem_register(
    media_lib_session_t* session,
    void* data,
    size_t size,
    mtl_dma_mem_handle** dma_mem
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->mem_register(session, data, size, dma_mem);
}

inline media_lib_error_t media_lib_mem_unregister(
    media_lib_session_t* session,
    mtl_dma_mem_handle* dma_mem
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->mem_unregister(session, dma_mem);
}

/* Zero-copy buffer operations - App-owned mode */
inline media_lib_error_t media_lib_buffer_post(
    media_lib_session_t* session,
    void* data,
    size_t size,
    void* app_ctx
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->buffer_post(session, data, size, app_ctx);
}

inline media_lib_error_t media_lib_buffers_flush(
    media_lib_session_t* session,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->buffers_flush(session, timeout_ms);
}

/* Polling interface */
inline media_lib_error_t media_lib_event_poll(
    media_lib_session_t* session,
    media_lib_event_t* event,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->event_poll(session, event, timeout_ms);
}

/* Statistics and monitoring */
inline media_lib_error_t media_lib_stats_get(
    media_lib_session_t* session,
    media_lib_session_stats_t* stats
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->stats_get(session, stats);
}

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_LIB_API_H */
