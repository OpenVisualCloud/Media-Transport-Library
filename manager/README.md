# MTL Manager Documentation

## Overview

![design](manager_design.svg)

MTL Manager is a daemon server designed to operate with root privileges. Its primary role is to manage the lifecycle and configurations of MTL instances. It addresses a variety of administrative tasks, including:

- Lcore Management: Ensures that MTL instances are aware of the lcores used by others, optimizing resource allocation.
- eBPF/XDP Loader: Dynamically loads and manages eBPF/XDP programs for advanced packet processing and performance tuning.
- NIC Configuration: Configures Network Interface Cards (NICs) with capabilities like adding or deleting flow and queue settings.
- Instances Monitor: Continuously monitors MTL instances, providing status reporting and clearing mechanisms.

## Build

To compile and install the MTL Manager, use the following commands:

```bash
meson setup build
meson compile -C build
sudo meson install -C build
```

Besides MTL Manager, it will also install a built-in XDP program for udp port filtering.

## Run

To run the MTL Manager, execute:

```bash
sudo MtlManager
```

This command will start the MTL Manager with root privileges, which are necessary for the advanced eBPF and network configurations and management tasks it performs.

The XDP program for udp port filtering will be loaded along with the libxdp's built-in xsk program when the AF_XDP socket is created. It utilizes the xdp-dispatcher program provided by libxdp which allows running of multiple XDP programs in chain on the same interface. You can check the loaded programs with xdp-loader:

```bash
$ sudo xdp-loader status
Interface        Prio  Program name      Mode     ID   Tag               Chain actions
--------------------------------------------------------------------------------------
lo                     <No XDP program loaded!>
ens787f0               <No XDP program loaded!>
ens787f1               <No XDP program loaded!>
ens785f0               xdp_dispatcher    native   23661 90f686eb86991928 
 =>              19     mtl_dp_filter             23670 02aea45cd16e8656  XDP_DROP
 =>              20     xsk_def_prog              23622 8f9c40757cb0a6a2  XDP_PASS
ens785f1               xdp_dispatcher    native   23675 90f686eb86991928 
 =>              19     mtl_dp_filter             23678 02aea45cd16e8656  XDP_DROP
 =>              20     xsk_def_prog              23639 8f9c40757cb0a6a2  XDP_PASS
virbr0                 <No XDP program loaded!>
docker0                <No XDP program loaded!>
```

## Run in a Docker container

Build the Docker image:

```bash
docker build -t mtl-manager:latest .
# docker build -t mtl-manager:latest --build-arg HTTP_PROXY=$http_proxy --build-arg HTTPS_PROXY=$https_proxy .
```

Run the Docker container as a daemon:

```bash
docker run -d \
  --name mtl-manager \
  --privileged --net=host \
  -v /var/run/imtl:/var/run/imtl \
  -v /sys/fs/bpf:/sys/fs/bpf \
  mtl-manager:latest
```

Print the MTL Manager logs:

```bash
docker logs -f mtl-manager
```

Shutdown the Docker container with SIGINT:

```bash
docker kill -s SIGINT mtl-manager
# docker rm mtl-manager
```
