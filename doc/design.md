# Design Guide

## 1. Introduction

This section provides a detailed design concept of the Intel® Media Transport Library, offering an in-depth dive into the technology used.

Similar to other network processing libraries, it consists of a control plane and a data plane. In the data plane, a lockless design is adopted to achieve ultra-high performance.

<div align="center">
<img src="png/software_stack.png" align="center" alt="Software Stack">
</div>

## 2. Core management

MTL default uses busy polling, also known as busy-waiting or spinning, to achieve high data packet throughput and low latency. This technique constantly checks for new data packets to process rather than waiting for an interrupt. The polling thread is pinned to a single CPU core to prevent the thread from migrating between CPU cores.

Busy polling allows the application to detect and process packets as soon as they arrive, minimizing latency. It provides consistent and predictable packet processing times because there's no waiting time introduced by other scheduling mechanisms. It also avoids context switches between the kernel and user space, which can be costly in terms of CPU cycles.

The drawbacks is it can lead to 100% CPU usage because the cores are always active, checking for new work.

With this PMD design, it is expected that a CPU thread will always be utilized to 100%, even with only one stream active. In our configuration, one core can handle up to a maximum of 16 1080p transmission sessions, although the actual density may vary depending on the hardware configuration.

BTW, we provide a option `MTL_FLAG_TASKLET_SLEEP` that enables the sleep option for the PMD thread. However, take note that enabling this option may impact latency, as the CPU may enter a sleep state when there are no packets on the network. If you are utilizing the RxTxApp, it can be enable by `--tasklet_sleep` arguments.
Additionally, the `MTL_FLAG_TASKLET_THREAD` option is provided to disable pinning to a single CPU core, for cases where a pinned core is not feasible.

<div align="center">
<img src="png/tasklet.png" align="center" alt="Tasklet">
</div>

### 2.1 Tasklet design

To efficient utilize the pinned polling thread, MTL has developed asynchronous scheduler called tasklet. A tasklet is a small, lightweight function that runs in the context of the pinned polling thread and is scheduled by MTL scheduler. Tasklets are used for performing quick, non-blocking operations that can't go to sleep.
The operation of MTL's internal jobs is typically triggered by the availability of packets in the NIC's RX queue, space in the TX queue, or available data in the ring. Consequently, the tasklet design is highly suitable for these processes.
One primary advantage of using tasklets is that all tasklets associated with a single stream session are bound to one thread, allowing for more efficient use of the Last Level Cache (LLC) at different stages of processing.

### 2.2 Scheduler quota

A single scheduler (pinned polling thread) can have numerous tasklets registered. To manage the distribution of tasklets across schedulers, a 'quota' system has been implemented in each scheduler, indicating the total data traffic each core can handle.
Sessions will submit a request to the scheduler manager for a scheduler to manage their jobs. Upon receiving a request, the scheduler manager will assess whether the recent scheduler has enough quota to service this new request.
If not, a new scheduler will be created and allocated to the session for the upcoming tasklet registration. For further details, please refer to the source code in [sch code](../lib/src/mt_sch.c).

The performance of the setup can vary, so the data traffic quota for each scheduler is customizable by the application through the `data_quota_mbs_per_sch` parameter.

### 2.3 Session migrate

Additionally, MTL has introduced support for session migration with the `MTL_FLAG_TX_VIDEO_MIGRATE` and `MTL_FLAG_RX_VIDEO_MIGRATE` flags. This feature enables runtime CPU usage calculations. When the system detects that a scheduler is operating at 100% capacity, that overloaded scheduler will attempt to redistribute its last few sessions to other underutilized schedulers.
This migration capability adds flexibility to deployment, accommodating the often unpredictable capacity of a system.

### 2.4 Multi process support

MTL supports multi-process deployment through the use of SR-IOV. Each process operates with its own perspective on core usage, to prevent conflicts that arise when multiple processes attempt to use the same core, MTL utilizes a Manager service which ensures that each MTL instance is allocated a distinct and unused core.
Each instance sends a request to the Manager service, which in return assigns a free core to the instance. The Manager service is also responsible for detecting when an instance disconnects and will subsequently release the associated resources. For more details, please consult the [Manager guide](../manager/README.md)

