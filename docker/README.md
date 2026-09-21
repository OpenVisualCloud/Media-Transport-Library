# Docker Guide

Use these images to build and test Media Transport Library (MTL).

These images are for development. Review their packages and permissions before production use.

## Supported Images

| `MTL_VARIANT` | Base system | DPDK | AF_XDP |
| --- | --- | --- | --- |
| `ubuntu22` | Ubuntu 22.04 | Yes | No |
| `ubuntu24` | Ubuntu 24.04 | Yes | Yes |
| `ubuntu26` | Ubuntu 26.04 | Yes | Yes |
| `rocky9` | Rocky Linux 9 | Yes | Yes |

The default variant is `ubuntu22`.

Each Dockerfile has two stages:

- The `builder` stage installs build tools, builds dependencies, and runs unit tests.
- The `final` stage contains the runtime packages, MTL files, and test tools.

## Prepare the Host

Follow the [run guide](../doc/run.md) before you start a container.

The host must provide these resources:

- Hugepages
- A configured data-plane interface
- VFIO access for DPDK
- The MTL Manager socket, when you use MTL Manager

Start MTL Manager as described in the [MTL Manager guide](../manager/README.md).

## Build an Image

Run commands from the `docker` directory.

Build the default Ubuntu 22.04 image:

```bash
docker compose build imtl
```

Build a different image:

```bash
MTL_VARIANT=ubuntu24 docker compose build imtl
MTL_VARIANT=ubuntu26 docker compose build imtl
MTL_VARIANT=rocky9 docker compose build imtl
```

Each command builds one image. The image name is `mtl:<variant>`.

Compose reads `HTTP_PROXY` and `HTTPS_PROXY` from the host environment.

```bash
export HTTP_PROXY=http://proxy.example.com:8080
export HTTPS_PROXY=http://proxy.example.com:8080
docker compose build imtl
```

## Run with DPDK

Start an interactive container for the default image:

```bash
docker compose run --rm imtl
```

Select a different image with the same variable that you used during the build:

```bash
MTL_VARIANT=ubuntu24 docker compose run --rm imtl
```

The base Compose file provides these settings:

| Setting | Purpose |
| --- | --- |
| `/dev/vfio` | Gives DPDK access to VFIO devices |
| `/var/run/imtl` | Connects applications to MTL Manager |
| Unlimited `memlock` | Lets DPDK lock memory for DMA |
| `SYS_NICE` | Lets DPDK set the NUMA memory policy |
| `IPC_LOCK` | Lets DPDK lock memory |

The container runs as the `imtl` user.

## Run with AF_XDP

Ubuntu 22.04 does not support this configuration.

Use Ubuntu 24.04, Ubuntu 26.04, or Rocky Linux 9. Apply the AF_XDP override file:

```bash
MTL_VARIANT=ubuntu24 docker compose \
  -f docker-compose.yml \
  -f docker-compose.xdp.yml \
  run --rm imtl
```

The override adds host networking, `NET_RAW`, and `CAP_BPF`.

Host networking gives the container direct access to the host interfaces. Review this access before use.

## Run RxTxApp

Create or select an input file. Then run RxTxApp inside the container:

```bash
./RxTxApp --config_file scripts/loop_json/1080p60_1v.json
```

For example, create a test YUV file with this command:

```bash
dd if=/dev/urandom of=test.yuv count=2160 bs=4800
```

## Run without MTL Manager

MTL Manager is the default and recommended configuration.

For the legacy configuration, add these options to the service in a local Compose override:

```yaml
services:
  imtl:
    ipc: host
    volumes:
      - /tmp/kahawai_lcore.lock:/tmp/kahawai_lcore.lock
      - /dev/null:/dev/null
```

Do not commit host-specific overrides.

## Build without Compose

Use a direct Docker command when you only need an image build:

```bash
docker build -t mtl:ubuntu22 -f ubuntu22.dockerfile ..
docker build -t mtl:ubuntu24 -f ubuntu24.dockerfile ..
docker build -t mtl:ubuntu26 -f ubuntu26.dockerfile ..
docker build -t mtl:rocky9 -f rocky9.dockerfile ..
```

The repository root is the build context. The root `.dockerignore` removes generated and local files from that context.

## File Responsibilities

| File | Responsibility |
| --- | --- |
| `docker-compose.yml` | Common DPDK runtime settings and image selection |
| `docker-compose.xdp.yml` | AF_XDP network and capability settings |
| `*.dockerfile` | Distribution packages and image build steps |
| `../.dockerignore` | Build context exclusions |

Use Compose for local runtime configuration. Use the CI image matrix to build all supported images.