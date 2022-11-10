# The kahawai ffmpeg plugin

## How To build:

./build_ffmpeg_plugin.sh

## How to run:

1. An example: ffmpeg -framerate 60 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -port 0000:31:00.0 -local_addr "192.168.96.2" -src_addr "239.168.85.20" -f kahawai -i "k" -vframes 2000 -f rawvideo /dev/null -y"
2. The parameter "framerate" supports 25, 30, 59, 60 and 120.
3. The parameter "pixel_format" supports yuv422p10le only for now.
4. The parameter "f kahawai" is required to select kahawai as the input device.
5. The parameter "i" can be set with anything for kahawai being a virtual device.
6. The parameter "vframes" shall be set with the frame number to be read.
7. The parameter "udp_port port local_addr src_addr fb_cnt" definitions are the same as the ones in rx_st20_pipeline_sample.
