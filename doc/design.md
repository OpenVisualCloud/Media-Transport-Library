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

## 4. TX Path

After receiving a frame from an application, MTL constructs a network packet from the frame in accordance with RFC 4175 <https://datatracker.ietf.org/doc/rfc4175/> and ST2110-21 timing requirement.

### 4.1 Zero Copy Packet Build

Most modern Network Interface Cards (NICs) support a multi-buffer descriptor feature, enabling the programming of the NIC to dispatch a packet to the network from multiple data segments. The MTL utilizes this capability to achieve zero-copy transmission when a DPDK Poll Mode Driver (PMD) is utilized, thereby delivering unparalleled performance.
In one typical setup, capable of sending approximately 50 Gbps(equivalent to 16 streams of 1080p YUV422 at 10-bit color depth and 59.94 fps) only requires a single core.

During the packet construction process, only the RTP header is regenerated to represent the packet position within a frame. The video data is carried in the second segment of an mbuf, which directly points to the original frame.

Note that if the currently used NIC does not support the multi-buffer feature, the MTL will need to copy the video frame into the descriptor, resulting in a loss of performance.

<div align="center">
<img src="png/tx_zero_copy.png" align="center" alt="TX Zero Copy">
</div>

### 4.2 ST2110-21 pacing

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

## 5. RX Path

The RX (Receive) packet classification in MTL includes two types: Flow Director and RSS (Receive Side Scaling). Flow Director is preferred if the NIC is capable, as it can directly feed the desired packet into the RX session packet handling function.
Once the packet is received and validated as legitimate, the RX session will copy the payload to the frame and notify the application if it is the last packet.

### 5.1 RX DMA offload

The process of copying data between packets and frames consumes a significant amount of CPU resources. MTL can be configured to use DMA to offload this copy operation, thereby enhancing performance. For detailed usage instructions, please refer to [DMA guide](./dma.md)

<div align="center">
<img src="png/rx_dma_offload.png" align="center" alt="RX DMA Offload">
</div>
