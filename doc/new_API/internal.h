// This is an internal definition, not exposed in the public header.
struct media_lib_session {
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
    char address[256];          // Alternatively, store a pointer if dynamically allocated.
    uint16_t port;
    uint32_t timeout_ms;

    /* Media type-specific data */
    union {
        struct {
            /* Video-specific fields: width, height, format, etc. */
        } video;
        struct {
            /* Audio-specific fields: sample rate, channels, format, etc. */
        } audio;
    } media;

    // Callback functions and user data.
    media_lib_transmit_callback_t tx_callback;
    void* tx_callback_user_data;
    media_lib_receive_callback_t rx_callback;
    void* rx_callback_user_data;

    // Internal state for managing the session.
    // For example: buffer pool pointer, current state flags, connection handles, etc.
    void* internal_data;

    // Statistics for monitoring performance.
    media_lib_session_stats_t stats;
};