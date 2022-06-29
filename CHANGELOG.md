# Change log

## Change log for 22.06:
* User frame mode with uframe_pg_callback for rx video session.
* Color format SIMD convert API between RFC4175 YUV422 10bit BE and other LE format, see st_convert_api.h.
* SIMD build and runtime framework.
* Migration suppport for tx/rx video session if the cpu lcore usage is too busy, see ST_FLAG_TX_VIDEO_MIGRATE/ST_FLAG_RX_VIDEO_MIGRATE.
* DMA/DSA helper on SIMD convert API to reduce LLC usage for 4K/8K resolution, see st_convert_api.h.
* Format auto detect for rx video session, see ST20_RX_FLAG_AUTO_DETECT.
* rx: add payload type check in the hdr sanity inspection.
* st22: add frame mode support for tx and rx.
* st22: add pipeline mode support for both tx and rx, see st_pipeline_api.h for the API, tx_st22_pipeline_sample.c/rx_st22_pipeline_sample.c for the sample code.
* st22: add encode/decode plugin sample code, see jpegxs_plugin_sample.c for the sample and kahawai.json for the plugin so config.
* app: json config support for st22 pipeline mode.
* tx/st20: runtime session create/free features, adding runtime ratelimit support.
* st30: support 125us and 80us in audio pacing

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
