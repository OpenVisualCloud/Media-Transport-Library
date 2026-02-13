# Changelog

## Changelog for 26.06

* st40: add ST40P pipeline sessions (TX + RX) to RxTxApp via JSON `"st40p"` key
* st40: add ST 2022-7 redundancy support for ST40P with configurable path delay
* st40: add Python validation tests for redundant ST40P (`test_st40p_redundant.py`)

## Changelog for 26.01

* DPDK: add support for DPDK 25.03, 25.07, and 25.11
* st40: promote ST40 pipeline API out of experimental tree with full RX/TX support
* st40: add split-packet and interlaced mode for ancillary data
* st40: add RX timestamp to st40_frame_info
* st30: add RX timestamp to st30_frame
* st20/st30/st40: add TX_FLAG_EXACT_USER_PACING for precise user-controlled pacing
* st20p: add ST20P_TX_FLAG_DROP_OLD_FRAME to fix buffer overflow in case of epoch drops
* st20: Change ST2022-7 stream class from D to A; update packet detection and slot allocation
* GStreamer: add ST40 RX plugin, DMA offload, zero-copy for ST20, PTP/redundant support
* FFmpeg: add redundant mode support
* Add unified stats retrieval API
* ice: add support for driver version 1.16.3, and 2.2.8
* Add support for e830 NIC
* Remove RDMA and Intel Media SDK support
* General bugfixes and stability improvements

## Changelog for 25.02

* Add plugins for GStreamer audio video and ancillary data
* Add experimental st40 tx pipeline mode
* Correct the implementation of flow rules in DPDK
* Add Sphinx documentation
* General code improvements and optimizations

## Changelog for 24.09

* ice: update driver to 1.14.9
* st2110/20: add force NUMA option support on session level, see ST20_TX_FLAG_FORCE_NUMA/ST20_RX_FLAG_FORCE_NUMA
* st2110/30: add force NUMA option support on session level, see ST30_TX_FLAG_FORCE_NUMA/ST30_RX_FLAG_FORCE_NUMA
* ffmpeg: fix RX side dropping frames at the beginning of the session with st20/st22/st30.
* st22: fix last frame dropping in TX. Ensure that last frame status changed to FREE.
* DPDK: optimizing memory pool size.
* manager: fix docker build.
* ffmpeg: improve unicast initialization, reduce amount of dropping frames in the beginning of the session.
* ixgbe: add driver support. Tested on 10-Gigabit X540-AT2 (1528) and Intel 10G X550T (1563).
* sch/tasklet: fix API correct NUMA assigned when `mtl_sch_create` is used.
* sch/tasklet: fix segfault when lcore out of `RTE_MAX_LCORE` assigned.
* app: add new video formats to sample app - YUV_420_16bit, YUV_422_8BIT, YUV_444_8bit, YUV_444_16bit.
* RTP: fix checking for valid payload type.
* st30: add `fifo_size` parameter parsing from user.
* st41: add `St2110-41` format for 'Fast Metadata Framework' standard.
* ffmpeg: add support of `44100` rate for `st30` format.
* ffmpeg: add support for v7.0 version
* st22: fix correct NUMA assigned `socket_id` with pipeline when creating a new session.
* GPU: add support for GPU direct buffers in ST2110/20. See `app/sample/gpu_direct` for usage.
* ffmpeg: add support for GPU buffers.

## Changelog for 24.06

* DPDK: upgrade DPDK version to 23.11.
* st22: add interlaced support.
* log: add custom log printer, see mtl_set_log_printer.
* rx/timing_parser: add timing_parser stat report for RX video, `--rx_timing_parser` in RxTxApp to enable.
* pipeline: add block get mode support, see `ST20P_TX_FLAG_BLOCK_GET`/`ST20P_RX_FLAG_BLOCK_GET`/`ST22P_TX_FLAG_BLOCK_GET`/`ST22P_RX_FLAG_BLOCK_GET`.
* rx/timing_parser: add support to export the timing_parser to app, see `app/sample/rx_st20p_timing_parser_sample.c` for usage.
* st40: add interlaced support.
* cvt: add st20_rfc4175_422be10_to_yuv422p8 with avx512
* backend: add XDP based backend, see doc/xdp.md.
* manager: add a daemon server for privileged control management, see manager/README.md.
* backend/xdp: add UDP port filter XDP program for splitting data path traffic.
* ice: update driver to 1.13.7
* rx/timing_parser: add timing_parser for audio, see `ST30_RX_FLAG_TIMING_PARSER_STAT` and `ST30_RX_FLAG_TIMING_PARSER_META`
* tools/ebpf: add lcore_monitor to monitor the lcore status for debug usage
* tools/ebpf: add udp_monitor to sniff all UDP streaming on the network
* usdt: add eBPF based User Statically-Defined Tracing (USDT) probes support, see `doc/usdt.md`
* multicast: add MTL_FLAG_NO_MULTICAST option
* st30p: add get/put support for audio, detail see `include/st30_pipeline_api.h`
* ffmpeg: add audio plugin support

