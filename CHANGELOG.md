# Change log

## Change log for 22.09:
* License: update to BSD-3
* dpdk: update DPDK to v22.07
* ice: update driver to 1.9.11
* vf: add SRIOV based virtual NIC support, see vf.md.
* vm: add SRIOV+KVM based virtulization support, see vm.md.
* CSC: Color format SIMD convert API from CPU little endian format to RFC4175 YUV422 10bit BE, see st_convert_api.h.
* AF_XDP: introduce AF_XDP PMD experimental support, see af_xdp.md.
* pipeline: introduce pipeline friendly API for both st20 tx and rx, see st_pipeline_api.h for detail.
* iova: add map/unmap support for IO device, see st_dma_map and st_dma_unmap.
* Header split: introduce hdr split offload experimental support, see header_split.md.
* Ext frame: external frame mode to support user allocated memory, see st20_ext_frame and ST20_TX_FLAG_EXT_FRAME/ST20P_TX_FLAG_EXT_FRAME.
* st30: support 250us, 333us, and 4ms packet time.
* Windows: support DSA for rx offload.
* Windows: add TAP driver support.
* API: change st20_frame_meta to st20_rx_frame_meta, also add st20_tx_frame_meta for get_next_frame of st20 tx.
* API: change st_frame_meta to st_frame, also change the callback arg of st20p.notify_frame_done from void* to struct st_frame*.
* plugin: add kahawai as a plugin to OBS, only rx path now.
* fps: add 120(119.88) fps support, see ST_FPS_P119_88.

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