If the background Manager service is not practical for your setup, there is a fallback method: managing the logical core (lcore) via shared memory. In this approach, all MTL instances loop through a shared memory structure to locate an unused core.
The instructions for this deprecated method can still be accessed in the [shm_lcore guide](./shm_lcore.md). However, we strongly advise against this method and recommend using the Manager service instead, as it has the capability to detect when any instance has been unexpectedly closed.

### 2.5 The tasklet API for application

Applications can also leverage the efficient tasklet framework. An important note is that the callback tasklet function cannot use any blocking methods, as the thread resource is shared among many tasklets. For more information, please refer to the [mtl_sch_api](../include/mtl_sch_api.h). Example usage is provided below:

```bash
    mtl_sch_create
    mtl_sch_register_tasklet
    mtl_sch_start
    mtl_sch_stop
    mtl_sch_unregister_tasklet
    mtl_sch_free
```

## 3. Memory management

### 3.1 Huge Page

MTL utilizes hugepages for performance optimization when processing packets at high speed.

* Reduced TLB (Translation Lookaside Buffer) Misses. By using larger page sizes, more physical memory can be addressed with fewer entries in the TLB. Fewer TLB misses mean fewer CPU cycles are spent on memory address translation, which leads to better performance.
* Improved Cache Usage. Contiguous Physical Memory. And the physical memory is contiguous, which is beneficial for I/O operations and can improve DMA (Direct Memory Access) from devices that are used in high-speed packet processing.
* Avoiding Page Faults. With hugepages, more data can be kept in the CPU cache because of the reduced page overhead. This can lead to better cache utilization, reduced cache misses, and faster access to the data needed for packet processing.

HugePages come in two sizes: 2MB and 1GB. MTL recommends using the 2MB pages because they are easier to configure in the system; typically, 1GB pages require many additional settings in the OS. Moreover, according to our performance measurements, the benefits provided by 2MB pages are sufficient.
The hugepages size is dependent on the workloads you wish to execute on the system, usually a 2G huge page is a good start point, consider increasing the value if memory allocation failures occur during runtime.

### 3.2 Memory API

In MTL, memory management is directly handled through DPDK's memory-related APIs, including mempool and mbuf. In fact, all internal data objects are constructed based on mbuf/mempool to ensure efficient lifecycle management.

## 4. Data path

### 4.1 Backend layer

The library incorporates a virtual data path backend layer, designed to abstract various NIC implementation and provide a unified packet TX/RX interface to the upper network layer. It currently supports three types of NIC devices:

* DPDK Poll-Mode Drivers (PMDs): These drivers fully bypass the kernel's networking stack, utilizing the 'DPDK poll mode' driver.
* Native Linux Kernel Network Socket Stack: This option supports the full range of kernel ecosystems. Related code can be found from [mt_dp_socket.c](../lib/src/datapath/mt_dp_socket.c)
* AF_XDP (Express Data Path) with eBPF filter: AF_XDP represents a significant advancement in the Linux networking stack, striking a balance between raw performance and integration with the kernel's networking ecosystem. Please refer to [mt_af_xdp.c](../lib/src/dev/mt_af_xdp.c) for detail.
* Native Windows Kernel Network Socket Stack: in plan, not implemented.

MTL selects the backend NIC based on input from the application. Users should specify both of the following parameters in `struct mtl_init_params`, the port name should follow the format described below, and the pmd type can be fetched using `mtl_pmd_by_port_name`.

```bash
  /**
   *  MTL_PMD_DPDK_USER. Use PCIE BDF port, ex: 0000:af:01.0.
   *  MTL_PMD_KERNEL_SOCKET. Use kernel + ifname, ex: kernel:enp175s0f0.
   *  MTL_PMD_NATIVE_AF_XDP. Use native_af_xdp + ifname, ex: native_af_xdp:enp175s0f0.
   */
  char port[MTL_PORT_MAX][MTL_PORT_MAX_LEN];
  enum mtl_pmd_type pmd[MTL_PORT_MAX];
```

### 4.2 Queue Manager

The library incorporates a queue manager layer, designed to abstract various queue implementation. Code please refer to [mt_queue.c](../lib/src/datapath/mt_queue.c) for detail.

#### 4.2.1 Tx

For transmitting (TX) data, there are two queue modes available:

Dedicated Mode: In this mode, each session exclusively occupies one TX queue resource.

