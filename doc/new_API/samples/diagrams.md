# Sequence Diagrams for Unified Session API

## Library-Owned RX Flow

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as mtl_session_t
    participant Network as Network I/O

    Note over App,Network: Library-Owned Receiver (sample-rx-lib-owned.c)
    
    App->>Session: mtl_video_session_create(mt, &config, &session)
    Session->>Session: Allocate frame buffers
    Session->>Network: Setup receiver
    Session-->>App: Return session handle
    
    App->>Session: mtl_session_start(session)
    Session->>Network: Start receiving
    
    loop Receive and process
        App->>Session: mtl_session_buffer_get(session, &buffer, timeout)
        Session->>Network: Wait for frame
        Network-->>Session: Frame received
        Session-->>App: Return mtl_buffer_t*
        
        Note over App: Process buffer->data
        
        App->>Session: mtl_session_buffer_put(session, buffer)
        Session->>Network: Return buffer to receive queue
    end
    
    App->>Session: mtl_session_shutdown(session)
    App->>Session: mtl_session_destroy(session)
```

## Library-Owned TX Flow

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as mtl_session_t
    participant Network as Network I/O
    
    Note over App,Network: Library-Owned Transmitter (sample-tx-lib-owned.c)
    
    App->>Session: mtl_video_session_create(mt, &config, &session)
    Session->>Session: Allocate frame buffers
    Session->>Network: Setup transmitter
    Session-->>App: Return session handle
    
    App->>Session: mtl_session_start(session)
    
    loop Fill and transmit
        App->>Session: mtl_session_buffer_get(session, &buffer, timeout)
        Session-->>App: Return empty buffer
        
        Note over App: Fill buffer->data with frame
        
        App->>Session: mtl_session_buffer_put(session, buffer)
        Session->>Network: Transmit frame
        Network-->>Session: Transmission complete
        Session->>Session: Mark buffer available
    end
    
    App->>Session: mtl_session_shutdown(session)
    App->>Session: mtl_session_destroy(session)
```

## User-Owned RX Flow (Zero-Copy)

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as mtl_session_t
    participant DMA as DMA Manager
    participant Network as Network I/O
    
    Note over App,Network: User-Owned Receiver (sample-rx-app-owned.c)
    
    App->>Session: mtl_video_session_create(mt, &config, &session)
    Session-->>App: Return session handle
    
    App->>App: aligned_alloc() for buffers
    App->>Session: mtl_session_mem_register(session, memory, size, &handle)
    Session->>DMA: Register for DMA access
    Session-->>App: Return DMA handle
    
    loop Post all buffers
        App->>Session: mtl_session_buffer_post(session, buf->data, size, ctx)
        Session->>Network: Add buffer to receive queue
    end
    
    App->>Session: mtl_session_start(session)
    
    loop Poll and repost
        App->>Session: mtl_session_event_poll(session, &event, timeout)
        Network-->>Session: Frame received
        Session-->>App: MTL_EVENT_BUFFER_READY + context
        
        Note over App: Process data at ctx->data
        
        App->>Session: mtl_session_buffer_post(session, ctx->data, size, ctx)
        Session->>Network: Repost buffer
    end
    
    App->>Session: mtl_session_mem_unregister(session, handle)
    App->>Session: mtl_session_shutdown(session)
    App->>Session: mtl_session_destroy(session)
```

## User-Owned TX Flow (Zero-Copy)

```mermaid
sequenceDiagram
    participant Producer as Producer Thread
    participant EventHandler as Event Thread
    participant Session as mtl_session_t
    participant Network as Network I/O
    
    Note over Producer,Network: User-Owned Transmitter (sample-tx-app-owned.c)
    
    Note over Session: Setup: create session, register memory, start
    
    loop Producer Thread
        Producer->>Producer: Wait for free buffer
        Note over Producer: Fill buffer with frame data
        Producer->>Session: mtl_session_buffer_post(session, data, size, ctx)
        Session->>Network: Queue for transmission
    end
    
    loop Event Thread  
        EventHandler->>Session: mtl_session_event_poll(session, &event, timeout)
        Network-->>Session: Transmission complete
        Session-->>EventHandler: MTL_EVENT_BUFFER_DONE + context
        EventHandler->>EventHandler: Mark buffer as free
    end
