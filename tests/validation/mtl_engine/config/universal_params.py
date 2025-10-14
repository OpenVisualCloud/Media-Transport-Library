# Universal parameter definitions for all media applications
# This serves as the common interface for RxTxApp, FFmpeg, and GStreamer

UNIVERSAL_PARAMS = {
    # Network parameters
    "source_ip": None,              # Source IP address (interface IP)
    "destination_ip": None,         # Destination IP address (session IP)
    "multicast_ip": None,          # Multicast group IP
    "port": 20000,                 # UDP port number
    "nic_port": None,              # Network interface/port name
    "nic_port_list": None,         # List of network interfaces/ports for multi-interface configs
    
    # Video parameters
    "width": 1920,                 # Video width in pixels
    "height": 1080,                # Video height in pixels
    "framerate": "p60",            # Frame rate (p25, p30, p50, p60, etc.)
    "interlaced": False,           # Progressive (False) or Interlaced (True)
    "pixel_format": "YUV422PLANAR10LE",  # Pixel format for both TX (input) and RX (output)
    "transport_format": "YUV_422_10bit",  # Transport format for streaming
    # Removed: default_video_format – legacy video format mapping now handled directly where needed.
    
    # Audio parameters
    "audio_format": "PCM24",       # Audio format
    "audio_channels": ["U02"],     # Audio channel configuration
    "audio_sampling": "96kHz",     # Audio sampling rate
    "audio_ptime": "1",            # Audio packet time
    
    # Streaming parameters
    "payload_type": 112,           # RTP payload type
    "session_type": "st20p",       # Session type (st20p, st22p, st30p, video, audio, etc.)
    "direction": None,             # Direction: tx (transmit), rx (receive), or None (both for RxTxApp)
    "replicas": 1,                 # Number of session replicas
    # Removed: queues – queue count not plumbed through new builders; retaining calculation left to legacy code paths.
    "framebuffer_count": None,     # Frame buffer count (for RX video: rx_video_fb_cnt)
    
    # Quality and encoding parameters
    "pacing": "gap",               # Pacing mode (gap, auto, etc.)
    "packing": "BPM",              # Packing mode
    "device": "AUTO",              # Device selection
    "codec": "JPEG-XS",            # Codec for compressed formats
    "quality": "speed",            # Quality setting
    "codec_threads": 2,            # Number of codec threads
    
    # File I/O parameters
    "input_file": None,            # Input file path
    "output_file": None,           # Output file path
    # Removed: url – generic video_url not used in refactored path; specific st20p_url/audio_url populated directly.
    
    # Test configuration
    "test_mode": "multicast",      # Test mode (unicast, multicast, kernel)
    "test_time": 30,               # Test duration in seconds
    "enable_rtcp": False,          # Enable RTCP
    "measure_latency": False,      # Enable latency measurement
    "display": False,              # Enable display output
    "enable_ptp": False,           # Enable PTP synchronization
    "virtio_user": False,          # Enable virtio-user mode
    
    # RxTxApp specific parameters
    "config_file": None,           # JSON config file path
    "lcores": None,                # DPDK lcore list (e.g., "28,29,30,31")
    "dma_dev": None,               # DMA device list (e.g., "DMA1,DMA2,DMA3")
    "log_level": None,             # Log level (debug, info, notice, warning, error)
    "log_file": None,              # Log file path
    "arp_timeout_s": None,         # ARP timeout in seconds (default: 60)
    "allow_across_numa_core": False, # Allow cores across NUMA nodes
    "no_multicast": False,         # Disable multicast join message
    "rx_separate_lcore": False,    # RX video on dedicated lcores
    "rx_mix_lcore": False,         # Allow TX/RX video on same core
    "runtime_session": False,      # Start instance before creating sessions
    "rx_timing_parser": False,     # Enable timing check for video RX streams
    "pcapng_dump": None,           # Dump n packets to pcapng files
    "rx_video_file_frames": None,  # Dump received video frames to yuv file
    "promiscuous": False,          # Enable RX promiscuous mode
    "cni_thread": False,           # Use dedicated thread for CNI messages
    "sch_session_quota": None,     # Max sessions count per lcore
    "p_tx_dst_mac": None,          # Destination MAC for primary port
    "r_tx_dst_mac": None,          # Destination MAC for redundant port
    "nb_tx_desc": None,            # Number of TX descriptors per queue
    "nb_rx_desc": None,            # Number of RX descriptors per queue
    "tasklet_time": False,         # Enable tasklet running time stats
    "tsc": False,                  # Force TSC pacing
    "pacing_way": None,            # Pacing way (auto, rl, tsc, tsc_narrow, ptp, tsn)
    "shaping": None,               # ST21 shaping type (narrow, wide)
    "vrx": None,                   # ST21 vrx value
    "ts_first_pkt": False,         # Set RTP timestamp at first packet egress
    "ts_delta_us": None,           # RTP timestamp delta in microseconds
    "mono_pool": False,            # Use mono pool for all queues
    "tasklet_thread": False,       # Run tasklet under thread
    "tasklet_sleep": False,        # Enable sleep if tasklets report done
    "tasklet_sleep_us": None,      # Sleep microseconds value
    "app_bind_lcore": False,       # Run app thread on pinned lcore
    "rxtx_simd_512": False,        # Enable DPDK SIMD 512 path
    "rss_mode": None,              # RSS mode (l3_l4, l3, none)
    "tx_no_chain": False,          # Use memcopy instead of mbuf chain
    "multi_src_port": False,       # Use multiple source ports for ST20 TX
    "audio_fifo_size": None,       # Audio FIFO size
    "dhcp": False,                 # Enable DHCP for all ports
    "phc2sys": False,              # Enable built-in phc2sys function
    "ptp_sync_sys": False,         # Enable PTP to system time sync
    "rss_sch_nb": None,            # Number of schedulers for RSS dispatch
    "log_time_ms": False,          # Enable ms accuracy log printer
    "rx_audio_dump_time_s": None,  # Dump audio frames for n seconds
    "dedicated_sys_lcore": False,  # Run MTL system tasks on dedicated lcore
    "bind_numa": False,            # Bind all MTL threads to NIC NUMA (when False, threads run without NUMA awareness)
    "force_numa": None,            # Force NIC port NUMA ID
    
    # Execution control defaults (moved from hardcoded literals in engine code)
    "sleep_interval": 4,           # Delay between starting TX and RX in dual-host tests
    "tx_first": True,              # Whether to start TX side before RX
    # Removed: output_format – validation infers format from pixel_format; explicit label no longer required.
    "timeout_grace": 10,           # Extra seconds appended to process timeout wrapper
    "process_timeout_buffer": 90,  # Buffer added to test_time for run() timeout
    "pattern_duration": 30,        # Duration for generated test patterns (FFmpeg/GStreamer)
    "default_framerate_numeric": 60, # Fallback numeric framerate when parsing fails
}