Shared Mode: In contrast, shared mode allows multiple sessions to utilize the same TX queue. To ensure there is no conflict in the packet output path, a spin lock is employed. While this mode enables more efficient use of resources by sharing them, there can be a performance trade-off due to the overhead of acquiring and releasing the lock.
The TX queue shared mode is enabled by `MTL_FLAG_SHARED_TX_QUEUE` flag. Code please refer to [mt_shared_queue.c](../lib/src/datapath/mt_shared_queue.c) for detail.

#### 4.2.2 RX

For RX data, there are three queue modes available:

Dedicated Mode: each session is assigned a unique RX queue. Flow Director is utilized to filter and steer the incoming packets to the correct RX queue based on criteria such as IP address, port, protocol, or a combination of these.

Shared Mode: allows multiple sessions to utilize the same RX queue. Each session will configure its own set of Flow Director rules to identify its specific traffic. However, all these rules will direct the corresponding packets to the same shared RX queue. Software will dispatch the packet to each session during the process of received packet for each queue.
The RX queue shared mode is enabled by `MTL_FLAG_SHARED_RX_QUEUE` flag. Code please refer to [mt_shared_queue.c](../lib/src/datapath/mt_shared_queue.c) for detail.

RSS mode: Not all NICs support Flow Director. For those that don't, we employs Receive Side Scaling (RSS) to enable the efficient distribution of network receive processing across multiple queues. This is based on a hash calculated from fields in packet headers, such as source and destination IP addresses, and port numbers.
Code please refer to [mt_shared_rss.c](../lib/src/datapath/mt_shared_rss.c) for detail.

### 4.3 ST2110 TX

After receiving a frame from an application, MTL constructs a network packet from the frame in accordance with RFC 4175 <https://datatracker.ietf.org/doc/rfc4175/> and ST2110-21 timing requirement.

#### 4.3.1 Zero Copy Packet Build

Most modern Network Interface Cards (NICs) support a multi-buffer descriptor feature, enabling the programming of the NIC to dispatch a packet to the network from multiple data segments. The MTL utilizes this capability to achieve zero-copy transmission when a DPDK Poll Mode Driver (PMD) is utilized, thereby delivering unparalleled performance.
In one typical setup, capable of sending approximately 50 Gbps(equivalent to 16 streams of 1080p YUV422 at 10-bit color depth and 59.94 fps) only requires a single core.

During the packet construction process, only the RTP header is regenerated to represent the packet position within a frame. The video data is carried in the second segment of an mbuf, which directly points to the original frame.

Note that if the currently used NIC does not support the multi-buffer feature, the MTL will need to copy the video frame into the descriptor, resulting in a loss of performance.

<div align="center">
<img src="png/tx_zero_copy.png" align="center" alt="TX Zero Copy">
</div>

#### 4.3.2 ST2110-21 pacing

The specific standard ST2110-21 deals with the traffic shaping and delivery timing of uncompressed video. It defines how the video data packets should be paced over the network to maintain consistent timing and bandwidth utilization.

Due to the stringent timing requirements at the microsecond level, existing solutions are primarily built on hardware implementations, which introduce significant dependencies not conducive to cloud-native deployments. The MTL adopts a software-based approach, embracing cloud-native concepts.
MTL addresses this challenge by leveraging the NIC's rate-limiting features along with a software algorithm. This combination has successfully passed numerous third-party interoperability verifications.

The default NIC queue depth is set to 512 in MTL, and MTL will always ensure the queue is fully utilized by the tasklet engine. In the case of 1080p at 50fps, one packet time in ST2110-21 is approximately ~5us.
With a queue depth of 512, the IMTL can tolerate a kernel scheduler jitter of up to ~2.5ms. If you observe any packet timing jitter, consider increasing the queue depth. MTL provides the `nb_tx_desc` option for this adjustment.
However, for a 4K 50fps session, the time for one packet is approximately ~1us, indicating that the duration for 512 packets is around ~500us. With a queue depth of 512, IMTL can only tolerate a scheduler jitter of about ~500us. However, by adjusting the depth to the maximum hardware-permitted value of 4096, IMTL should be capable of handling a maximum scheduler jitter of 4ms.

In the case that the rate-limiting feature is unavailable, TSC (Timestamp Counter) based software pacing is provided as a fallback option.

<div align="center">
<img src="png/tx_pacing.png" align="center" alt="TX Pacing">
</div>

### 4.4 ST2110 RX

The RX (Receive) packet classification in MTL includes two types: Flow Director and RSS (Receive Side Scaling). Flow Director is preferred if the NIC is capable, as it can directly feed the desired packet into the RX session packet handling function.
Once the packet is received and validated as legitimate, the RX session will copy the payload to the frame and notify the application if it is the last packet.