```

## Polymorphic Session - Same API for All Media Types

```mermaid
flowchart TB
    subgraph Creation ["Type-Specific Creation"]
        VC[mtl_video_session_create]
        AC[mtl_audio_session_create]
        NC[mtl_ancillary_session_create]
    end
    
    subgraph Session ["Unified mtl_session_t"]
        S[session handle]
    end
    
    subgraph Operations ["Polymorphic Operations<br/>(Same for ALL media types)"]
        START[mtl_session_start]
        GET[mtl_session_buffer_get]
        PUT[mtl_session_buffer_put]
        POST[mtl_session_buffer_post]
        POLL[mtl_session_event_poll]
        STOP[mtl_session_stop]
        DESTROY[mtl_session_destroy]
    end
    
    VC --> S
    AC --> S
    NC --> S
    
    S --> START
    S --> GET
    S --> PUT
    S --> POST
    S --> POLL
    S --> STOP
    S --> DESTROY
```

## Slice-Level TX Flow (Ultra-Low Latency)

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as mtl_session_t
    participant Network as Network I/O
    
    Note over App,Network: Slice Mode TX (sample-tx-slice-mode.c)
    
    App->>Session: mtl_video_session_create(config with mode=SLICE)
    Session-->>App: Return session handle
    
    App->>Session: mtl_session_start(session)
    
    loop Frame Loop
        App->>Session: mtl_session_buffer_get(session, &buffer, timeout)
        Session-->>App: Return empty buffer
        
        loop Line by Line
            Note over App: Fill line N with video data
            App->>Session: mtl_session_slice_ready(session, buffer, lines=N+1)
            Session->>Network: Transmit line N immediately
            Note over Network: Wire latency ~1 line time!
        end
        
        App->>Session: mtl_session_buffer_put(session, buffer)
        Note over Session: Frame complete
    end
```

## Slice-Level RX Flow (Ultra-Low Latency)

```mermaid
sequenceDiagram
    participant Network as Network I/O
    participant Session as mtl_session_t
    participant App as Application
    
    Note over Network,App: Slice Mode RX (sample-rx-slice-mode.c)
    
    App->>Session: mtl_video_session_create(config with mode=SLICE)
    App->>Session: mtl_session_start(session)
    
    loop Frame Reception
        App->>Session: mtl_session_buffer_get(session, &buffer, timeout)
        
        loop As Packets Arrive
            Network-->>Session: RTP packets (lines 0-N)
            Session-->>App: MTL_EVENT_SLICE_READY (lines_ready=N+1)
            Note over App: Process lines immediately
            Note over App: Don't wait for full frame!
        end
        
        Network-->>Session: Final packets
        Session-->>App: MTL_EVENT_BUFFER_READY (frame complete)
        App->>Session: mtl_session_buffer_put(session, buffer)
    end
```

## ST22 Plugin Flow

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as mtl_session_t
    participant Plugin as JPEGXS Plugin
    participant Network as Network I/O
    
    Note over App,Network: ST22 TX (sample-tx-st22-plugin.c)
    
    Note over Plugin: Plugin registered at mtl_init time
    
    App->>Session: mtl_video_session_create(config with compressed=true)
    Session->>Plugin: Create encoder context
    Session-->>App: Return session handle
    
    App->>Session: mtl_session_get_plugin_info(&info)
    Session-->>App: Plugin name, version, device type
    
    App->>Session: mtl_session_start(session)
    
    loop Encode and Transmit
        App->>Session: mtl_session_buffer_get(session, &buffer, timeout)
        Session-->>App: Return buffer for RAW video
        
        Note over App: Fill with uncompressed frame
        
        App->>Session: mtl_session_buffer_put(session, buffer)
        Session->>Plugin: Encode frame (JPEGXS)
        Plugin-->>Session: Compressed codestream
        Session->>Network: Transmit ST22 packets
    end
```