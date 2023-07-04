# JSON Configuration Guide

Intel® Media Transport Library sample app can use json file to configure sessions. this documentation explains how to write the json files.

## Examples

Example `tx_multicast.json` file, find more example config file in [example config](../config):

```json
{
    "interfaces": [
        {
            "name": "0000:86:00.0",
            "ip": "192.168.30.10"   
        }
    ],
    "tx_sessions": [
        {
            "dip": "239.1.1.1",
            "interface": [
                0
            ],
            "video": [
                {
                    "replicas": 1,
                    "type": "frame",
                    "pacing": "gap",
                    "start_port": 20000,
                    "payload_type": 112,
                    "tr_offset": "default",
                    "video_format": "i1080p59",
                    "pg_format": "YUV_422_10bit",
                    "video_url": "./test.yuv"
                }
            ],
            "audio": [
                {
                    "replicas": 1,
                    "start_port": 30000,
                    "payload_type": 111,
                    "type": "frame",
                    "audio_format": "PCM16",
                    "audio_channel": ["ST"],
                    "audio_sampling": "48kHz",
                    "audio_ptime": "1",
                    "audio_url": "./test.wav"
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

> Session group contains a group of sessions which use some common settings, there can be 0 or multiple video/audio/ancillary sessions in one group, and there can be 0 or multiple tx/rx session groups in the following parts in case you have multiple destination IP. (see [example config](../config/test_tx_1port_1v_2dest.json))

### TX Sessions (array of tx session groups)

Common settings for following sessions in the group:

​ **dip (array-string):** destination IP address, at least 1 primary IP, the second is redundant IP

​ **interfaces (array-int):** interfaces/ports used by the sessions, at least 1 primary interface, the second is redundant interface

#### video (array of video sessions)

Items in each element of the "video" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **type (string):** `"frame", "rtp", "slice"` app->lib data type

​ **pacing (string):** `"gap", "linear"` pacing type

​ **packing (string):** `"GPM_SL", "BPM", "GPM"` packing mode, default is "GPM_SL" single line mode

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **tr_offset (string):** `"default", "none"` tr_offset for frame

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

   **ancillary_fps (string):** `"p59", "p50", "p29"`ancillary fps which should be aligned to video

### RX Sessions (array of rx session groups)

Common settings for following sessions in the group:

​ **ip (array-string):** the transmitter's IP or the multicast group IP, at least 1 primary IP, the second is redundant IP

​ **interfaces (array-int):** interfaces/ports used by the sessions, at least 1 primary interface, the second is redundant interface

#### video (array of video sessions) for RX

Items in each element of the "video" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **type (string):** `"frame", "rtp", "slice"` lib->app data type

​ **pacing (string):** `"gap", "linear"` pacing type

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

​ **tr_offset (string):** `"default", "none"` tr_offset for frame

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

​ **audio_url (string):** audio reference file

#### ancillary (array of ancillary sessions) for RX

Items in each element of the "ancillary" array

​ **replicas (int):** `1~max_num` the number of session copies

​ **start_port (int):** `0~65535` start udp port for copies of sessions

​ **payload_type (int):** `0~127` 7 bits payload type define in RFC3550

### Others

 **shared_tx_queues (bool):** If enable the shared tx queues or not, (optional). The queue number is limited for NIC, to support sessions more than queue number, enable this option to share queue resource between sessions.

 **shared_rx_queues (bool):** If enable the shared rx queues or not, (optional). The queue number is limited for NIC, to support sessions more than queue number, enable this option to share queue resource between sessions.

 **tx_no_chain (bool):** If disable the tx chain support or not, (optional). Tx chain is for zero copy support for audio and video transmitters with two different memory pool for header and payload. Use can enable this option to use copy mode to reduce the mempool usage, usually for audio sessions number max than 128.

 **sch_session_quota (int):** The quota unit in 1080p59 yuv422_10bit for one single scheduler(core), (optional). Default it will use the quota define in the lib.

 **rss_mode (string):** `"none", "l3_l4", "l3"` (optional). Default it will be detected by lib.
