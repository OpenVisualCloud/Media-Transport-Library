# Pacing, Timing & Performance — Design Knowledge

## Why Pacing Exists

SMPTE ST2110-21 mandates that media streams arrive at the receiver within strict timing bounds. You can't just blast packets at wire speed — the receiver's buffer would overflow, then underflow. Pacing means **spreading packets evenly across the frame period** to match the video's temporal structure.

A 1080p60 frame has ~4,500 packets that must be sent across ~16.7ms. The **ideal** spacing is ~3.7µs between packets (`trs` — Time between Reference packets). Real hardware can't achieve perfect spacing, but the spec defines bounds (Narrow, Wide, Linear models) within which the sender must stay.

## The Two Pacing Strategies

### Hardware Rate Limiter (RL) — Preferred
Intel E810 NICs have a hardware Traffic Manager (TM) that can enforce per-flow rate limits. MTL creates a TM shaper per session:
- Configured with the **exact wire rate** adjusted for blanking (`reactive` factor: active_lines/total_lines)
- The NIC hardware spaces packets internally — the CPU just enqueues and the NIC drains at the configured rate
- This is the gold standard: sub-microsecond accuracy, zero CPU overhead for timing

**Why `reactive` matters**: A 1080p frame has 1080 active lines but 1125 total lines (45 are blanking). The rate limiter runs at `wire_rate × (1080/1125)` so that data-bearing packets arrive during the active period, with natural gaps during blanking.

### Software TSC Pacing — Fallback
When no hardware RL is available (non-E810, or RL capacity exhausted at 128 shapers), the **transmitter tasklet** does timing in software:
- Read TSC (Time Stamp Counter) to get current time
- Compare against the next scheduled packet time (`pacing.tsc_time_cursor`)
- If too early, return and let the scheduler poll again (busy-wait effectively)
- If on time or late, transmit the packet

**TSC accuracy**: TSC is calibrated at boot (that's what the TSC calibration thread does). It gives ~nanosecond resolution but the actual pacing jitter depends on how often the scheduler polls the transmitter tasklet. Under load, jitter increases.

## The Builder→Transmitter Pipeline

Why not build and pace in one tasklet? Because **pacing requires fast reaction to timing events**, while packet building involves frame reads and header construction. Separating them:

1. **Builder** runs when it can — reads frames, constructs packets, enqueues to `rte_ring`
2. **Transmitter** runs with timing priority — dequeues from ring, applies pacing, bursts to NIC

The ring acts as a shock absorber. If the builder is slow for a few iterations (cache miss, page fault), the transmitter still has queued packets to send on time. Conversely, the builder can run ahead and fill the ring during idle periods.

**Ring sizing**: Too small → transmitter starves (pacing gaps). Too large → excessive latency and memory. Default is 512 entries (covers roughly one frame worth of packets for 1080p).

## Warm-Up and Padding Packets

The hardware rate limiter has a ramp-up delay — the first few packets after enabling the shaper may not be paced correctly. MTL sends **padding packets** (valid RTP packets with padding bit set) before the first frame's data. The receiver ignores padding, but the RL gets primed.

**How many?** Configurable. The default is enough to fill one frame's worth of pacing time. If you see the first frame of a stream having timing violations, increase warm-up padding.

## PTP: The Time Reference

All pacing is relative to **PTP time** (IEEE 1588). PTP synchronizes clocks across all devices in the network to sub-microsecond accuracy.

- MTL implements a PTP slave in software (`mt_ptp.c`)
- The NIC hardware timestamps PTP packets for precise offset calculation
- Every RTP packet carries a timestamp derived from PTP time
- The receiver uses PTP timestamps to reconstruct the sender's timing intent

**Without PTP**: Pacing still works locally (TSC-based), but the sender and receiver clocks drift. For studio environments, PTP is mandatory.

## The Epoch Timing Model

MTL aligns frame transmission to **epoch boundaries** — the PTP time at which a frame period starts. For 59.94fps:
- Frame period = 1001/60000 seconds ≈ 16.683ms
- Epoch N starts at `base_time + N × frame_period`

The builder targets the current epoch: if the app provides a frame late (after the epoch starts), MTL may advance to the next epoch, effectively dropping the late frame. This is intentional — sending a late frame would cause timing violations downstream.

**The "late" frame stat**: When you see `stat_frame_late` incrementing, the app isn't providing frames fast enough. The fix is in the application, not in MTL.

## VRX (Virtual Receiver Buffer) Conformance

The receiver maintains a virtual buffer model per ST2110-21. It monitors whether incoming packets arrive within the spec's timing envelope:
- `vrx_min` / `vrx_max` — observed buffer excursion bounds
- If packets arrive too fast (burst), VRX max increases
- If packets arrive too slow (starvation), VRX min drops below zero

These stats are diagnostic. High VRX max → sender is bursty. VRX min going negative → packets arriving late. This tells you whether your pacing is compliant.

## Performance Debugging Mental Model

When diagnosing performance issues, think in layers:

1. **Is the NIC the bottleneck?** Check `stat_tx_burst` vs `stat_tx_bytes`. If burst count is high but bytes are low, packets are being truncated or dropped at the NIC.

2. **Is the scheduler overloaded?** Check if the scheduler is at 100% CPU (never sleeping). If so, it has too many sessions — reduce load or add more schedulers.

3. **Is pacing broken?** RX side: check VRX stats. TX side: check `stat_epoch_mismatch` and `stat_frame_late`. Non-zero epoch_mismatch means the transmitter couldn't keep up with the pacing clock.

4. **Is it a NUMA problem?** If a session that should be light (1080p30) is consuming excessive CPU, check NUMA affinity. Socket mismatch between NIC and memory causes 2× latency on every DMA operation.

5. **Is the ring underflowing?** If the transmitter runs but sends fewer packets than expected per epoch, the builder→transmitter ring may be starving. Increase ring size or reduce builder workload.

## Key Performance Numbers (for intuition)

- 1080p60 uncompressed = ~5 Gbps per stream
- 4K60 uncompressed = ~12 Gbps per stream
- One E810 100G port ≈ 8× 4K60 or 20× 1080p60 (theoretical)
- RL supports up to 128 concurrent shapers per port
- TSC resolution ≈ 1ns, but scheduler poll interval adds 10-100µs jitter
