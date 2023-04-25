# Introduction

Intel® Media Transport Library provides a efficient user space udp stack support from v23.04 version, this UDP LibOS stack is designed to POSIX compatible which allow user can adopt without any code logical change. It has a LD preload layer to intercept UDP socket API calls and replace with our implementation to allow non code change deployment and even no need rebuild. <br>

The data plane traffic is handled within socket API directly under current thread context for extremely performance and lowest latency. Other user-space UDP stack usually run with client-service architect, it introduce a cross-core message cost which hurt the performance and add extra latency. Another major benefit is the data affinity, LLC is kept between the call and UDP stack as it shared both the CPU and data context.

## 1. LibOS design

LibOS is commonly used in cloud computing environments, where it can provide a lightweight and efficient platform for running containers and microservices. Intel® Media Transport Library provide a UDP protocols support to replace the kernel network stack for a lightweight and modular approach to building and running UDP based applications. This allows for greater flexibility and efficiency, as applications can be customized to include the required functionality, and can be run on a variety of platforms without the need for significant modifications.

## 2. LD preload

LD_PRELOAD is an environment variable used in Linux and other Unix-like operating systems to specify additional shared libraries to be loaded before the standard system libraries. This allows users to override or extend the functionality of existing libraries without modifying the original source code. Intel® Media Transport Library has a LD preload layer to intercept many network APIs. The implementation can be found at [udp libos code](../lib/src/udp/).

### 2.1. The UDP APIs intercepted

Note: only SOCK_DGRAM stream will be intercepted and direct to LibOS UDP stack, other streams like TCP will fallback to OS path. The detail code can be found at [ld preload code](../ld_preload/udp/).

| API            | status   | comment |
| :---           | :---     | :---    |
| socket         | &#x2705; |         |
| close          | &#x2705; |         |
| bind           | &#x2705; |         |
| sendto         | &#x2705; |         |
| sendmsg        | &#x2705; | with GSO support    |
| recvfrom       | &#x2705; |         |
| recvmsg        | &#x2705; |         |
| poll           | &#x2705; | with mix fd support |
| ppoll          | &#x2705; | with mix fd support |
| select         | &#x2705; | with mix fd support |
| pselect        | &#x2705; | with mix fd support |
| epoll_create   | &#x2705; |         |
| epoll_create1  | &#x2705; |         |
| epoll_ctl      | &#x2705; |         |
| epoll_wait     | &#x2705; |         |
| epoll_pwait    | &#x2705; | with mix fd support |
| ioctl          | &#x2705; |         |
| fcntl          | &#x2705; |         |
| fcntl64        | &#x2705; |         |
| getsockopt     | &#x2705; |         |
| setsockopt     | &#x2705; |         |

### 2.2. Usage

Customize the so path as your setup as the install path could vary in different OS. MUFD_CFG env point to the configuration file which include the PCIE dpdk BDF port, ip address, queue numbers and other option, see 2.3. MUFD_CFG for detail.

```bash
MUFD_CFG=app/udp/ufd_server.json LD_PRELOAD=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so program-to-run
```

### 2.3. MUFD_CFG configuration

The json file define all the required info for port instance detail. Example json can be found at [sample](../app/udp/ufd_server.json).

#### 2.3.1 interfaces

List the interface that can be used

​ **name (string):** PF/VF pci name, for example: 0000:af:01.0

​ **proto (string):** `"static", "dhcp"` interface network protocol, if DHCP is used, below ip/netmask/gateway will be ignored

​ **ip (string):** interface assigned IP, for example: 192.168.100.1

​ **netmask (string):** interface netmask(optional), for example: 255.255.254.0

​ **gateway (string):** interface gateway(optional), for example: 172.16.10.1, use "route -n" to check the gateway address before binding port to DPDK PMD.

#### 2.3.2 others

 **nb_udp_sockets (int):** The max number of socket sessions supported.

 **nb_nic_queues (int):** The max number of tx and rx queues for NIC.

 **nb_tx_desc (int):** The descriptor number for each NIC tx queue.

 **nb_rx_desc (int):** The descriptor number for each NIC rx queue.

 **log_level (int):** The log level, possible values: debug, info, notice, warning, error.

 **rx_poll_sleep_us (int):** The sleep time(us) in the rx routine to check if there's a available packet in the queue.

 **nic_queue_rate_limit_g (int):** The max rate speed(gigabit per second) for tx queue, only available for ICE(e810) nic.

 **rx_ring_count (int):** The ring count for rx socket session, must be power of 2.

 **nic_shared_queues (bool):** If enable the shared queue support or not. The queue number is limited for NIC, to support sessions more than queue number, enable this option to share queue resource between sessions.

#### 2.3.3 experimental

 **udp_lcore (bool):** If enable the lcore mode or not. The lcore mode will start a dedicated lcore to busy loop all rx queues to receive network packets and then deliver the packet to socket session ring.

 **wake_thresh_count (int):** The threshold for lcore tasklet to check if wake up the socket session, only for lcore mode.

 **wake_timeout_us (int):** The timeout(us) for lcore tasklet to wake up the socket session, only for lcore mode.

## 3. Workload tested

### 3.1 librist

Refer to [guide](../ecosystem/librist/) for detail.

### 3.2 nginx-quic

Get from https://github.com/nginx-quic/nginx-quic

Example command:

```bash
MUFD_CFG=ufd_server.json LD_PRELOAD=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so ./nginx-quic/objs/nginx -p nginx_conf/
```

Below is the configuration we supported.

```bash
master_process off; # no fork support now
listen 443 http3; # reuse port is not supported
quic_gso on; # gso is verified
use epoll; # epoll or select, both are supported
```

### 3.3 ngtcp2

Get from https://github.com/ngtcp2/ngtcp2

Example command:

```bash
MUFD_CFG=ufd_client.json LD_PRELOAD=/usr/local/lib/x86_64-linux-gnu/libmtl_udp_preload.so ngtcp2/examples/client 192.168.85.80 443 https://example.com:443/5G_data -q
```
