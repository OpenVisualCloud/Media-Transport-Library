# Docker guide

Docker guide for Intel® Media Transport Library.

## 1. DPDK NIC PMD and env setup on host

Follow [run guide](../doc/run.md) to setup the hugepages, driver of NIC PFs, vfio(2110) user group and vfio driver mode for VFs.

## 2. Build Docker image

```bash
docker build -t mtl:latest -f ubuntu.dockerfile ../
```

Refer to below build command if you are in a proxy env.

```bash
http_proxy=http://proxy.xxx.com:xxx
https_proxy=https://proxy.xxx.com:xxx
docker build -t mtl:latest -f ubuntu.dockerfile --build-arg HTTP_PROXY=$http_proxy --build-arg HTTPS_PROXY=$https_proxy ../
```

## 3. Run and login into the docker container

### 3.1 Run MTL Manager

Before running any IMTL container, please refer to [MTL Manager](../manager/README.md) to run the Manager daemon server.

For legacy way of running multiple containers without MTL Manager, please add the following arguments to the docker run commands in below sections:

```bash
  -v /tmp/kahawai_lcore.lock:/tmp/kahawai_lcore.lock \
  -v /dev/null:/dev/null \
  --ipc=host \
```

### 3.2 Run the docker container

#### 3.2.1 Run with docker command

For DPDK PMD backend, pass the VFIO devices:

```bash
docker run -it \
  --device /dev/vfio \
  --cap-add SYS_NICE \
  --cap-add IPC_LOCK \
  --cap-add NET_ADMIN \
  -v /var/run/imtl:/var/run/imtl \
  --ulimit memlock=-1 \
  mtl:latest
```

For AF_XDP backend, pass the host network interfaces:

```bash
docker run -it \
  --net host \
  --device /dev/vfio \
  --cap-add SYS_NICE \
  --cap-add NET_ADMIN \
  --cap-add NET_RAW \
  --cap-add CAP_BPF \
  -v /var/run/imtl:/var/run/imtl \
  --ulimit memlock=-1 \
  mtl:latest
```

Explanation of `docker run` arguments:

| Argument | Description |
| --- | --- |
| `--net host` | For AF_XDP backend to access NICs |
| `-v /var/run/imtl:/var/run/imtl` | For connection with MTL Manager |
| `--device /dev/vfio` | For DPDK eal to access the VFIO devices |
| `--ulimit memlock=-1` | For DPDK PMD to do DMA remapping or AF_XDP backend to create UMEM |
| `--cap-add SYS_NICE` | For DPDK eal to set NUMA memory policy |
| `--cap-add IPC_LOCK` | For DPDK PMD to do DMA mapping |
| `--cap-add NET_ADMIN` | For kernel NIC configuration |
| `--cap-add NET_RAW` | For AF_XDP backend to create socket |
| `--cap-add CAP_BPF` | For AF_XDP backend to update xsks_map |
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
  --cap-add NET_ADMIN \
  -v /var/run/imtl:/var/run/imtl \
  --ulimit memlock=-1 \
  mtl:latest
```

#### 3.2.3 Run with docker-compose

Edit the `docker-compose.yml` file to specify the configuration.

Run the service:

```bash
docker-compose run imtl
# docker compose run imtl
```

## 4. Run RxTXApp

```bash
# Run below command to generate a fake yuv file or follow "#### 3.3 Prepare source files:" in [run guide](../doc/run.md)
# dd if=/dev/urandom of=test.yuv count=2160 bs=4800
# Edit and Run the loop json file.
./build/app/RxTxApp --config_file tests/script/loop_json/1080p60_1v.json
```
