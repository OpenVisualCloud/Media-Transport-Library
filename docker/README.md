# Docker guide
Docker guide for MTL

## 1. Build Docker image
```bash
docker build -t mtl:latest -f ubuntu.dockerfile ./
```
Refer to below build command if you are in a proxy env.
```bash
docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY=http://proxy.xxx.com:xxx --build-arg HTTPS_PROXY=https://proxy.xxx.com:xxx ./
```

## 2. DPDK NIC PMD and env setup
Please refer to [run guide](../doc/run.md)

## 3. Run the docker image
```bash
docker run --privileged -it -v /dev/vfio/vfio:/dev/vfio/vfio mtl:latest
```
non-root run need additional permission settings for the vfio and hugepage.

## 3. Run RxTXApp inside docker
```bash
cd Media-Transport-Library/
# Run below command to generate a fake yuv file or follow "#### 3.3 Prepare source files:" in [run guide](../doc/run.md)
# dd if=/dev/urandom of=test.yuv count=2160 bs=4800
# Edit and Run the loop json file
./build/app/RxTxApp --config_file tests/script/loop_json/1080p60_1v.json
```
