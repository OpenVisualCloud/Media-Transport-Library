# The ffmpeg plugin for MTL

## Notice

FFmpeg is an open source project licensed under LGPL and GPL. See <https://www.ffmpeg.org/legal.html>. You are solely responsible for determining if your use of FFmpeg requires any additional licenses. Intel is not responsible for obtaining any such licenses, nor liable for any licensing fees due, in connection with your use of FFmpeg.

## 1. Build

### 1.1 Build openh264

```bash
git clone https://github.com/cisco/openh264.git
cd openh264
git checkout openh264v2.4.0
make -j "$(nproc)"
sudo make install
sudo ldconfig
cd ../
```

### 1.2 Build ffmpeg with MTL patches

Note: $mtl_source_code should be pointed to top source code tree of Media Transport Library.

```bash
git clone https://github.com/FFmpeg/FFmpeg.git
cd FFmpeg
git checkout release/7.0
# apply the build patch
git am $mtl_source_code/ecosystem/ffmpeg_plugin/7.0/*.patch
# copy the mtl in/out implementation code
cp $mtl_source_code/ecosystem/ffmpeg_plugin/mtl_*.c -rf libavdevice/
cp $mtl_source_code/ecosystem/ffmpeg_plugin/mtl_*.h -rf libavdevice/
# build with --enable-mtl, customize the option as your setup
./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl
make -j "$(nproc)"
sudo make install
sudo ldconfig
```

Note, for ffmpeg 4.4 or 6.1 version, replace 7.0 with 4.4/6.1 for above example commands.

## 2. ST20P raw video run guide

The MTL ST20P plugin is implemented as an FFMpeg input/output device, enabling direct reading from or sending raw video via the ST2110-20 stream.

### 2.1 St20p input

Reading a st2110-20 10bit YUV422 stream on "239.168.85.20:20000" with payload_type 112:

```bash
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -f mtl_st20p -i "k" -f rawvideo /dev/null -y
```

Below error indicate MTL don't detect a video stream on the listening address.

```bash
[mtl_st20p @ 0x55634f8b3c80] mtl_st20p_read_packet(0), st20p_rx_get_frame timeout
[in#0/mtl_st20p @ 0x55634f8b3b40] Error during demuxing: Input/output error
[in#0/mtl_st20p @ 0x55634f8b3b40] Error retrieving a packet from demuxer: Input/output error
[vost#0:0/rawvideo @ 0x55634f8bcc80] No filtered frames for output stream, trying to initialize anyway.
```

Reading two st2110-20 10bit YUV422 stream, one on "239.168.85.20:20000" and the second on "239.168.85.20:20002":

```bash
<!-- markdownlint-disable line-length -->
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -f mtl_st20p -i "1" -p_port 0000:af:01.0 -p_rx_ip 239.168.85.20 -udp_port 20002 -payload_type 112 -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -f mtl_st20p -i "2" -map 0:0 -f rawvideo /dev/null -y -map 1:0 -f rawvideo /dev/null -y
```

Reading a st2110-20 10bit YUV422 stream on "239.168.85.20:20000" with payload_type 112, and use libopenh264 to encode the stream to out.264 file:

```bash
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -f mtl_st20p -i "k" -c:v libopenh264 out.264 -y
```

### 2.2 St20p output

Reading from a yuv stream from a local file and sending a st2110-20 10bit YUV422 stream on "239.168.85.20:20000" with payload_type 112:

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -filter:v fps=59.94 -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -f mtl_st20p -
```

## 3. ST22 compressed video run guide

A typical workflow for processing an MTL ST22 compressed stream with FFMpeg is outlined in the following steps: Initially, FFMpeg reads a YUV frame from the input source, then forwards the frame to a codec to encode the raw video into a compressed codec stream. Finally, the codec stream is sent to the MTL ST22 plugin.
The MTL ST22 plugin constructs the codec stream and transmits it as ST2110-22 RTP packets, adhering to the standard. In addition to the JPEG XS stream, the MTL ST22 plugin is capable of supporting various other common compressed codecs, including H264, H265, and HEVC, among others.

<div align="center">
<img src="ffmpeg_st22_flow.png" align="center" alt="Tasklet">
</div>

### 3.1 St22 output

Reading from a yuv stream from local source file, encode with h264 codec and sending a st2110-22 codestream on "239.168.85.20:20000" with payload_type 112:

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv420p -i yuv420p_1080p.yuv -filter:v fps=59.94 -c:v libopenh264 -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -f mtl_st22 -
```

### 3.2 St22 input

Reading a st2110-22 codestream on "239.168.85.20:20000" with payload_type 112, decode with ffmpeg h264 codec:

