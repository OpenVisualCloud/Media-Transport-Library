# JSON Configuration Guide

Media Transport Library sample app can use JSON file to configure sessions. This documentation explains how to write the JSON files.

## Examples

Example `tx_1v_1a_1anc.json` file, find more examples config files in [example config directory](../config).

```json
{
    "interfaces": [
        {
            "name": "0000:af:01.0",
            "ip": "192.168.30.10"
        }
    ],
    "tx_sessions": [
        {
            "dip": [
                "239.1.1.1",
            ],
            "interface": [
                0,
            ],
            "st20p": [
                {
                    "replicas": 1,
                    "start_port": 20000,
                    "payload_type": 112,
                    "width": 1920,
                    "height": 1080,
                    "fps": "p59",
                    "device": "AUTO",
                    "input_format": "YUV422PLANAR10LE",
                    "transport_format": "YUV_422_10bit",
                    "st20p_url": "./yuv422p10le_1080p.yuv"
                }
            ],
            "st30p": [
                {
                    "replicas": 1,
                    "start_port": 30000,
                    "payload_type": 111,
                    "audio_format": "PCM24",
                    "audio_channel": ["U02"],
                    "audio_sampling": "96kHz",
                    "audio_ptime": "1",
                    "audio_url": "./test.pcm"
                }
            ],
            "ancillary": [
                {
                    "replicas": 1,
                    "start_port": 40000,
                    "payload_type": 113,
                    "type": "frame",
                    "ancillary_format": "closed_caption",
                    "ancillary_url": "./test.txt",
                    "ancillary_fps": "p59"
                }
            ],
            "fastmetadata": [
                {
                    "replicas": 1,
                    "start_port": 40000,
                    "payload_type": 115,
                    "type": "frame",
                    "fastmetadata_data_item_type": 123456,
                    "fastmetadata_k_bit": 1,
                    "fastmetadata_url": "./test.txt",
                    "fastmetadata_fps": "p59"
                }
            ]
        }
    ]
}
```

## Schema

### Interfaces

List all interfaces that can be used by app

​ **name (string):** PF/VF pci name, for example: 0000:86:00.0

​ **proto (string):** `"static", "dhcp"` interface network protocol, if DHCP is used, below IPs will be ignored

​ **ip (string):** interface assigned IP

​ **netmask (string):** interface netmask(optional), for example: 255.255.254.0

​ **gateway (string):** interface gateway(optional), for example: 172.16.10.1, use "route -n" to check the gateway address before binding port to DPDK PMD.

 **tx_queues_cnt (int):** the number of tx queues(optional). Default it will calculated based on the sessions number in this configuration.

 **rx_queues_cnt (int):** the number of tx queues(optional). Default it will calculated based on the sessions number in this configuration.

> Session group contains a group of sessions which use some common settings, there can be 0 or multiple video/audio/ancillary sessions in one group, and there can be 0 or multiple tx/rx session groups in the following parts in case you have multiple destination IP.
> See [tx_multi_dest](../config/test_tx_1port_2v_2dest.json) and [rx_multi_dest](../config/test_rx_1port_2v_2dest.json) for multiple destination reference.

### TX Sessions (array of tx session groups)

Common settings for following sessions in the group:

​ **dip (array-string):** destination IP address, at least 1 primary IP, the second is redundant IP

​ **interfaces (array-int):** interfaces/ports used by the sessions, at least 1 primary interface, the second is redundant interface

#### video (array of video sessions)

Items in each element of the "video" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **type (string):** `"frame", "rtp", "slice"` app->lib data type

​ **pacing (string):** `"narrow", "wide", "linear"` pacing type

​ **packing (string):** `"BPM", "GPM", "GPM_SL"` packing mode, default is "BPM" mode (1260 bytes in payload)

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **video_format (string):** `"i1080p59", "i1080p50", "i1080p29", "i720p59", "i720p50", "i720p29", "i2160p59", "i2160p50", "i2160p29"` video format

​ **pg_format (string):** `"YUV_422_10bit"`, `"YUV_422_12bit"`, `"YUV_444_10bit"`, `"YUV_444_12bit"`, `"RGB_10bit"`, `"RGB_12bit"` pixel group format