## Changelog for 23.12

* log: add log to file support, see mtl_openlog_stream.
* DPDK: upgrade DPDK version to 23.07.
* virtio_user: add virtio_user support for exception path, deprecate kni.
* st22p/tx: add external frame support, see ST22P_TX_FLAG_EXT_FRAME.
* backend: add kernel socket based backend, see doc/kernel_socket.md.
* DPDK pmd: add AF_PACKET PMD support, see doc/experimental/af_packet.md.
* st22p/rx: add external frame support, see ST22P_RX_FLAG_EXT_FRAME.
* ptp: add user callback for ptp sync message. See ptp_sync_notify in struct mtl_init_params.
* api: add arp timeout parameter support for st2110 unicast address. See arp_timeout_s in struct mtl_init_params.
* st2110/tx: fix RTP passthrough interface support when PMD doesn't support multi segment mbuf.
* st2110/tx: fix redundant when PMD doesn't support multi segment mbuf.
* backend/kernel: add multi thread support for both TX and RX.
* convert: add interlace support.
* rtcp: add retransmit packet support for st20 sessions, see STxx_RX_FLAG_ENABLE_RTCP.
* st2110: add ssrc support.
* rss: add multi-core support.
* log: default add time info print, and a API `mtl_set_log_prefix_formatter` to customize the log prefix formatter.
* lcore: add `LcoreMgr` tools to manage the inactive lcore status. See doc/lcore.md
* ice: update driver to 1.12.7

## Changelog for 23.08

* lib: add DHCP client implementation.
* ice: update driver to 1.11.14
* udp: add LD_PRELOAD to support no-code change deployment, see doc/udp.md
* udp: add select/epoll/send_msg/recvmsg support.
* ecosystem/ffmpeg: add output support.
* rss: add shared mode support, see mt_shared_rss.h
* st/video: add multi_src_port support.
* sample: add hdr split with gpu direct, see app/sample/ext_frame/rx_st20p_hdr_split_gpu_direct.c
* api/ops: add udp_src_port configuration
* build: disable Werror for debug build.
* tx/st30: separate build and pacing stage.
* tx/pacing: add epoch drop/onward stat.
* tx/st2110: add epoch information in stxx_tx_frame_meta for get_next_frame callback.
* DPDK: upgrade DPDK version to latest 23.03
* Windows: add MSYS2 build guide and CI
* vm: add Windows guest OS support, see vm_WIN.md.
* rx/video: add fpt(first packet time to epoch) in struct st20_rx_frame_meta.
* st20p: add interlace format support, refer to tests/script/loop_json/st20p_2v_1080i50.json
* CI: add ossf scorecard support.
* gtest: support run with AWS ena driver.
* dhcp: set proto to dhcp in interfaces segment of JSON to enable DHCP.
* rx/video: support out of sequence for both first and last(marker) packet.
* udp: add gso(general segment offload) for tx
* udp: add reuse port support.
* udp: add fork support
* Windows: add MSVC sample build, refer to app/sample/msvc/imtl_sample.
* audio: improve max density per process to 750 for both tx and rx by shared queue and no_tx_chain.
* ptp: add tsc time source support for VF.
* doc: use vf mode as default for E810 nic.
* tx/st20: improve the pacing profile accuracy.

## Changelog for 23.04

