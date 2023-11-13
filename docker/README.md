# Docker guide

Docker guide for Intel® Media Transport Library.

## 1. Build Docker image

```bash
docker build -t mtl:latest -f ubuntu.dockerfile ./
```

Refer to below build command if you are in a proxy env.

```bash
http_proxy=http://proxy.xxx.com:xxx
https_proxy=https://proxy.xxx.com:xxx
docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY=$http_proxy --build-arg HTTPS_PROXY=$https_proxy ./
```

## 2. DPDK NIC PMD and env setup on host

Follow [run guide](../doc/run.md) to setup the hugepages, driver of NIC PF, vfio driver mode for VFs.

## 3. Run and login into the docker container with root user

The sample usage provided below is enabled with specific privileged settings such as VFIO access, a shared IPC namespace and root user inside the docker.

### 3.1 Run the docker container

#### 3.1.1 Run multiple docker container with SHM requirement

```bash
touch /tmp/kahawai_lcore.lock
docker run -it \
  --device /dev/vfio \
  --cap-add SYS_NICE \
  --cap-add IPC_LOCK \
  --cap-add NET_ADMIN \
  --cap-add SYS_TIME \
  --cap-add NET_RAW \
  -v /tmp/kahawai_lcore.lock:/tmp/kahawai_lcore.lock \
  -v /dev/null:/dev/null \
  --ipc=host \
  mtl:latest
```

Explanation of Docker arguments:

| Argument | Description |
| --- | --- |
| `--device /dev/vfio` | Access the VFIO device |
| `--cap-add SYS_NICE` | For set_mempolicy |
| `--cap-add IPC_LOCK` | For DMA mapping |
| `--cap-add NET_ADMIN` | Optional, for kernel NIC configuration |
| `--cap-add SYS_TIME` | Optional, for systime adjustment |
| `--cap-add NET_RAW` | Optional, for AF_XDP socket |
| `-v /tmp/kahawai_lcore.lock:/tmp/kahawai_lcore.lock` | For multiple instances lcore management |
| `-v /dev/null:/dev/null` | For multiple instances lcore management |
| `--ipc=host` | For multiple instances lcore management |

#### 3.1.2 Run single docker container

If you confirm that all IMTL processes will run within a single Docker container, you can disregard the settings related to shared memory. Simply execute the following command:

```bash
docker run -it \
  --device /dev/vfio \
  --cap-add SYS_NICE \
  --cap-add IPC_LOCK \
  --cap-add NET_ADMIN \
  --cap-add SYS_TIME \
  --cap-add NET_RAW \
  mtl:latest
```

#### 3.1.3 Specify NIC devices for container

If you only need to pass specific NICs to the container, you can use the following command to list the IOMMU group:

```bash
../script/nicctl.sh list all

ID      PCI BDF         Driver          NUMA    IOMMU   IF Name
0       0000:4b:01.0    vfio-pci        0       311     *
1       0000:4b:01.1    vfio-pci        0       312     *
```

Then, you can specify the IOMMU group IDs to the `--device` argument:

```bash
docker run -it \
  --device /dev/vfio/vfio \
  --device /dev/vfio/311 \
  --device /dev/vfio/312 \
  --cap-add SYS_NICE \
  --cap-add IPC_LOCK \
  mtl:latest
```

### 3.1.4 Run with docker-compose

Edit the `docker-compose.yml` file to specify the configuration.

Run the service:

```bash
docker-compose run imtl
# docker compose run imtl
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
