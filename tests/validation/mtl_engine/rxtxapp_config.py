# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

config_empty = {
    "tx_no_chain": False,
    "interfaces": [
        {
            "name": "",
            "ip": "",
        },
        {
            "name": "",
            "ip": "",
        },
    ],
    "tx_sessions": [
        {
            "dip": [""],
            "interface": [0],
            "video": [],
            "st20p": [],
            "st22p": [],
            "st30p": [],
            "audio": [],
            "ancillary": [],
            "st40p": [],
            "fastmetadata": [],
        },
    ],
    "rx_sessions": [
        {
            "ip": [""],
            "interface": [1],
            "video": [],
            "st20p": [],
            "st22p": [],
            "st30p": [],
            "audio": [],
            "ancillary": [],
            "st40p": [],
            "fastmetadata": [],
        },
    ],
}

config_empty_rx = {
    "interfaces": [
        {
            "name": "",
            "ip": "",
        },
    ],
    "rx_sessions": [
        {
            "ip": [""],
            "interface": [0],
            "video": [],
            "st20p": [],
            "st22p": [],
            "st30p": [],
            "audio": [],
            "ancillary": [],
            "st40p": [],
        },
    ],
}

config_empty_rx_rgb24_multiple = {
    "interfaces": [
        {
            "name": "",
            "ip": "",
        },
        {
            "name": "",
            "ip": "",
        },
    ],
    "rx_sessions": [
        {
            "ip": [""],
            "interface": [0],
            "video": [],
            "st20p": [],
            "st22p": [],
            "st30p": [],
            "audio": [],
            "ancillary": [],
            "st40p": [],
        },
        {
            "ip": [""],
            "interface": [1],
            "video": [],
            "st20p": [],
            "st22p": [],
            "st30p": [],
            "audio": [],
            "ancillary": [],
            "st40p": [],
        },
    ],
}

config_empty_tx = {
    "interfaces": [
        {
            "name": "",
            "ip": "",
        },
    ],
    "tx_sessions": [
        {
            "dip": [""],
            "interface": [0],
            "video": [],
            "st20p": [],
            "st22p": [],
            "st30p": [],
            "audio": [],
            "ancillary": [],
            "st40p": [],
        },
    ],
}

# video

config_tx_video_session = {
    "replicas": 1,
    "type": "frame",
    "pacing": "gap",
    "packing": "BPM",
    "start_port": 20000,
    "payload_type": 112,
    "tr_offset": "default",
    "video_format": "",
    "pg_format": "",
    "video_url": "",
}

config_rx_video_session = {
    "replicas": 1,
    "type": "frame",
    "pacing": "gap",
    "start_port": 20000,
    "payload_type": 112,
    "tr_offset": "default",
    "video_format": "",
    "pg_format": "",
    "display": False,
}

# st20p

config_tx_st20p_session = {
    "replicas": 1,
    "start_port": 20000,
    "payload_type": 112,
    "width": 1920,
    "height": 1080,
    "fps": "p60",
    "interlaced": False,
    "device": "AUTO",
    "pacing": "gap",
    "packing": "BPM",
    "input_format": "YUV422PLANAR10LE",
    "transport_format": "YUV_422_10bit",
    "st20p_url": "",
    "display": False,
    "enable_rtcp": False,
}

config_rx_st20p_session = {
    "replicas": 1,
    "start_port": 20000,
    "payload_type": 112,
    "width": 1920,
    "height": 1080,
    "fps": "p60",
    "interlaced": False,
    "device": "AUTO",
    "pacing": "gap",
    "packing": "BPM",
    "output_format": "YUV422PLANAR10LE",
    "transport_format": "YUV_422_10bit",
    "measure_latency": False,
    "display": False,
    "enable_rtcp": False,
    "st20p_url": "",
}

# st22p

config_tx_st22p_session = {
    "replicas": 1,
    "start_port": 20000,
    "payload_type": 114,
    "width": 1920,
    "height": 1080,
    "fps": "p25",
    "interlaced": False,
    "pack_type": "codestream",
    "codec": "JPEG-XS",
    "device": "AUTO",
    "quality": "speed",
    "input_format": "YUV422PLANAR10LE",
    "st22p_url": "",
    "codec_thread_count": 2,
    "enable_rtcp": False,
}

config_rx_st22p_session = {
    "replicas": 1,
    "start_port": 20000,
    "payload_type": 114,
    "width": 1920,
    "height": 1080,
    "fps": "p25",
    "interlaced": False,
    "pack_type": "codestream",
    "codec": "JPEG-XS",
    "device": "AUTO",
    "quality": "speed",
    "output_format": "YUV422PLANAR10LE",
    "display": False,
    "measure_latency": False,
    "codec_thread_count": 2,
    "enable_rtcp": False,
}

# st30p

config_tx_st30p_session = {
    "replicas": 1,
    "start_port": 30000,
    "payload_type": 111,
    "audio_format": "PCM24",
    "audio_channel": ["U02"],
    "audio_sampling": "96kHz",
    "audio_ptime": "1",
    "audio_url": "",
}

