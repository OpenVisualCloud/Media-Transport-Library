# The ffmpeg plugin

## How To build

./build_ffmpeg_plugin.sh

## How to run

One-session input example:

```bash
ffmpeg -framerate 60 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -port 0000:31:00.0 -local_addr "192.168.96.2" -src_addr "239.168.85.20" -dma_dev "0000:00:01.0" -ext_frames_mode 1 -f kahawai -i "k" -vframes 2000 -f rawvideo /dev/null -y"
```

Two-sessions input example:

```bash
<!-- markdownlint-disable line-length -->
ffmpeg -framerate 60 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -port 0000:31:00.0 -local_addr "192.168.96.2" -src_addr "239.168.85.20" -total_sessions 2 -ext_frames_mode 1 -f kahawai -i "1" -framerate 60 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20001 -port 0000:31:00.0 -local_addr "192.168.96.3" -src_addr "239.168.85.20" -total_sessions 2 -ext_frames_mode 1 -f kahawai -i "2" -map 0:0 -vframes 5000 -f rawvideo /dev/null -y -map 1:0 -vframes 5000 -f rawvideo /dev/null -y
```

With openh264 encoder example:

```bash
ffmpeg -framerate 60 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -port 0000:31:00.0 -local_addr "192.168.96.2" -src_addr "239.168.85.20" -dma_dev "0000:00:01.0" -ext_frames_mode 1 -f kahawai -i "k" -vframes 2000 -c:v libopenh264 out.264 -y
```

One-session output example:

```bash
ffmpeg -video_size 512x512 -f rawvideo -pix_fmt rgb24 -i input.rgb -filter:v fps=60 -udp_port 20000 -port 0000:31:00.0 -local_addr 192.168.96.2 -dst_addr 239.168.85.20 -f kahawai_mux -
```

Input parameters description:

1. "framerate" supports 25, 30, 59, 60 and 120.
2. "pixel_format" supports yuv422p10le only for input now.
3. "f kahawai" is required to set to select Intel® Media Transport Library as the input device.
4. "i" can be set with anything for being a virtual device, no dev node.
5. "vframes" shall be set with the frame number to be read.
6. "udp_port port local_addr src_addr fb_cnt" definitions are the same as in sample.
7. "total_sessions" shall be set with the total number of rx sessions.
8. "ext_frames_mode" can be set to 1 (ext frames enabled) or 0 (disabled).
9. "dma_dev" can be set with a DMA device node on the same rx socket.

Output parameters description:

1. "f kahawai_mux" is required to select kahawai as the output device.
2. "udp_port port local_addr src_addr fb_cnt" definitions are the same as in sample.
3. "total_sessions" shall be set with the total number of tx sessions.

Known issue
1. Only RGB24 pixel format supported for output
2. Either input or output only operation could be enabled and kahawai transcoding is not supported
