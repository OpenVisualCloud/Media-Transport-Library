# Change log

## Change log for next release:

## Change log for Beta 22.04:
* Enabled field mode to support 1080i,PAL,NTSC with narrow pacing.
* Mempool usage optimization for both TX/RX, each TX session has dedicated mempool, one mempool for each RX queue. It also improve LLC affinity extremely.
* DMA offload support for RX video, lcore usage reduced.
* DPDK 21.11 support.
* VF pass all GTest tests.
* Support for 25fps.
* Enhanced algorithm with noise filter for video pacing profiling and TSC freq calibration.
* Fix check sum calculation(rf1071) for multicast report message.
* RX video slice mode support for low latency.
* TX video slice mode support for low latency.
* DMA(copy/fill) user API support, st_udma_xxx.
* PCAP dump API support for RX video session.
* Support for yuv444 8, 10, 12, 16 bit.

## Change log for V0.7.1:
* Dynamic IP address change support for RX video session.
* Provide meta data info in the RX callback.
* Full audio channels support.
* GPM(general packet mode) and BPM(block packet mode) support for TX.
* 8K resolution support for both TX and RX.
* More automatic test cases coverage, 144 Gtest items totally.
* Add rtp path sample code.
* Add ST2110-22 sample code.

## Change log for first version V0.7.0:
* First version with new design based on DPDK 21.08.
