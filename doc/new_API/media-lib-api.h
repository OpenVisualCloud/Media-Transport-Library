#ifndef MEDIA_LIB_API_H
#define MEDIA_LIB_API_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*** Library Instance, the API for instance stays without changes ***/
typedef struct mtl_handle mtl_handle;

/*** Some Example Error Codes.. to be defined***/
typedef enum {
    MEDIA_LIB_SUCCESS = 0,
    MEDIA_LIB_ERROR_INVALID_PARAMETER = -1,
    MEDIA_LIB_ERROR_NOT_INITIALIZED = -2,
    MEDIA_LIB_ERROR_OUT_OF_MEMORY = -3,
    MEDIA_LIB_ERROR_TIMEOUT = -4,
    MEDIA_LIB_ERROR_CONNECTION_FAILED = -5,
    MEDIA_LIB_ERROR_DISCONNECTED = -6,
    MEDIA_LIB_ERROR_BUFFER_FULL = -7,
    MEDIA_LIB_ERROR_BUFFER_EMPTY = -8,
    MEDIA_LIB_ERROR_INVALID_STATE = -9,
    MEDIA_LIB_ERROR_UNSUPPORTED = -10,
    MEDIA_LIB_ERROR_UNKNOWN = -100
} media_lib_error_t;

/*** Media Type Enumeration ***/
typedef enum {
    MEDIA_LIB_TYPE_VIDEO = 0,
    MEDIA_LIB_TYPE_AUDIO = 1,
    MEDIA_LIB_TYPE_ANCILLARY = 2,
    MEDIA_LIB_TYPE_BLOB = 3
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
    MEDIA_LIB_EVENT_BUFFER_RECEIVED = 1,     // A buffer was received
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
    void* buffer;                       // Associated buffer or context pointer (if applicable)
} media_lib_event_t;

/*** Session ***/
typedef struct media_lib_session media_lib_session_t;

/*** Buffer Structure ***/
typedef struct media_lib_buffer {
    void* data;                  // Pointer to buffer data
    mtl_iova_t iova;             // Pointer to IOVA buffer
    size_t size;                 // Total size of the buffer in bytes
    uint64_t timestamp;          // Frame/sample timestamp (microseconds)
    uint32_t flags;              // Buffer flags (e.g., keyframe indicator)
    uint32_t buffer_id;          // Unique buffer ID for tracking
    void* _internal;             // Reserved for internal library use
    void* user_data;             // Optional user data pointer
    
    /* Media type-specific data */
    union {
        struct {
            /* Video-specific fields: width, height, format, etc. */
        } video;
        struct {
            /* Audio-specific fields: sample rate, channels, format, etc. */
        } audio;
    } media;
} media_lib_buffer_t;

/*** Base Session Configuration ***/
typedef struct {
    media_lib_session_type_t type;          // Receiver or transmitter
    media_lib_buffer_ownership_t ownership; // Buffer ownership mode

    /* Buffer configuration */
    size_t buffer_size;         // Size of each buffer (in bytes)
    uint32_t num_buffers;       // Total number of buffers in the pool
    
    /* Network configuration */
    //address port etc. this will be copied from current API

} media_lib_session_base_config_t;

/*** Video-specific Session Configuration ***/
typedef struct {
    media_lib_session_base_config_t base;  // Base configuration
    
    /* Video configuration */
    uint32_t width;             // Video frame width (pixels)
    uint32_t height;            // Video frame height (pixels)
    uint32_t framerate;         // Frames per second
    v_fmt_t format;             // Video format (e.g., "YUV420P")
    // etc.

} media_lib_video_session_config_t;

/*** Audio-specific Session Configuration ***/
typedef struct {
    media_lib_session_base_config_t base;  // Base configuration
    
    /* Audio configuration */
    uint32_t sample_rate;       // Samples per second
    uint32_t channels;          // Number of audio channels
    uint32_t bits_per_sample;   // Bits per sample
    a_fmt_t format;             // Audio format string (e.g., "PCM", "AAC")
    // etc.
} media_lib_audio_session_config_t;

