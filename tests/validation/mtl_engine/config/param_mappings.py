# Parameter translation mappings for different applications
# Maps universal parameter names to application-specific names

# RxTxApp parameter mapping
RXTXAPP_PARAM_MAP = {
    # Network parameters
    "source_ip": "ip",
    "destination_ip": "dip",
    "multicast_ip": "ip",
    "port": "start_port",
    "nic_port": "name",
    
    # Video parameters  
    "width": "width",
    "height": "height",
    "framerate": "fps",
    "interlaced": "interlaced",
    "pixel_format": "input_format",      # for TX sessions
    "pixel_format_rx": "output_format",  # for RX sessions
    "transport_format": "transport_format",
    
    # Audio parameters
    "audio_format": "audio_format",
    "audio_channels": "audio_channel",
    "audio_sampling": "audio_sampling", 
    "audio_ptime": "audio_ptime",
    
    # Streaming parameters
    "payload_type": "payload_type",
    "replicas": "replicas",
    "pacing": "pacing",
    "packing": "packing",
    "device": "device",
    "codec": "codec",
    "quality": "quality",
    "codec_threads": "codec_thread_count",
    
    # File I/O
    "input_file": "st20p_url",      # for input files
    "output_file": "st20p_url",     # for output files (RX)
    "url": "video_url",             # for video files
    
    # Flags
    "enable_rtcp": "enable_rtcp",
    "measure_latency": "measure_latency", 
    "display": "display",
    
    # RxTxApp specific command-line parameters
    "config_file": "--config_file",
    "enable_ptp": "--ptp",
    "lcores": "--lcores",
    "test_time": "--test_time",
    "dma_dev": "--dma_dev",
    "log_level": "--log_level",
    "log_file": "--log_file",
    "arp_timeout_s": "--arp_timeout_s",
    "allow_across_numa_core": "--allow_across_numa_core",
    "no_multicast": "--no_multicast",
    "rx_separate_lcore": "--rx_separate_lcore",
    "rx_mix_lcore": "--rx_mix_lcore",
    "runtime_session": "--runtime_session",
    "rx_timing_parser": "--rx_timing_parser",
    "pcapng_dump": "--pcapng_dump",
    "rx_video_file_frames": "--rx_video_file_frames",
    "rx_video_fb_cnt": "--rx_video_fb_cnt",
    "promiscuous": "--promiscuous",
    "cni_thread": "--cni_thread",
    "sch_session_quota": "--sch_session_quota",
    "p_tx_dst_mac": "--p_tx_dst_mac",
    "r_tx_dst_mac": "--r_tx_dst_mac",
    "nb_tx_desc": "--nb_tx_desc",
    "nb_rx_desc": "--nb_rx_desc",
    "tasklet_time": "--tasklet_time",
    "tsc": "--tsc",
    "pacing_way": "--pacing_way",
    "shaping": "--shaping",
    "vrx": "--vrx",
    "ts_first_pkt": "--ts_first_pkt",
    "ts_delta_us": "--ts_delta_us",
    "mono_pool": "--mono_pool",
    "tasklet_thread": "--tasklet_thread",
    "tasklet_sleep": "--tasklet_sleep",
    "tasklet_sleep_us": "--tasklet_sleep_us",
    "app_bind_lcore": "--app_bind_lcore",
    "rxtx_simd_512": "--rxtx_simd_512",
    "rss_mode": "--rss_mode",
    "tx_no_chain": "--tx_no_chain",
    "multi_src_port": "--multi_src_port",
    "audio_fifo_size": "--audio_fifo_size",
    "dhcp": "--dhcp",
    "virtio_user": "--virtio_user",
    "phc2sys": "--phc2sys",
    "ptp_sync_sys": "--ptp_sync_sys",
    "rss_sch_nb": "--rss_sch_nb",
    "log_time_ms": "--log_time_ms",
    "rx_audio_dump_time_s": "--rx_audio_dump_time_s",
    "dedicated_sys_lcore": "--dedicated_sys_lcore",
    "bind_numa": "--bind_numa",
    "not_bind_numa": "--not_bind_numa",
    "force_numa": "--force_numa",
}

# FFmpeg parameter mapping
FFMPEG_PARAM_MAP = {
    # Network parameters
    "source_ip": "-p_sip",
    "destination_ip": "-p_tx_ip",      # for TX
    "multicast_ip": "-p_rx_ip",        # for RX
    "port": "-udp_port",
    "nic_port": "-p_port",
    
    # Video parameters
    "width": "-video_size",             # combined with height as "1920x1080"
    "height": "-video_size",            # combined with width as "1920x1080"
    "framerate": "-fps",
    "fps_numeric": "-filter:v",         # fps filter parameter
    "pixel_format": "-pix_fmt",
    "video_size": "-video_size",
    
    # Streaming parameters
    "payload_type": "-payload_type",
    "session_type": "-f",               # format specifier (automatically converted: st20p->mtl_st20p, etc.)
    
    # File I/O
    "input_file": "-i",
    "output_file": "",                  # output file is typically last argument
}

# GStreamer parameter mapping
GSTREAMER_PARAM_MAP = {
    # Network parameters  
    "source_ip": "dev-ip",
    "destination_ip": "ip",
    "port": "udp-port",
    "nic_port": "dev-port",
    
    # Video parameters
    "width": "rx-width",                # for RX pipeline
    "width_tx": "width",                # for caps in TX pipeline  
    "height": "rx-height",              # for RX pipeline
    "height_tx": "height",              # for caps in TX pipeline
    "framerate": "rx-fps",              # for RX pipeline
    "framerate_tx": "framerate",        # for caps in TX pipeline
    "pixel_format": "rx-pixel-format",
    "pixel_format_tx": "format",        # for caps in TX pipeline
    
    # Audio parameters
    "audio_format": "rx-audio-format",
    "audio_channels": "rx-channel", 
    "audio_sampling": "rx-sampling",
    
    # Streaming parameters
    "payload_type": "payload-type",
    "queues": "tx-queues",              # for TX
    "queues_rx": "rx-queues",           # for RX
    "framebuffer_count": "tx-framebuff-cnt",
    
    # File I/O
    "input_file": "location",           # for filesrc
    "output_file": "location",          # for filesink
}
