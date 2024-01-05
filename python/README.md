# The Python support

IMTL leverage SWIG, found at <https://github.com/swig/swig/tree/master>, to transform C APIs into a binding layer that Python can utilize.

Before using the Python binding, please ensure that IMTL is [built](../doc/build.md) and the NIC is [set up](../doc/run.md) correctly.

## 1. Installation of SWIG Dependency

It is recommended to install the SWIG dependency from your operating system's software repository.

For Ubuntu:

```bash
sudo apt-get install swig
```

For Centos:

```bash
sudo yum install swig
```

If you encounter issues installing SWIG through your operating system's package manager, follow the build and installation guide from the SWIG GitHub repository at <https://github.com/swig/swig/tree/master>.

Below are the example steps to build the `v4.1.1` release. Replace the tag with a newer one if there is a more recent release of SWIG available:

```bash
git clone https://github.com/swig/swig.git
cd swig/
git checkout v4.1.1
./autogen.sh
./configure
make
sudo make install
```

## 2. Build and install IMTL python binding layer

### 2.1 Create IMTL binding layer code based on swig

```bash
cd $imtl_source_code/python/swig/
swig -python -I/usr/local/include -o pymtl_wrap.c pymtl.i
```

If you encounter the error `pymtl.i:15: Error: Unable to find 'mtl/mtl_api.h'`, this is typically due to an incorrect include path. Use the following command to locate the correct path for `mtl_api.h`:

```bash
find /usr/ -name mtl_api.h
```

Once you have obtained the correct path, you can update your SWIG interface file or your build configuration to reference the correct location of `mtl_api.h`.

### 2.2 Build

```bash
python3 setup.py build_ext --inplace
```

### 2.3 Install

```bash
sudo python3 setup.py install
```

Checking the log to see the path installed.

```bash
creating /usr/local/lib/python3.10/dist-packages/pymtl-0.1-py3.10-linux-x86_64.egg
Extracting pymtl-0.1-py3.10-linux-x86_64.egg to /usr/local/lib/python3.10/dist-packages
```

## 3. Run python example code

Install `opencv-python` dependency:

```bash
# for yuv display
sudo pip3 install opencv-python
# PyAv for video decode/encode
sudo pip3 install av
```

### 3.1 st20p_tx.py

Run the `st20p_tx.py`, which reads YUV video data from a file and transmits it over the network as a ST2110 ST_FRAME_FMT_YUV422RFC4175PG2BE10 stream.

```bash
python3 python/example/st20p_tx.py --p_port 0000:ac:01.0 --p_sip 192.168.108.101 --p_tx_ip 239.168.85.20 --tx_url yuv422p10le_1080p.yuv --pipeline_fmt YUV422PLANAR10LE --width 1920 --height 1080 --udp_port 20000 --payload_type 112
```

### 3.2 st20p_rx.py

Execute the `st20p_rx.py` to receive a ST2110 ST_FRAME_FMT_YUV422RFC4175PG2BE10 stream and display it.

```bash
python3 python/example/st20p_rx.py --p_port 0000:ac:01.1 --p_sip 192.168.108.102 --p_rx_ip 239.168.85.20 --pipeline_fmt YUV422PLANAR10LE --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --display
```

### 3.3 st20p_tx_decode.py

Use `st20p_tx_decode.py` to decode YUV video from an encoded file `jellyfish-3-mbps-hd-hevc-10bit.mkv` and transmit it as a ST2110 ST_FRAME_FMT_YUV422RFC4175PG2BE10 stream across the network.

```bash
python3 python/example/st20p_tx_decode.py --p_port 0000:ac:01.0 --p_sip 192.168.108.101 --p_tx_ip 239.168.85.20 --tx_url jellyfish-3-mbps-hd-hevc-10bit.mkv --udp_port 20000 --payload_type 112
```

### 3.4 st20p_rx_encode.py

Run the `st20p_rx_encode.py` to receive a ST2110 ST_FRAME_FMT_YUV422RFC4175PG2BE10 stream and encode it to a `.mp4` encoder file.

```bash
python3 python/example/st20p_rx_encode.py --p_port 0000:ac:01.1 --p_sip 192.168.108.102 --p_rx_ip 239.168.85.20 --rx_url test.mp4 --width 1920 --height 1080 --udp_port 20000 --payload_type 112
```

### 3.5 st22p_tx.py

Run the `st22p_tx.py`, which reads YUV video data from a file and transmits it over the network as a compressed ST2110-22 stream.

```bash
python3 python/example/st22p_tx.py --p_port 0000:ac:01.0 --p_sip 192.168.108.101 --p_tx_ip 239.168.85.20 --tx_url yuv422p10le_1080p.yuv --pipeline_fmt YUV422PLANAR10LE --st22_codec jpegxs --width 1920 --height 1080 --udp_port 20000 --payload_type 112
```

### 3.6 st22p_rx.py

Execute the `st22p_rx.py` to receive a compressed ST2110-22 stream stream and display it.

```bash
python3 python/example/st22p_rx.py --p_port 0000:ac:01.1 --p_sip 192.168.108.102 --p_rx_ip 239.168.85.20 --pipeline_fmt YUV422PLANAR10LE --st22_codec jpegxs --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --display
```

### 3.7 interlaced

For TX, interlaced yuv file is used and `--interlaced` is enabled.

st20p_tx:

```bash
python3 python/example/st20p_tx.py --p_port 0000:ac:01.0 --p_sip 192.168.108.101 --p_tx_ip 239.168.85.20 --tx_url yuv422p10le_1080i.yuv --pipeline_fmt YUV422PLANAR10LE --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --interlaced
```

st22p_tx:

```bash
python3 python/example/st22p_tx.py --p_port 0000:ac:01.0 --p_sip 192.168.108.101 --p_tx_ip 239.168.85.20 --tx_url yuv422p10le_1080i.yuv --pipeline_fmt YUV422PLANAR10LE --st22_codec jpegxs --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --interlaced
```

For RX, `--interlaced` is enabled.

st20p_rx:

```bash
python3 python/example/st20p_rx.py --p_port 0000:ac:01.1 --p_sip 192.168.108.102 --p_rx_ip 239.168.85.20 --pipeline_fmt YUV422PLANAR10LE --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --interlaced --display
```

st22p_rx:

```bash
python3 python/example/st22p_rx.py --p_port 0000:ac:01.1 --p_sip 192.168.108.102 --p_rx_ip 239.168.85.20 --pipeline_fmt YUV422PLANAR10LE --st22_codec jpegxs --width 1920 --height 1080 --udp_port 20000 --payload_type 112 --interlaced --display
```
