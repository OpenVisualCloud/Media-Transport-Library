# DPDK Usage Patterns — Design Knowledge

## How MTL Abstracts DPDK

MTL doesn't expose DPDK to applications. Instead, it wraps DPDK behind a **backend abstraction** that supports multiple transport technologies. The key insight: MTL code above the backend layer doesn't know or care whether packets go through DPDK, AF_XDP, kernel sockets, or RDMA.

The abstraction point is `mt_queue.c` — all queue operations (get, put, burst_tx, burst_rx) dispatch through function pointers set at queue creation time based on the backend type. This means adding a new backend is straightforward: implement the queue interface, register it in device init.

## EAL Initialization: Why It's Complicated

DPDK's EAL (Environment Abstraction Layer) is a heavy global init that takes over memory management, thread creation, and PCI devices. MTL must:
1. Build EAL argument list dynamically based on user config (hugepage size, IOVA mode, allowed PCI devices)
2. Run `rte_eal_init()` **once** per process — it's not repeatable
3. Handle multi-process mode for shared memory scenarios

**Why MTL builds EAL args dynamically**: The user specifies ports as PCI addresses. MTL translates these to `--allow` arguments so EAL only probes the necessary devices. It also detects IOVA mode (PA vs VA) based on the kernel's IOMMU support, sets hugepage params, and handles logging.

**Why EAL runs in a separate thread for plugins**: When loading DPDK plugins (like the AF_XDP PMD), the init sequence has ordering constraints. MTL handles this transparently.

## Port Configuration Design

MTL configures each NIC port for maximum flexibility:
- **RSS disabled by default** for video ports — flow director is preferred for precise queue steering
- **RSS enabled** when `SHARED_RSS` flag is set (for NICs without flow director)
- **Promiscuous mode** enabled so multicast traffic is received
- **Multi-segment TX** enabled for the zero-copy mbuf chain model
- **Checksum offload** to NIC hardware — CPU doesn't compute IP/UDP checksums

**Port start timing**: Ports are started during `mtl_start()`, not during `mtl_init()`. This is intentional — flow rules must be installed before packets arrive, but flow rules require an active port on some NICs. The startup sequence ensures rules are ready before traffic flows.

## The Queue Dispatch Architecture

This is the central design for how packets reach the right session:

```
                         ┌─── Dedicated Queue ──→ Session (1:1)
                         │
Incoming Packet → NIC ───┤─── Flow Director ────→ Queue → Session
                         │
                         ├─── Shared Queue (RSQ) → Software dispatch → per-session ring
                         │
                         └─── Shared RSS ────────→ Software hash dispatch → per-session ring
```

**Decision logic**: 
- If the NIC has flow director AND queues available → dedicated queue per session
- If queues are scarce → shared queue with software dispatch
- If no flow director at all → shared RSS (hardware RSS + software refinement)

**Why both RSQ and RSS?** RSQ uses NIC flow rules to steer to a shared queue, then software sorts by 5-tuple. RSS uses the NIC's hash function to spread across queues, then software checks which session each packet belongs to. RSQ gives better isolation; RSS works on simpler hardware.

## Flow Rules: Steering Packets to Queues

For dedicated queues, MTL installs `rte_flow` rules matching on:
- Destination IP (multicast group)
- Destination UDP port
- Optionally source IP (for SSM — Source-Specific Multicast)

