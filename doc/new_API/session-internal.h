// This is an internal definition, not exposed in the public header.
struct media_lib_session_base_t {
    // Must be the first field for polymorphism.
    media_lib_session_vtable_t* vtable;

    // Pointer to the parent library instance.
    media_lib_instance_t* instance;

    // Session configuration (common)
    media_lib_type_t media_type;               // MEDIA_LIB_TYPE_VIDEO or MEDIA_LIB_TYPE_AUDIO
    media_lib_session_type_t session_role;     // Receiver or transmitter
    media_lib_buffer_ownership_t ownership;    // Buffer ownership mode

    // Common session parameters (from base config)
    size_t buffer_size;                     
    uint32_t num_buffers;
    // etc.

    // Statistics for monitoring performance.
    media_lib_session_stats_t stats;
};

// Video session structure
typedef struct media_lib_video_session {
    // Base session must be the first member
    media_lib_session_base_t base;
    
    // Video-specific fields
    uint32_t width;
    uint32_t height;
    uint32_t framerate;
    uint32_t format;
    // Additional video-specific fields can be added here
} media_lib_video_session_t;

// Audio session structure
typedef struct media_lib_audio_session {
    // Base session must be the first member
    media_lib_session_base_t base;
    
    // Audio-specific fields
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bits_per_sample;
    uint32_t format;
    // Additional audio-specific fields can be added here
} media_lib_audio_session_t;