#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Forward declarations
typedef struct media_lib_session_base media_lib_session_t;
typedef struct media_lib_buffer_base media_lib_buffer_t;

// Base buffer structure
typedef struct media_lib_buffer_base {
    void* data;
    mtl_iova_t iova;
    size_t size;
    uint64_t timestamp;
    uint32_t flags;
    uint32_t buffer_id;
    void* _internal;
    void* user_data;
} media_lib_buffer_base_t;

// Video buffer structure
typedef struct media_lib_video_buffer {
    // Base buffer must be the first member
    media_lib_buffer_base_t base;
    
    // Video-specific fields
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t stride;
    // Additional video buffer fields
} media_lib_video_buffer_t;

// Audio buffer structure
typedef struct media_lib_audio_buffer {
    // Base buffer must be the first member
    media_lib_buffer_base_t base;
    
    // Audio-specific fields
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint32_t samples_per_frame;
    // Additional audio buffer fields
} media_lib_audio_buffer_t;

// Type casting macros for safe type conversion
#define MEDIA_LIB_SESSION_TO_VIDEO(session) \
    ((media_lib_video_session_t*)(session))

#define MEDIA_LIB_SESSION_TO_AUDIO(session) \
    ((media_lib_audio_session_t*)(session))

#define MEDIA_LIB_BUFFER_TO_VIDEO(buffer) \
    ((media_lib_video_buffer_t*)(buffer))

#define MEDIA_LIB_BUFFER_TO_AUDIO(buffer) \
    ((media_lib_audio_buffer_t*)(buffer))