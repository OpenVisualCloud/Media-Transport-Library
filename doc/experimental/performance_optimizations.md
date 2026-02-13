# Performance Optimizations

## Linux Kernel

Possible Linux kernel level optimizations to network stack that can improve performance as well as maximize throughput. Every variable comes with proposed value to be set - this can be done by using for example `sysctl -w` command-line helper, for example: `sysctl -w net.ipv4.ip_forward=1`:

### Generic

- `vm.swappiness=0`: Sets the swappiness to 0, minimizing the swapping of processes from RAM to disk, which is beneficial for performance-sensitive applications.
- `vm.panic_on_oom=0`: Disables kernel panic on out-of-memory conditions, allowing the system to handle OOM situations without crashing.
- `vm.overcommit_memory=1`: Allows the kernel to allocate more memory than is physically available, useful for applications that manage their own memory.
- `kernel.panic=10`: Configures the kernel to reboot automatically 10 seconds after a panic, ensuring quick recovery from critical errors.
- `kernel.panic_on_oops=1`: Enables kernel panic on oops, which are non-fatal errors, to prevent potential system instability.
- `vm.max_map_count=262144`: Increases the maximum number of memory map areas a process can have, useful for applications with large memory requirements.
- `net.ipv4.ip_local_port_range="1024 65000"`: Sets the range of local ports available for outgoing connections, allowing more simultaneous connections.
- `net.ipv4.tcp_tw_reuse=1`: Enables reuse of sockets in TIME-WAIT state for new connections, improving network performance.
- `net.ipv4.tcp_fin_timeout=15`: Reduces the time a socket remains in FIN-WAIT-2 state, freeing resources faster.
- `net.core.rmem_max=16777216`: Sets the maximum receive buffer size for network sockets, allowing larger data transfers.
- `vm.nr_hugepages=64`: Allocates 64 hugepages, which are large memory pages that reduce overhead and improve performance for memory-intensive applications.
- `net.ipv6.conf.default.disable_ipv6=1`: Disables IPv6 on the default network interface, which can simplify network configuration if IPv6 is not needed.
- `net.ipv6.conf.all.disable_ipv6=1`: Disables IPv6 on all network interfaces, similar to the previous setting.
- `vm.hugetlb_shm_group=$(id -g)`: Sets the group ID for hugepage shared memory, allowing group members to use hugepages.
- `kernel.sched_rt_runtime_us=-1`: Disables the runtime limit for real-time tasks, allowing them to run indefinitely.
- `kernel.numa_balancing=0`: Disables automatic NUMA balancing, which can improve performance by preventing unnecessary memory migrations.
- `kernel.hung_task_timeout_secs=0`: Disables the timeout for detecting hung tasks, preventing false positives in performance-sensitive environments.
- `net.core.somaxconn=4096`: Increases the maximum number of connections that can be queued for acceptance, useful for high-load servers.
- `net.core.netdev_max_backlog=4096`: Sets the maximum number of packets allowed to queue when the network interface is overwhelmed.
- `net.core.optmem_max=16777216`: Sets the maximum amount of optional memory buffers for network operations.
- `net.core.rmem_max=16777216`: Sets the maximum receive buffer size for network sockets, allowing larger data transfers.
- `net.core.wmem_max=16777216`: Sets the maximum send buffer size for network sockets, allowing larger data transfers.
- `net.ipv4.tcp_rmem="4096 131072 16777216"`: Configures the minimum, default, and maximum TCP receive buffer sizes.
- `net.ipv4.tcp_wmem="4096 131072 16777216"`: Configures the minimum, default, and maximum TCP send buffer sizes.
- `net.ipv4.tcp_syn_retries=2`: Reduces the number of SYN retries for establishing TCP connections, speeding up connection attempts.
- `net.ipv4.tcp_synack_retries=2`: Reduces the number of SYN-ACK retries, speeding up connection establishment.
- `net.ipv4.tcp_max_syn_backlog=20480`: Increases the maximum number of queued SYN requests, improving handling of incoming connections.
- `net.ipv4.tcp_max_tw_buckets=400000`: Increases the maximum number of TIME-WAIT sockets, allowing more connections to be handled.
- `net.ipv4.tcp_no_metrics_save=1`: Disables saving of TCP metrics, which can reduce overhead.
- `net.ipv4.tcp_mtu_probing=1`: Enables TCP MTU probing, which can optimize packet sizes for better performance.
- `net.ipv4.tcp_congestion_control=bbr`: Sets the TCP congestion control algorithm to BBR, which can improve throughput and reduce latency.
- `net.core.default_qdisc=fq`: Sets the default queuing discipline to FQ (Fair Queueing), which can improve network fairness and performance.
- `net.ipv4.tcp_keepalive_time=600`: Sets the interval for TCP keepalive probes, ensuring connections remain active.
- `net.ipv4.ip_forward=1`: Enables IP forwarding, allowing the system to route packets between networks.
- `fs.inotify.max_user_watches=1048576`: Increases the maximum number of `inotify` watches, useful for applications monitoring many files.
- `fs.inotify.max_user_instances=8192`: Increases the maximum number of `inotify` instances, allowing more simultaneous file monitoring.
- `net.ipv4.neigh.default.gc_thresh1=8096`: Sets the threshold for garbage collection of ARP entries, improving network performance.
- `net.ipv4.neigh.default.gc_thresh2=12288`: Sets the threshold for garbage collection of ARP entries, improving network performance.
- `net.ipv4.neigh.default.gc_thresh3=16384`: Sets the threshold for garbage collection of ARP entries, improving network performance.

### External software

Other useful tools, may require additional packages to be installed and available on execution machine:

- `tuned-adm active`: Displays the currently active tuning profile, which helps in understanding the current system performance configuration.
- `tuned-adm profile network-throughput`: Activates the `network-throughput` profile, which optimizes the system settings for maximum network performance, including adjustments to CPU, disk, and network parameters.
- `cpupower idle-set -D0`: Disables CPU idle states, ensuring that the CPU remains active and responsive, which can improve performance in latency-sensitive applications.
- `cpupower frequency-set -g performance`: Sets the CPU frequency governor to `performance`, which keeps the CPU running at its maximum frequency, enhancing performance for demanding workloads.
- `timedatectl set-ntp false`: Disables automatic synchronization of the system clock with network time servers, which can be useful in environments where precise control over time settings is required or where network time synchronization is managed externally.