​ **video_url (string):** video source

​ **display (bool):** `true, false` display video frames with SDL, only works with YUV 422 10bit and 8bit stream

#### audio (array of audio sessions)

Items in each element of the "audio" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **type (string):** `"frame", "rtp"` app->lib data type

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **audio_format (string):** `"PCM8", "PCM16", "PCM24"` audio format

​ **audio_channel (array-string):** `"M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP", "U01...U64"` audio channel-order(a listing of these channel grouping symbols), the library only cares the total number of channels

​ **audio_sampling (string):** `"48kHz", "96kHz"` audio sample rate

​ **audio_ptime (string):** `"1", "0.12", "0.25", "0.33", "4"` audio packet time, AES67(st30) supported: 1ms, 4ms, 125us(0.12), 250us(0.25) and 333us(0.33), AM824(st31) supported: 1ms

​ **audio_url (string):** audio source

#### ancillary (array of ancillary sessions)

Items in each element of the "ancillary" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **ancillary_format (string):** `"closed_caption"` ancillary format

​ **ancillary_url (string):** ancillary source

​ **ancillary_fps (string):** `"p59", "p50", "p29"` ancillary fps which should be aligned to video

#### st40p (array of st40p pipeline sessions)

Alternative to the "ancillary" key above, using the pipeline API (`st40p_tx_create` / `st40p_rx_create`). The `"st40p"` key supports the same base fields plus additional test and pacing controls.

Items in each element of the "st40p" array:

​ **replicas (int):** `1~max_num` the number of session copies

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **ancillary_fps (string):** `"p59", "p50", "p29"` ancillary fps

​ **ancillary_url (string):** ancillary source file path (empty string for synthetic data)

​ **interlaced (bool):** `true, false` enable interlaced field cadence (optional, default false)

​ **user_pacing (bool):** `true, false` use application-provided timestamps for pacing (optional)

​ **exact_user_pacing (bool):** `true, false` strict user pacing (implies user_pacing, optional)

​ **user_timestamp (bool):** `true, false` use application-provided RTP timestamps (optional)

​ **enable_rtcp (bool):** `true, false` enable RTCP retransmission (optional)

​ **test_mode (string):** `"seq-gap", "no-marker", "bad-parity", "paced"` TX test mutation pattern (optional, TX only)

​ **test_pkt_count (int):** packet count for the test pattern schedule (optional, TX only)

​ **test_frame_count (int):** number of frames to apply the test pattern (optional, TX only; 65535 for continuous)

​ **redundant_delay_ns (int):** extra delay in nanoseconds before sending on the redundant port R (optional, TX only). Simulates path asymmetry for ST 2022-7 dejitter validation.

​ **reorder_window_ns (int):** max wait in nanoseconds for out-of-order packets before forcing frame advance (optional, RX only; default 10 ms = 10000000)

#### fast metadata (array of fast metadata sessions)

Items in each element of the "fastmetadata" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **type (string):** `"frame", "rtp"` app->lib data type

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **fastmetadata_data_item_type (int):** `0~4194303`  (0x - 0x3fffff) 22 bits data item type

​ **fastmetadata_k_bit (int):** `0~1` 1 bit K-bit value

​ **fastmetadata_url (string):** fast metadata source

 **fastmetadata_fps (string):** `"p59", "p50", "p29"` fast metadata fps which should be aligned to video

### RX Sessions (array of rx session groups)

Common settings for following sessions in the group:

​ **ip (array-string):** the transmitter's IP or the multicast group IP, at least 1 primary IP, the second is redundant IP

​ **mcast_src_ip (array-string):** the source IP filter for multicast(optional), assume primary and redundant sessions use different multicast group, if one of the source filter set, another is allowed to be set as 0.0.0.0(any source)

​ **interfaces (array-int):** interfaces/ports used by the sessions, at least 1 primary interface, the second is redundant interface

#### video (array of video sessions) for RX

