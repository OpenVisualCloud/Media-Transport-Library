# RTCP Retransmission

## Introduction

With the expansion of the Media Transport Library (MTL) to cloud and edge environments, maintaining high-quality media streaming amidst the potential for packet loss becomes paramount.
In these distributed environments, network conditions can be highly variable, leading to increased risk of packet drops.
Traditional techniques such as SMPTE ST2022-7 hitless switching for redundant paths or implementation of Forward Error Correction (FEC) protocols, although effective, bring additional complexity or require significantly more bandwidth.

To address these challenges while adapting to the versatile deployment scenarios, MTL has been enhanced with an RTCP (Real-time Transport Control Protocol) retransmission feature.
This software-driven approach provides a flexible solution for handling packet loss that can complement or even take the place of traditional methods in cloud and edge computing contexts.

## RTCP Design and Workflow

The RTCP retransmission mechanism within MTL is designed with the cloud and edge paradigm in mind, ensuring that media streams can be reliably transmitted even in the face of adverse network conditions.
The workflow leverages RTCP to monitor the status of each packet, and in the event of packet loss, an RTCP NACK (Negative Acknowledgment) is promptly generated to request the retransmission of the lost packet(s).
This real-time detection and correction process serves to maintain the continuity and quality of the media stream.

![RTCP Retransmission](png/rtcp.svg)

## RTCP Configuration Code Example

To enable and configure RTCP in MTL for packet loss handling, the following code snippet provides a template:

```cpp
ops_tx.flags |= ST20P_TX_FLAG_ENABLE_RTCP;  // Enabling RTCP on the transmitter side
ops.rtcp.buffer_size = 1024;        // Setting RTCP buffer size for retransmission

ops_rx.flags |= ST20P_RX_FLAG_ENABLE_RTCP;  // Enabling RTCP on the receiver side
ops.rtcp.nack_interval_us = 250;         // Defining the NACK interval for loss detection (in microseconds)
ops.rtcp.seq_bitmap_size = 64;           // The size of the sequence number bitmap for tracking packet loss
ops.rtcp.seq_skip_window = 4;            // The allowable sequence number skip window
```

In this configuration:

- The `ST20P_TX_FLAG_ENABLE_RTCP` and `ST20P_RX_FLAG_ENABLE_RTCP` flags are set to enable RTCP on both transmission and reception paths, respectively. Other supported session types are `st20`, `st22` and `st22p`.
- The transmitter's `rtcp_buffer_size` sets the buffer capacity for storing packets which might need to be retransmitted. The buffer size should not be less than tx descriptor size.
- The receiver's `nack_interval_us` specifies how frequently it will check and potentially request retransmission of lost packets.
- The `seq_bitmap_size` determines the range of sequence numbers the receiver will monitor for loss detection. The total number of packets for tracking is `seq_bitmap_size * 8`.
- The `seq_skip_window` configures the permissible range within which out-of-order packets will be accepted without triggering a NACK.

By integrating RTCP retransmission within MTL, the robustness and reliability of media streaming over IP in varied deployment scenarios are significantly enhanced, solidifying MTL's role in modern media transport solutions.

## Performance Evaluation

To measure the efficacy of the RTCP retransmission feature under varying network conditions, we conducted a series of simulated tests on the receiver side (RX).
These tests intentionally introduced packet loss to evaluate how different rates of loss impact the frame receive ratio.
By adjusting the RTCP parameters, we were able to observe the resilience of the system and identify optimal configurations for different scenarios.

| video format | tx buffer | rx window | rx nack interval | packet loss rate | complete frame ratio |
| :----: | :----: | :----: | :----: | :----: | :----: |
| 1080p60 | / | / | / | 10% | 67.9% |
| 1080p60 | 512 | 256 | 100 | 10% | 0% |
| 1080p60 | 512 | 256 | 400 | 10% | 0% |
| 1080p60 | 1024 | 512 | 400 | 10% | 99.6% |
| **1080p60** | **1024** | **512** | **250** | **10%** | **100%** |
| 1080p60 | 1024 | 512 | 250 | 15% | 99.9% |
| 1080p60 | 1024 | 512 | 250 | 20% | 99.6% |

The results demonstrate that with careful configuration, RTCP retransmission can significantly mitigate the impact of packet loss, ensuring that media streams remain stable and of high quality, even as network performance fluctuates.