#### 4.4.1 RX DMA offload

The process of copying data between packets and frames consumes a significant amount of CPU resources. MTL can be configured to use DMA to offload this copy operation, thereby enhancing performance. For detailed usage instructions, please refer to [DMA guide](./dma.md)

<div align="center">
<img src="png/rx_dma_offload.png" align="center" alt="RX DMA Offload">
</div>

## 5. Control path

For the DPDK Poll Mode Driver backend, given its nature of fully bypassing the kernel, it is necessary to implement specific control protocols within MTL."

### 5.1 ARP

Address Resolution Protocol is a communication protocol used for discovering the link layer address, such as a MAC address, associated with a given internet layer address, typically an IPv4 address. This mapping is critical for local area network communication. The code can be found from [mt_arp.c](../lib/src/mt_arp.c)

### 5.2 IGMP

The internet Group Management Protocol is a communication protocol used by hosts and adjacent routers on IPv4 networks to establish multicast group memberships. IGMP is used for managing the membership of internet Protocol multicast groups and is an integral part of the IP multicast specification. MTL support the IGMPv3 version. The code can be found from [mt_mcast.c](../lib/src/mt_mcast.c)

### 5.3 DHCP

Dynamic Host Configuration Protocol is a network management protocol used on IP networks whereby a DHCP server dynamically assigns an IP address and other network configuration parameters to each device on a network, so they can communicate with other IP networks.
DHCP allows devices known as clients to get an IP address automatically, reducing the need for a network administrator or a user to manually assign IP addresses to all networked devices.

The DHCP option is not default on, enable it by set `net_proto` in `struct mtl_init_params` to `MTL_PROTO_DHCP`.

The code implementation can be found from [mt_dhcp.c](../lib/src/mt_dhcp.c).

### 5.4 PTP

Precision Time Protocol, also known as IEEE 1588, is designed for accurate clock synchronization between devices on a network. PTP is capable of clock accuracy in the sub-microsecond range, making it ideal for systems where precise timekeeping is vital. PTP uses a master-slave architecture for time synchronization.
Typically, a PTP grandmaster is deployed within the network, and clients synchronize with it using tools like ptp4l.

MTL support two type of PTP client settings, the built-in PTP client implementation inside MTL or using a external PTP time source.

#### 5.4.1 Built-in PTP

This project includes a built-in support for the PTP client protocol, which is also based on the hardware offload timesync feature. This combination allows for achieving a PTP time clock source with an accuracy of approximately 30ns.

To enable this feature in the RxTxApp sample application, use the `--ptp` argument. The control for the built-in PTP feature is the `MTL_FLAG_PTP_ENABLE` flag in the `mtl_init_params` structure.

Note: Currently, the VF (Virtual Function) does not support the hardware timesync feature. Therefore, for VF deployment, the timestamp of the transmitted (TX) and received (RX) packets is read from the CPU TSC (TimeStamp Counter) instead. In this case, it is not possible to obtain a stable delta in the PTP adjustment, and the maximum accuracy achieved will be up to 1us.

#### 5.4.2 Customized PTP time source by Application

Some setups may utilize external tools, such as `ptp4l`, for synchronization with a grandmaster clock. MTL provides an option `ptp_get_time_fn` within `struct mtl_init_params`, allowing applications to customize the PTP time source. In this mode, whenever MTL requires a PTP time, it will invoke this function to acquire the actual PTP time.
Consequently, it is the application's responsibility to retrieve the time from the PTP client configuration.

#### 5.4.3 37 seconds offset between UTC and TAI time

There's actually a difference of 37 seconds between Coordinated Universal Time (UTC) and International Atomic Time (TAI). This discrepancy is due to the number of leap seconds that have been added to UTC to keep it synchronized with Earth's rotation, which is gradually slowing down.

It is possible to observe a 37-second offset in some third-party timing equipment using MTL in conjunction with external PTP4L. This is typically caused by the time difference between Coordinated Universal Time (UTC) and International Atomic Time (TAI).
While PTP grandmasters disseminate the offset in their announce messages, this offset is not always accurately passed to the `ptp_get_time_fn` function. The RxTxApp provides a `--utc_offset` option, with a default value of 37 seconds, to compensate for this discrepancy. Consider adjusting the offset if you encounter similar issues.
