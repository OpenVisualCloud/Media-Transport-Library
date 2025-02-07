# St22 Encode / Decode Plugin Based on Ffmpeg libavcodec

## 1. Install the build dependency

### 1.1. Ubuntu/Debian

```bash
sudo apt-get install libavcodec-dev
```

### 1.2. CentOS

```bash
sudo yum install libavcodec-devel
```

Another options is build directly from ffmpeg source code

Follow the guide in ffmpeg site.

## 2. Build and install

```bash
./script/build_st22_avcodec_plugin.sh
```

## 3. Test

### 3.1. Prepare a yuv420p file

```bash
wget https://larmoire.org/jellyfish/media/jellyfish-3-mbps-hd-hevc.mkv
ffmpeg -i jellyfish-3-mbps-hd-hevc.mkv -vframes 150 -c:v rawvideo yuv420p_1080p.yuv
ffmpeg -s 1920x1080 -pix_fmt yuv420p -i yuv420p_1080p.yuv -pix_fmt yuv422p yuv422p_1080p.yuv
```

### 3.2. Edit kahawai.json to enable the st22 avcodec plugin

```json
        {
            "enabled": 1,
            "name": "st22_avcodec",
            "path": "/usr/local/lib/x86_64-linux-gnu/libst_plugin_st22_avcodec.so"
        },
        {
            "enabled": 1,
            "name": "st22_avcodec",
            "path": "/usr/local/lib64/libst_plugin_st22_avcodec.so"
        },
```

### 3.3. Run the sample with tx and rx based on h264

Customize the p_port as the setup.

Tx and RX run with kernel loopback:

```bash
python3 python/example/st22p_tx.py --p_port kernel:lo --p_tx_ip 127.0.0.1 --tx_url yuv420p_1080p.yuv --pipeline_fmt YUV420PLANAR8 --st22_codec h264 --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --display --display_scale_factor 4
```

```bash
python3 python/example/st22p_rx.py --p_port kernel:lo --p_rx_ip 127.0.0.1 --pipeline_fmt YUV420PLANAR8 --st22_codec h264 --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --display --display_scale_factor 4
```

Tx and RX run with dpdk port:

```bash
python3 python/example/st22p_tx.py --p_port 0000:af:01.0 --p_sip 192.168.108.101 --p_tx_ip 239.168.85.20 --tx_url yuv420p_1080p.yuv --pipeline_fmt YUV420PLANAR8 --st22_codec h264 --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --display --display_scale_factor 4
```

```bash
python3 python/example/st22p_rx.py --p_port 0000:af:01.1 --p_sip 192.168.108.102 --p_rx_ip 239.168.85.20 --pipeline_fmt YUV420PLANAR8 --st22_codec h264 --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --display --display_scale_factor 4
```