Items in each element of the "video" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **type (string):** `"frame", "rtp", "slice"` lib->app data type

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **video_format (string):** `"i1080p59", "i1080p50", "i1080p29", "i720p59", "i720p50", "i720p29", "i2160p59", "i2160p50", "i2160p29"` video format

​ **pg_format (string):** `"YUV_422_10bit"`, `"YUV_422_12bit"`, `"YUV_444_10bit"`, `"YUV_444_12bit"`, `"RGB_10bit"`, `"RGB_12bit"` pixel group format

​ **user_pg_format (string):** `"YUV_422_8bit"` user required pixel group format

​ **display (bool):** `true, false` display video frames with SDL, only works with YUV 422 10bit and 8bit stream

#### audio (array of audio sessions) for RX

Items in each element of the "audio" array

​ **replicas (int):** `1~max_num` th number of session copies

​ **type (string):** `"frame", "rtp"` lib->app data type

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **audio_format (string):** `"PCM8", "PCM16", "PCM24"` audio format

​ **audio_channel (array-string):** `"M", "DM", "ST", "LtRt", "51", "71", "222", "SGRP", "U01...U64"` audio channel-order(a listing of these channel grouping symbols), the library only cares the total number of channels

​ **audio_sampling (string):** `"48kHz", "96kHz"` audio sample rate

​ **audio_ptime (string):** `"1", "0.12", "0.25", "0.33", "4"` audio packet time, AES67(st30) supported: 1ms, 4ms, 125us(0.12), 250us(0.25) and 333us(0.33), AM824(st31) supported: 1ms
> The maximum frame size that can be sent in the Media Transport Library (MTL) is
> 1440 bytes. This means that the combination of channel size, packet time (ptime),
> and audio channel should not exceed this limit.

​ **audio_url (string):** audio reference file

#### ancillary (array of ancillary sessions) for RX

Items in each element of the "ancillary" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

#### st40p (array of st40p pipeline sessions) for RX

Items in each element of the "st40p" array (pipeline RX counterpart to "ancillary"):

​ **replicas (int):** `1~max_num` the number of session copies

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **ancillary_fps (string):** `"p59", "p50", "p29"` ancillary fps

​ **ancillary_url (string):** (optional) reference file for RX output

​ **interlaced (bool):** `true, false` enable interlaced field cadence (optional)

​ **enable_rtcp (bool):** `true, false` enable RTCP retransmission (optional)

​ **reorder_window_ns (int):** max wait in nanoseconds for out-of-order packets before forcing frame advance (optional; default 10 ms). Increase this when testing with `redundant_delay_ns` on TX to accommodate path asymmetry.

#### fast metadata (array of fast metadata sessions) for RX

Items in each element of the "fastmetadata" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **fastmetadata_data_item_type (int):** `0~4194303`  (0x - 0x3fffff) 22 bits data item type - reference value (for testing the flow) - Optional setting

​ **fastmetadata_k_bit (int):** `0~1` 1 bit K-bit value - reference value (for testing the flow) - Optional setting

​ **fastmetadata_url (string):** fast metadata reference file (for testing the flow) - Optional setting

### Others

 **shared_tx_queues (bool):** If enable the shared tx queues or not, (optional). The queue number is limited for NIC, to support sessions more than queue number, enable this option to share queue resource between sessions.

 **shared_rx_queues (bool):** If enable the shared rx queues or not, (optional). The queue number is limited for NIC, to support sessions more than queue number, enable this option to share queue resource between sessions.

 **tx_no_chain (bool):** If disable the tx chain support or not, (optional). Tx chain is for zero copy support for audio and video transmitters with two different memory pool for header and payload. Use can enable this option to use copy mode to reduce the mempool usage, usually for audio sessions number max than 128.

 **sch_session_quota (int):** The quota unit in 1080p59 yuv422_10bit for one single scheduler(core), (optional). Default it will use the quota define in the lib.

 **rss_mode (string):** `"none", "l3_l4", "l3"` (optional). Default it will be detected by lib.

 **log_file (string):** set log file for mtl log. If you're initiating multiple RxTxApp processes simultaneously, please ensure each process has a unique filename path. Default the log is writing to stderr.
