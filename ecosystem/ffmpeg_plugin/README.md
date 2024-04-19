# The ffmpeg plugin for MTL

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

Note: $imtl_source_code should be pointed to top source code tree of Intel® Media Transport Library.

```bash
git clone https://github.com/FFmpeg/FFmpeg.git
cd FFmpeg
git checkout release/6.1
# apply the build patch
git am $imtl_source_code/ecosystem/ffmpeg_plugin/6.1/*.patch
# copy the mtl in/out implementation code
cp $imtl_source_code/ecosystem/ffmpeg_plugin/mtl_*.c -rf libavdevice/
cp $imtl_source_code/ecosystem/ffmpeg_plugin/mtl_*.h -rf libavdevice/
# build with --enable-mtl, customize the option as your setup
./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl
make -j "$(nproc)"
sudo make install
sudo ldconfig
```

Note, for ffmpeg 4.4 version, replace 6.1 with 4.4 for above example commands.

## 2. Run

### 2.1 St20p input example

Single session input example, please start the st2110-20 10bit YUV422 tx stream on "239.168.85.20:20000" before using:

```bash
ffmpeg -total_sessions 1 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -udp_port 20000 -payload_type 112 -f mtl_st20p -i "k" -vframes 2000 -f rawvideo /dev/null -y
```

Two sessions input example, one on "239.168.85.20:20000" and the second on "239.168.85.20:20002":

```bash
<!-- markdownlint-disable line-length -->
ffmpeg -total_sessions 2 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -udp_port 20000 -payload_type 112 -f mtl_st20p -i "1" -port 0000:af:01.0 -rx_addr "239.168.85.20" -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -udp_port 20002 -payload_type 112 -f mtl_st20p -i "2" -map 0:0 -vframes 2000 -f rawvideo /dev/null -y -map 1:0 -vframes 2000 -f rawvideo /dev/null -y
```

Open-h264 encoder example on stream "239.168.85.20:20000":

```bash
ffmpeg -total_sessions 1 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -udp_port 20000 -payload_type 112 -f mtl_st20p -i "k" -vframes 2000 -c:v libopenh264 out.264 -y
```

### 2.2 St20p output example

One-session output example which reading from a yuv file and sending ST2110-20 stream to "239.168.85.20:20000":

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -filter:v fps=59.94 -total_sessions 1 -port 0000:af:01.1 -local_addr "192.168.96.3" -tx_addr 239.168.85.20 -udp_port 20000 -payload_type 112 -f mtl_st20p -
```

### 2.3 St22p input example

Single session input example, please start the st2110-22 jpegxs tx stream on "239.168.85.20:20000" before using:

```bash
ffmpeg -total_sessions 1 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -fps 59.94 -pix_fmt yuv422p10le -video_size 1920x1080 -udp_port 20000 -payload_type 112 -f mtl_st22p -i "k" -vframes 2000 -f rawvideo /dev/null -y
```

### 2.4 St22p output example

One-session output example which reading from a yuv file and sending ST2110-22 stream to "239.168.85.20:20000":

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -filter:v fps=59.94 -total_sessions 1 -port 0000:af:01.1 -local_addr "192.168.96.3" -tx_addr 239.168.85.20 -udp_port 20000 -payload_type 112 -bpp 3 -f mtl_st22p -
```

### 2.5 St30p input example

Reading a st2110-30 stream(pcm24,1ms packet time,2 channels) on "239.168.85.20:30000" with payload_type 111 and encoded to a wav file:

```bash
ffmpeg -total_sessions 1 -pf pcm24 -at 1ms -ac 2 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -udp_port 30000 -payload_type 111 -f mtl_st30p -i "0" dump.wav -y
```

### 2.4 St30p output example

One-session output example which reading from a yuv file and sending ST2110-22 stream to "239.168.85.20:20000":

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -filter:v fps=59.94 -total_sessions 1 -port 0000:af:01.1 -local_addr "192.168.96.3" -tx_addr 239.168.85.20 -udp_port 20000 -payload_type 112 -bpp 3 -f mtl_st22p -
```
