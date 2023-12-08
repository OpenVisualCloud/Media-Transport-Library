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

Follow [run guide](../doc/run.md) to setup the hugepages, driver of NIC PFs, vfio driver mode for VFs.

## 3. Run and login into the docker container

### 3.1 Run MTL Manager

Before running any IMTL container, please refer to [MTL Manager](../manager/README.md) to run the Manager daemon server.

### 3.2 Run the docker container

#### 3.2.1 Run with docker command

For DPDK backend, pass the VFIO devices:

```bash
docker run -it \
  --device /dev/vfio \
  --cap-add SYS_NICE \
  --cap-add IPC_LOCK \
  -v /var/run/imtl:/var/run/imtl \
  mtl:latest
```

For kernel / AF_XDP backend (new dockerfile WIP), pass the host network interfaces:

```bash
docker run -it \
  --net host \
  --device /dev/vfio \
  --cap-add SYS_NICE \
  --cap-add NET_ADMIN \
  --cap-add NET_RAW \
  -v /var/run/imtl:/var/run/imtl \
  mtl:latest
```

Explanation of `docker run` arguments:

| Argument | Description |
| --- | --- |
| `-v /var/run/imtl:/var/run/imtl` | For connection with MTL Manager |
| `--device /dev/vfio` | Access the VFIO device |
| `--cap-add SYS_NICE` | For set_mempolicy |
| `--cap-add IPC_LOCK` | For DMA mapping |
| `--cap-add NET_ADMIN` | For kernel NIC configuration |
| `--cap-add NET_RAW` | For AF_XDP socket |
| `--cap-add SYS_TIME` | For systime adjustment if `--phc2sys` enabled |

#### 3.2.2 Specify VFIO devices for container

If you only need to pass specific PFs/VFs to the container, you can use the following command to list the IOMMU group:

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
  -v /var/run/imtl:/var/run/imtl \
  mtl:latest
```

### 3.2.3 Run with docker-compose

Edit the `docker-compose.yml` file to specify the configuration.

Run the service:

```bash
docker-compose run imtl
# docker compose run imtl
```

### 3.3 Switch to the root user inside a Docker container

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