config_rx_st30p_session = {
    "replicas": 1,
    "start_port": 30000,
    "payload_type": 111,
    "audio_format": "PCM24",
    "audio_channel": ["U02"],
    "audio_sampling": "96kHz",
    "audio_ptime": "1",
}

# audio

config_tx_audio_session = {
    "replicas": 1,
    "type": "frame",
    "start_port": 30000,
    "payload_type": 111,
    "audio_format": "",
    "audio_channel": ["U02"],
    "audio_sampling": "48kHz",
    "audio_ptime": "1",
    "audio_url": "",
}

config_rx_audio_session = {
    "replicas": 1,
    "type": "frame",
    "start_port": 30000,
    "payload_type": 111,
    "audio_format": "",
    "audio_channel": ["U02"],
    "audio_sampling": "48kHz",
    "audio_ptime": "1",
    "audio_url": "",
}

# ancillary

config_tx_ancillary_session = {
    "replicas": 1,
    "start_port": 40000,
    "payload_type": 113,
    "type": "frame",
    "ancillary_format": "closed_caption",
    "ancillary_url": "",
    "ancillary_fps": "p59",
}

config_rx_ancillary_session = {
    "replicas": 1,
    "payload_type": 113,
    "start_port": 40000,
}

# st40p

config_tx_st40p_session = {
    "replicas": 1,
    "start_port": 40000,
    "payload_type": 113,
    "ancillary_fps": "p59",
    "interlaced": False,
    "ancillary_url": "",
    "user_pacing": False,
    "exact_user_pacing": False,
    "user_timestamp": False,
    "enable_rtcp": False,
    "test_mode": "none",
    "test_pkt_count": 0,
    "test_frame_count": 0,
    "redundant_delay_ns": 0,
}

config_rx_st40p_session = {
    "replicas": 1,
    "start_port": 40000,
    "payload_type": 113,
    "ancillary_fps": "p59",
    "interlaced": False,
    "ancillary_url": "",
    "user_pacing": False,
    "exact_user_pacing": False,
    "user_timestamp": False,
    "enable_rtcp": False,
    "test_mode": "none",
    "test_pkt_count": 0,
    "test_frame_count": 0,
    "redundant_delay_ns": 0,
    "reorder_window_ns": 0,
}

# st41

config_tx_st41_session = {
    "replicas": 1,
    "start_port": 40000,
    "payload_type": 115,
    "type": "frame",
    "fastmetadata_data_item_type": 1234567,
    "fastmetadata_k_bit": 0,
    "fastmetadata_fps": "p59",
    "fastmetadata_url": "",
}

config_rx_st41_session = {
    "replicas": 1,
    "payload_type": 115,
    "start_port": 40000,
    "fastmetadata_data_item_type": 1234567,
    "fastmetadata_k_bit": 0,
    "fastmetadata_url": "",
}

# performance tests

config_performance_empty = {
    "interfaces": [],
    "tx_sessions": [],
    "rx_sessions": [],
}

config_performance_tx_video_session = {
    "dip": [],
    "interface": [],
    "video": [
        {
            "replicas": 1,
            "type": "frame",
            "pacing": "gap",
            "packing": "BPM",
            "start_port": 20000,
            "payload_type": 112,
            "tr_offset": "default",
            "video_format": "",
            "pg_format": "",
            "video_url": "",
        }
    ],
}

config_performance_rx_video_session = {
    "ip": [],
    "interface": [],
    "video": [
        {
            "replicas": 1,
            "type": "frame",
            "pacing": "gap",
            "start_port": 20000,
            "payload_type": 112,
            "tr_offset": "default",
            "video_format": "",
            "pg_format": "",
            "display": False,
        }
    ],
}

config_performance_tx_st20p_session = {
    "dip": [],
    "interface": [],
    "st20p": [
        {
            "replicas": 1,
            "start_port": 20000,
            "payload_type": 112,
            "width": 1920,
            "height": 1080,
            "fps": "p50",
            "interlaced": False,
            "device": "AUTO",
            "pacing": "gap",
            "packing": "BPM",
            "input_format": "YUV422RFC4175PG2BE10",
            "transport_format": "YUV_422_10bit",
            "st20p_url": "",
            "display": False,
            "enable_rtcp": False,
        }
    ],
}

config_performance_rx_st20p_session = {
    "ip": [],
    "interface": [],
    "st20p": [
        {
            "replicas": 1,
            "start_port": 20000,
            "payload_type": 112,
            "width": 1920,
            "height": 1080,
            "fps": "p50",
            "interlaced": False,
            "device": "AUTO",
            "pacing": "gap",
            "packing": "BPM",
            "output_format": "YUV422RFC4175PG2BE10",
            "transport_format": "YUV_422_10bit",
            "measure_latency": False,
            "display": False,
            "enable_rtcp": False,
            "st20p_url": "",
        }
    ],
}