**Rule lifecycle**: Created during session init, destroyed during session teardown. If rule creation fails (NIC doesn't support it), MTL falls back to shared queue mode automatically.

**The IP+port combo is the session identifier on the wire**: Two sessions can share an IP if they have different UDP ports, or share a port if they have different IPs. But the same IP+port means the same session (or redundant path).

## Mbuf Lifecycle: The Zero-Copy Path

Understanding the mbuf lifecycle is essential for debugging memory issues:

### TX: Frame → Wire (zero-copy)
1. **Header mbuf** allocated from session's mempool — contains Ethernet/IP/UDP/RTP headers (~62 bytes)
2. **Payload "mbuf"** created via `rte_pktmbuf_attach_extbuf()` — points into app's frame buffer, no copy
3. **Chain**: `rte_pktmbuf_chain(header, payload)` — NIC scatter-gathers both
4. **Enqueue** to TX ring → NIC DMAs from both mbuf data areas
5. **Completion**: NIC signals done → mbuf refcnt decremented → when zero, header mbuf returned to pool, extbuf's `sh_info.free_cb` fires to release frame refcnt

### RX: Wire → Frame
1. **NIC writes** received packet into mbuf from RX queue's mempool
2. **Tasklet polls** `rte_eth_rx_burst()` — gets batch of mbufs
3. **Payload extracted** and **memcpy'd** (or DMA-copied) into frame buffer at the correct offset
4. **Mbuf freed** immediately back to RX mempool
5. Frame buffer accumulates all packets until complete

**Why RX copies but TX doesn't**: In TX, the frame buffer already exists in correct order — we just point to slices of it. In RX, packets arrive in random order and must be placed at specific offsets in the frame. You can't attach_extbuf into a destination — you have to copy.

## Traffic Manager: Hardware Pacing

The Intel E810 TM (Traffic Manager) hierarchy in MTL:

```
Port (wire rate) → Node (schedulable) → Leaf (per-session shaper)
```

Each TX session that uses RL pacing gets a TM leaf node with a configured rate. The hierarchy is:
- **Level 0**: Port-level node (root)
- **Level 1**: Intermediate nodes (one per queue group)  
- **Level 2**: Leaf nodes (one per session) — this is where the rate limit is applied

**Capacity limit**: 128 leaf nodes per port. Beyond that, sessions must use TSC pacing. For deployments with many low-bandwidth sessions, this limit matters.

**Shaper accuracy**: The E810 TM works in bytes/sec granularity. MTL converts the ST2110 rate (including ethernet overhead, preamble, IFG) to the TM rate parameter. Getting this calculation wrong causes either over-pacing (packets arrive too fast, VRX violations) or under-pacing (frame can't be sent within its epoch).

## Multi-Process and Shared Memory

DPDK supports multi-process mode for shared NIC access:
- Primary process initializes EAL and ports
- Secondary processes attach to the same shared memory regions
- MTL uses this for the manager daemon (`mtl_manager`) which oversees multiple MTL instances

**Gotcha**: Secondary processes CANNOT create/destroy mempools or rings — only the primary can. This constrains what the manager daemon can do.

## Backend-Specific Knowledge

### AF_XDP
- Uses Linux kernel's XDP socket for zero-copy without DPDK's VFIO/UIO
- UMEM (shared memory region) must be pre-allocated and registered with the kernel
- Less performant than DPDK PMD but doesn't require root or special drivers
- Two flavors: native (`af_xdp:`) and DPDK (`dpdk_af_xdp:`) — native is preferred

### Kernel Socket
- Pure socket()/sendto()/recvfrom() — maximum compatibility, minimum performance  
- Useful for development/testing without DPDK or XDP setup
- Creates dedicated TX/RX threads (unlike DPDK which uses tasklets)

### RDMA UD
- Experimental: uses RDMA Unreliable Datagram for transport
- Bypasses kernel network stack but doesn't require DPDK
- Currently limited feature set

## Debugging DPDK Issues

- **Port doesn't start**: Check VFIO/UIO binding (`dpdk-devbind.py`). Ensure hugepages are allocated. Check IOMMU settings.
- **No packets received**: Check flow rules with `rte_flow_query()`. Verify multicast group join succeeded. Check if RSS is enabled when it shouldn't be (or vice versa).
- **TX drops**: NIC TX descriptor ring full — either sending too fast or NIC is saturated. Check `stat_tx_burst` and ring occupancy.
- **Mempool exhaustion**: All mbufs in flight — either TX completion is slow or mempool is undersized. Watch `rte_mempool_avail_count()`.
- **Wrong NUMA node**: Use `rte_socket_id()` vs `rte_eth_dev_socket_id()` mismatch — silent performance halving.
