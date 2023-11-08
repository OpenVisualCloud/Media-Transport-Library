# Docker guide

Docker guide for Intel® Media Transport Library

## 1. Build Docker image

```bash
docker build -t mtl:latest -f ubuntu.dockerfile ./
```

Refer to below build command if you are in a proxy env.

```bash
docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY=http://proxy.xxx.com:xxx --build-arg HTTPS_PROXY=https://proxy.xxx.com:xxx ./
```

## 2. DPDK NIC PMD and env setup on host

Follow [run guide](../doc/run.md) to setup the hugepages, driver of NIC PF, vfio driver mode for VFs.

## 3. Run and login into the docker container with root user

The sample usage provided below is enabled with specific privileged settings such as VFIO access, a shared IPC namespace and root user inside the docker.

### 3.1 Run the docker container

The argument `/dev/vfio/` enables the Docker instance to access the VFIO device.

The arguments `/dev/null, /tmp/kahawai_lcore.lock, and --ipc=host` and touch `/tmp/kahawai_lcore.lock` command are used for managing shared memory within IMTL, primarily for lcore management across multiple IMTL docker containers.

```bash
touch /tmp/kahawai_lcore.lock
docker run --privileged -it -v /dev/vfio/:/dev/vfio/ -v /dev/null:/dev/null -v /tmp/kahawai_lcore.lock:/tmp/kahawai_lcore.lock --ipc=host mtl:latest
```

If you confirm that all IMTL processes will run within a single Docker container, you can disregard the settings related to shared memory. Simply execute the following command:

```bash
docker run --privileged -it -v /dev/vfio/:/dev/vfio/  mtl:latest
```

### 3.2 Switch to the root user inside a Docker container

On the docker bash shell:

```bash
sudo -s
```

## 4. Run RxTXApp

```bash
cd Media-Transport-Library/
# Run below command to generate a fake yuv file or follow "#### 3.3 Prepare source files:" in [run guide](../doc/run.md)
# dd if=/dev/urandom of=test.yuv count=2160 bs=4800
# Edit and Run the loop json file
./build/app/RxTxApp --config_file tests/script/loop_json/1080p60_1v.json
```
