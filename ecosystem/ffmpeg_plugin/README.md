# The ffmpeg plugin for MTL

## 1. Build

### 1.1 Build openh264

```bash
git clone https://github.com/cisco/openh264.git -b openh264v2.4.0
cd openh264
make -j "$(nproc)"
sudo make install
sudo ldconfig
cd ../
```

### 1.2 Build ffmpeg with MTL patches

Note: $imtl_source_code should be pointed to top source code tree of Intel® Media Transport Library.

```bash
git clone https://github.com/FFmpeg/FFmpeg.git -b release/6.1
cd FFmpeg
# apply the build patch
git am $imtl_source_code/ecosystem/ffmpeg_plugin/0001-avdevice-add-mtl-in-out-dev-support.patch
# copy the mtl in/out implementation code
cp $imtl_source_code/ecosystem/ffmpeg_plugin/mtl_* -rf libavdevice/
# build with --enable-mtl, customize the option as your setup
./configure --enable-shared --disable-static --enable-nonfree --enable-pic --enable-gpl --enable-libopenh264 --enable-encoder=libopenh264 --enable-mtl
make -j "$(nproc)"
sudo make install
sudo ldconfig
```

## 2. Run

### 2.1 Input example

Single session input example, please start the st2110-20 10bit YUV422 tx stream on "239.168.85.20:20000" before using:

```bash
ffmpeg -total_sessions 1 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -framerate 59.94 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -payload_type 112 -f mtl -i "k" -vframes 2000 -f rawvideo /dev/null -y
```

Two sessions input example, one on "239.168.85.20:20000" and the second on "239.168.85.20:20002":

```bash
<!-- markdownlint-disable line-length -->
ffmpeg -total_sessions 2 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -framerate 59.94 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -payload_type 112 -f mtl -i "1" -port 0000:af:01.0 -rx_addr "239.168.85.20" -framerate 59.94 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20002 -payload_type 112 -f mtl -i "2" -map 0:0 -vframes 2000 -f rawvideo /dev/null -y -map 1:0 -vframes 2000 -f rawvideo /dev/null -y
```

Open-h264 encoder example on stream "239.168.85.20:20000":

```bash
ffmpeg -total_sessions 1 -port 0000:af:01.0 -local_addr "192.168.96.2" -rx_addr "239.168.85.20" -framerate 59.94 -pixel_format yuv422p10le -width 1920 -height 1080 -udp_port 20000 -payload_type 112 -f mtl -i "k" -vframes 2000 -c:v libopenh264 out.264 -y
```

### 2.2 Output example

One-session output example which reading from a yuv file and sending ST2110-20 stream to "239.168.85.20:20000":

```bash
ffmpeg -stream_loop -1 -video_size 1920x1080 -f rawvideo -pix_fmt yuv422p10le -i yuv422p10le_1080p.yuv -filter:v fps=59.94 -total_sessions 1 -port 0000:af:01.1 -local_addr "192.168.96.3" -tx_addr 239.168.85.20 -udp_port 20000 -payload_type 112 -f mtl -
```