* DPDK: upgrade DPDK version to latest 22.11 for both Windows and Linux.
* ptp: add pi controller and software frequency adjust to improve the accuracy to ~100ns.
* mtl_init_params: add gateway and netmask support for wan.
* udp: introduce a highly efficient udp stack support, see mudp_api.h
* udp: add POSIX socket compatible(file descriptor) API support, see mudp_sockfd_api.h
* udp: add sample code, see app/udp
* arp: add zero timeout support for UDP stack.
* mtl_init_params: add multi port support, user can initial MTL instance with up to 8 ports. See tests/script/multi_port_json/ for how to use from RxTxApp.
* sch/tasklet: add runtime unregister support.
* udp: add lcore daemon mode support for rx, see MTL_FLAG_UDP_LCORE.
* dev: add rss mode support for rx queue in case no rte_flow available.
* st2110/rx: add rss queue mode support
* st2110/tx/video: add tx no chain mode support
* st2110/tx/video: enable IOVA PA mode in 2M huge page environment
* sample/pipeline: add SQD split and merge sample
* sample/pipeline: add downscale sample
* sample/pipeline: add 2si-down sample
* CSC: add rfc4175 422be12 to le/planar avx512 version
* AWS: add ENA running guide, see aws.md for detail

## Changelog for 22.12

* tasklet: add thread and sleep option for core usage, see ST_FLAG_TASKLET_THREAD and ST_FLAG_TASKLET_SLEEP.
* tx: add user timestamp control, see ST20_TX_FLAG_USER_TIMESTAMP/ST30_TX_FLAG_USER_TIMESTAMP/ST40_TX_FLAG_USER_TIMESTAMP.
* rx/video: add dual core redundant mode support. See st20_redundant_api.h for detail. Header split only support this dual core redundant mode.
* lib/log: add notice level log, see ST_LOG_LEVEL_NOTICE.
* build: add clang support. See "Build with clang" section in build.md.
* tx: add user pacing control, see ST20_TX_FLAG_USER_PACING, ST30_TX_FLAG_USER_PACING, ST40_TX_FLAG_USER_PACING.
* video: add notify_vsync callback which happened when epoch time change to a new frame, vsync period is same to fps. See notify_vsync parameter in the session create ops for detail.
* csc: add y210 format support, y210 is the format for GPU 10bit yuv422 layout.
* sch/tasklet: enhance sleep feature with timer based sleep and user control.
* build: add AddressSanitizer(aka ASan) support, see asan.md
* fps: add full fps support, see README.md.
* plugin: implement OBS Studio source plugin, support UYVY/NV12/I420 formats.
* ext frame: add external frame support, see doc/external_frame.md.
* build: rename package name to mtl, also API naming prefix to mtl_xxx.
* st20p: add YUV_422_12bit YUV_444_10bit YUV_444_12bit RGB_10bit RGB_12bit support.
* plugin: add ffmpeg support with MTL package, see plugins/ffmpeg_plugin/.
* st20p: add packet level convert mode, see ST20P_RX_FLAG_PKT_CONVERT.
* arp: cache the results to a table, the second arp request can use the history results directly.
* st20p: add plane and linesize support, see struct st_frame.
* st20p: add st_frame_convert API which both the source and destination are st_frame.
* sample: add MSDK encode sample with MTL source, see app/msdk.

## Changelog for 22.09

* License: update to BSD-3
* DPDK: update DPDK to v22.07
* ice: update driver to 1.9.11
* vf: add SRIOV based virtual NIC support, see vf.md.
* vm: add SRIOV+KVM based virtualization support, see vm.md.
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
* API: change st_frame_meta to st_frame, also change the callback arg of st20p.notify_frame_done from (void*) to struct (st_frame*).
* plugin: add Media Transport Library as a plugin to OBS, only rx path now.
* fps: add 120(119.88) fps support, see ST_FPS_P119_88.

## Changelog for 22.06

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

## Changelog for Beta 22.04

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

## Changelog for V0.7.1

* Dynamic IP address change support for RX video session.
* Provide metadata info in the RX callback.
* Full audio channels support.
* GPM(general packet mode) and BPM(block packet mode) support for TX.
* 8K resolution support for both TX and RX.
* More automatic test cases coverage, 144 Gtest items totally.
* Add rtp path sample code.
* Add ST2110-22 sample code.

## Changelog for first version V0.7.0

* First version with new design based on DPDK 21.08.