/*** Session Interface (Function Table) ***/
typedef struct {
    /* Session lifecycle */
    media_lib_error_t (*start)(media_lib_session_t* session);
    media_lib_error_t (*stop)(media_lib_session_t* session);
    media_lib_error_t (*destroy)(media_lib_session_t* session);
    
    /* Buffer management - Library-owned mode */
    media_lib_error_t (*buffer_acquire)(media_lib_session_t* session,
                                        media_lib_buffer_t** buffer,
                                        uint32_t timeout_ms);
    media_lib_error_t (*buffer_release)(media_lib_session_t* session,
                                       media_lib_buffer_t* buffer);
    
    /* DMA memory management - User-owned mode */
    media_lib_error_t (*mem_register)(media_lib_session_t* session,
                                      void* data,
                                      size_t size, 
                                      mtl_dma_mem_handle** dma_mem);
    media_lib_error_t (*mem_unregister)(media_lib_session_t* session,
                                        mtl_dma_mem_handle* dma_mem);
    
    /* Zero-copy buffer operations - App-owned mode */
    media_lib_error_t (*post_rx)(media_lib_session_t* session,
                                 void* data,
                                 size_t size,
                                 void* app_ctx);
    media_lib_error_t (*post_tx)(media_lib_session_t* session,
                                 void* data,
                                 size_t size,
                                 void* app_ctx);
    
    /* Transmitter operations */
    media_lib_error_t (*transmit)(media_lib_session_t* session,
                                  media_lib_buffer_t* buffer,
                                  uint32_t timeout_ms);
    media_lib_error_t (*transmit_flush)(media_lib_session_t* session,
                                        uint32_t timeout_ms);
    
    /* Receiver operations */
    media_lib_error_t (*receive)(media_lib_session_t* session,
                                 media_lib_buffer_t** buffer,
                                 uint32_t timeout_ms);
    
    /* Polling interface */
    media_lib_error_t (*poll)(media_lib_session_t* session,
                              media_lib_event_t* event,
                              uint32_t timeout_ms);
    
    /* Statistics and monitoring */
    media_lib_error_t (*get_stats)(media_lib_session_t* session,
                                   void* stats);
    
} media_lib_session_vtable_t;

/*** Session Statistics ***/
typedef struct {
    uint64_t frames_processed;       // Total frames/buffers sent or received
    uint64_t bytes_processed;        // Total bytes transmitted or received
    uint64_t dropped_frames;         // Frames/buffers dropped due to errors
    double current_rate;             // Current effective frame/sample rate
    uint32_t total_buffers;          // Total number of buffers
    uint32_t buffers_in_use;         // Number of buffers currently in use
    uint64_t last_timestamp;         // Timestamp of the last processed frame/sample
    
    /* Queue statistics */
    uint32_t free_queue_depth;       // Number of buffers available for acquisition
    uint32_t transmit_queue_depth;   // Number of buffers pending transmission
    
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

inline media_lib_error_t media_lib_session_destroy(media_lib_session_t* session) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->destroy(session);
}

/* Buffer management - Library-owned mode */
inline media_lib_error_t media_lib_buffer_acquire(
    media_lib_session_t* session,
    media_lib_buffer_t** buffer,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->buffer_acquire(session, buffer, timeout_ms);
}

inline media_lib_error_t media_lib_buffer_release(
    media_lib_session_t* session,
    media_lib_buffer_t* buffer
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->buffer_release(session, buffer);
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
inline media_lib_error_t media_lib_post_rx(
    media_lib_session_t* session,
    void* data,
    size_t size,
    void* app_ctx
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->post_rx(session, data, size, app_ctx);
}

inline media_lib_error_t media_lib_post_tx(
    media_lib_session_t* session,
    void* data,
    size_t size,
    void* app_ctx
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->post_tx(session, data, size, app_ctx);
}

/* Transmitter operations */
inline media_lib_error_t media_lib_transmit(
    media_lib_session_t* session,
    media_lib_buffer_t* buffer,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->transmit(session, buffer, timeout_ms);
}

inline media_lib_error_t media_lib_transmit_flush(
    media_lib_session_t* session,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->transmit_flush(session, timeout_ms);
}

/* Receiver operations */
inline media_lib_error_t media_lib_receive(
    media_lib_session_t* session,
    media_lib_buffer_t** buffer,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->receive(session, buffer, timeout_ms);
}

/* Polling interface */
inline media_lib_error_t media_lib_poll(
    media_lib_session_t* session,
    media_lib_event_t* event,
    uint32_t timeout_ms
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->poll(session, event, timeout_ms);
}

/* Statistics and monitoring */
inline media_lib_error_t media_lib_session_get_stats(
    media_lib_session_t* session,
    media_lib_session_stats_t* stats
) {
    if (!session) return MEDIA_LIB_ERROR_INVALID_PARAMETER;
    media_lib_session_vtable_t* vtable = (media_lib_session_vtable_t*)session;
    return vtable->get_stats(session, stats);
}

#ifdef __cplusplus
}
#endif

#endif /* MEDIA_LIB_API_H */
