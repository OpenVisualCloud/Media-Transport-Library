# St22 encode/decode plugin based on ffmpeg libavcodec

## 1. Install the build dependency.
#### 1.1 Ubuntu/Debian:
```bash
sudo apt-get install libavcodec-dev
```
#### 1.2 Centos:
```bash
sudo yum install libavcodec-devel
```
#### 1.3 Build directly from ffmpeg source code.
Follow the guide in ffmpeg website.

## 2. Build and install:
```bash
./script/build_st22_ffmpeg_plugin.sh
```

## 3. Test:

#### 3.1 Prepare a yuv422p8le file.
```bash
wget https://www.larmoire.info/jellyfish/media/jellyfish-3-mbps-hd-hevc.mkv
ffmpeg -i jellyfish-3-mbps-hd-hevc.mkv -vframes 3 -c:v rawvideo yuv420p8le.yuv
ffmpeg -s 1920x1080 -pix_fmt yuv420p -i yuv420p8le.yuv -pix_fmt yuv422p test_planar8.yuv
```

#### 3.2 Edit kahawai.json to enable the st22 ffmpeg plugin.
```bash
        {
            "enabled": 1,
            "name": "st22_ffmpeg",
            "path": "/usr/local/lib/x86_64-linux-gnu/libst_plugin_st22_ffmpeg.so"
        },
        {
            "enabled": 1,
            "name": "st22_ffmpeg",
            "path": "/usr/local/lib64/libst_plugin_st22_ffmpeg.so"
        },
```

#### 3.3 Prepare a yuv422p8le file.
Modify tx_st22_pipeline_sample.c and rx_st22_pipeline_sample.c to use h264 codec.
```bash
ops_tx.codec = ST22_CODEC_H264_CBR;
```
```bash
ops_rx.codec = ST22_CODEC_H264_CBR;
```
Build and run TxSt22PipelineSample, RxSt22PipelineSample.
```bash
ST_PORT_P=0000:af:00.1 ./build/app/TxSt22PipelineSample
```
```bash
ST_PORT_P=0000:af:00.0 ./build/app/RxSt22PipelineSample
```