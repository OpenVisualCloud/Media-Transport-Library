@page js_config JSON Configuration Guide

# JSON Configuration Guide

Kahawai sample app can use json file to configure sessions. this documentation explains how to write the json files.

## Examples

Example `tx_multicast.json` file, find more example config file in `<kahawai>/config`:

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
                    "type": "frame",
                    "audio_format": "PCM16",
                    "audio_channel": "stereo",
                    "audio_sampling": "48kHz",
                    "audio_frametime_ms": 1,
                    "audio_url": "./test.wav"
                }
            ],
            "ancillary": [
                {
                    "replicas": 1,
                    "start_port": 40000,
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

​	**name (string):** PF/VF pci name, for example: 0000:86:00.0

​	**ip (string):** interface assigned IP


> Session group contains a group of sessions which use some common settings, there can be 0 or multiple video/audio/ancillary sessions in one group, and there can be 0 or multiple tx/rx session groups in the following parts in case you have multiple destination IP. (see [example config](../config/test_tx_1port_1v_2dest.json))

### TX Sessions (array of tx session groups)

Common settings for following sessions in the group:

​	**dip (array-string):** destination IP address, at least 1 primary IP, the second is redundant IP

​	**interfaces (array-int):** interfaces/ports used by the sessions, at least 1 primary interface, the second is redundant interface

#### video (array of video sessions)

Items in each element of the "video" array

​	**replicas (int):** `1~max_num` the number of session copies

​	**type (string):** `"frame", "rtp"` app->lib data type

​	**pacing (string):** `"gap", "linear"` pacing type

​	**start_port (int):** `0~65535` start udp port for copies of sessions

​	**tr_offset (string):** `"default", "none"` tr_offset for frame

​	**video_format (string):** `"i1080p59", "i1080p50", "i1080p29", "i720p59", "i720p50", "i720p29", "i2160p59", "i2160p50", "i2160p29"` video format

​	**pg_format (string):** `"YUV_422_10bit"` pixel group format

​	**video_url (string):** video source

#### audio (array of audio sessions)

Items in each element of the "audio" array

​	**replicas (int):** `1~max_num` the number of session copies

​	**type (string):** `"frame", "rtp"` app->lib data type

​	**start_port (int):** `0~65535` start udp port for copies of sessions

​	**audio_format (string):** `"PCM8", "PCM16", "PCM24" ` audio format

​	**audio_channel (string):** `"mono", "stereo"` audio channel

​	**audio_sampling (string):** `"48kHz", "96kHz"` audio sample rate

​	**audio_frametime_ms (int):** `1~max_time` audio sample time, default is 1ms

​	**audio_url (string):** audio source

#### ancillary (array of ancillary sessions)

Items in each element of the "ancillary" array

​	**replicas (int):** `1~max_num` the number of session copies

​	**start_port (int):** `0~65535` start udp port for copies of sessions

​	**ancillary_format (string):** `"closed_caption"` ancillary format

​	**ancillary_url (string):** ancillary source

   **ancillary_fps (string):** `"p59", "p50", "p29"`ancillary fps which should be aligned to video



### RX Sessions (array of rx session groups)

Common settings for following sessions in the group:

​	**ip (array-string):** the transmitter's IP or the multicast group IP, at least 1 primary IP, the second is redundant IP

​	**interfaces (array-int):** interfaces/ports used by the sessions, at least 1 primary interface, the second is redundant interface

#### video (array of video sessions)

Items in each element of the "video" array

​	**replicas (int):** `1~max_num` the number of session copies

​	**type (string):** `"frame", "rtp"` lib->app data type

​	**pacing (string):** `"gap", "linear"` pacing type

​	**start_port (int):** `0~65535` start udp port for copies of sessions

​	**tr_offset (string):** `"default", "none"` tr_offset for frame

​	**video_format (string):** `"i1080p59", "i1080p50", "i1080p29", "i720p59", "i720p50", "i720p29", "i2160p59", "i2160p50", "i2160p29"` video format

​	**pg_format (string):** `"YUV_422_10bit"` pixel group format

​	**display (bool):** `true, false` display video frames with SDL

#### audio (array of audio sessions)

Items in each element of the "audio" array

​	**replicas (int):** `1~max_num` th number of session copies

​	**type (string):** `"frame", "rtp"` lib->app data type

​	**start_port (int):** `0~65535` start udp port for copies of sessions

​	**audio_format (string):** `"PCM8", "PCM16", "PCM24" ` audio format

​	**audio_channel (string):** `"mono", "stereo"` audio channel

​	**audio_sampling (string):** `"48kHz", "96kHz"` audio sample rate

​	**audio_frametime_ms (int):** `1~max_time` audio sample time, default is 1ms

​	**audio_url (string):** audio reference file

#### ancillary (array of ancillary sessions)

Items in each element of the "ancillary" array

​	**replicas (int):** `1~max_num` the number of session copies

​	**start_port (int):** `0~65535` start udp port for copies of sessions