```bash
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -video_size 1920x1080 -st22_codec h264 -f mtl_st22 -i "k" -f rawvideo /dev/null -y
```

### 3.3 SVT-JPEGXS

Make sure the FFMpeg is build with both MTL and SVT-JPEGXS plugin:

```bash
# start rx
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -video_size 1920x1080 -st22_codec jpegxs -timeout_s 10 -f mtl_st22 -i "k" -vframes 10 -f rawvideo /dev/null -y
# start tx
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv420p -i yuv420p_1080p.yuv -filter:v fps=59.94 -c:v libsvt_jpegxs -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -f mtl_st22 -
```

### 3.4 SVT-HEVC

Make sure the FFMpeg is build with both MTL and SVT-HEVC plugin:

```bash
# start rx
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -video_size 1920x1080 -st22_codec h265 -timeout_s 10 -f mtl_st22 -i "k" -vframes 10 -f rawvideo /dev/null -y
# start tx
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv420p -i yuv420p_1080p.yuv -filter:v fps=59.94 -c:v libsvt_hevc -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -f mtl_st22 -
```

### 3.5 St22p support

Another option involves utilizing the MTL built-in ST22 codec plugin, where FFmpeg can directly send or retrieve the YUV raw frame to/from the MTL ST22P plugin. MTL will then internally decode or encode the codec stream.

Reading a st2110-22 pipeline jpegxs codestream on "239.168.85.20:20000" with payload_type 112:

```bash
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -st22_codec jpegxs -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -f mtl_st22p -i "k" -f rawvideo /dev/null -y
```

Reading from a yuv file and sending a st2110-22 pipeline jpegxs codestream on "239.168.85.20:20000" with payload_type 112:

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -filter:v fps=59.94 -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -st22_codec jpegxs -f mtl_st22p -
```

## 4. ST30P audio run guide

### 4.1 St30p input

Reading a st2110-30 stream(pcm24,1ms packet time,2 channels) on "239.168.85.20:30000" with payload_type 111 and encoded to a wav file:

```bash
ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 30000 -payload_type 111 -pcm_fmt pcm24 -at 1ms -ac 2 -f mtl_st30p -i "0" dump.wav -y
```

### 4.2 St30p output

Reading from a wav file and sending a st2110-30 stream(pcm24,1ms packet time,2 channels) on "239.168.85.20:30000" with payload_type 111:

```bash
ffmpeg -stream_loop -1 -i test.wav -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 30000 -payload_type 111 -at 1ms -f mtl_st30p -
```

### 4.3 St30p pcm16 example

For pcm16 audio, use `mtl_st30p_pcm16` muxer, set `pcm_fmt` to `pcm16` for demuxer.

```bash
ffmpeg -stream_loop -1 -i test.wav -p_port 0000:af:01.1 -p_sip 192.168.96.3 -p_tx_ip 239.168.85.20 -udp_port 30000 -payload_type 111 -at 1ms -f mtl_st30p_pcm16 -

ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 30000 -payload_type 111 -pcm_fmt pcm16 -at 1ms -ac 2 -f mtl_st30p -i "0" dump_pcm16.wav -y
```

### Enabling experimental MTL_GPU_DIRECT in FFmpeg with ST20p Support

The MTL_GPU_DIRECT experimental feature plans on enhancing FFmpeg's performance by allowing direct access to GPU memory, which can be particularly beneficial when working with high-throughput video streams such as those handled by the MTL ST20 codec plugin.

#### Building FFmpeg with MTL_GPU_DIRECT Enabled
To take advantage of the MTL_GPU_DIRECT feature, you must compile FFmpeg with this option enabled. Here’s how to do it:

```bash
./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl --extra-cflags="-DMTL_GPU_DIRECT_ENABLED"
```
or use
```bash
./build_ffmpeg_plugin.sh -g
```

Reading a st2110-20 10bit YUV422 stream on "239.168.85.20:20000" with payload_type 112 and
enabled gpu_direct:

```bash
./ffmpeg -p_port 0000:af:01.0 -p_sip 192.168.96.2 -p_rx_ip 239.168.85.20 -udp_port 20000 -payload_type 112 -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -gpu_direct 1 -gpu_driver 0 -gpu_device 0 -f mtl_st20p -i "k" -f rawvideo /dev/null -y
```

#### Additional Notes
GPU Direct Flag: When compiling FFmpeg with the MTL_GPU_DIRECT feature, ensure that your system's GPU drivers and hardware support direct GPU memory access.

[GPU Documentation](../docs/gpu.md)

By following these steps, you can effectively build and utilize FFmpeg with the MTL_GPU_DIRECT feature enabled.
