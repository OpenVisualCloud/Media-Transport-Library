# Documentation Map

Start here. Each folder holds the documents of one topic.

## First Steps

To run your first ST 2110 stream on Linux, read these documents in this order:

1. [Build Guide](getting_started/build.md): build DPDK, the Media Transport Library and the sample applications.
2. [E800 Series Driver Guide](getting_started/e800_series_drivers.md): check and install the driver of an Intel® E810 or E830 adapter.
3. [Run Guide](getting_started/run.md): set up IOMMU, VFs and huge pages, then run the sample application.

The [Design Guide](design.md) tells how the library works inside.

## Folders

| Folder | Documents |
| --- | --- |
| [getting_started/](getting_started/) | [Build Guide](getting_started/build.md), [E800 Series Driver Guide](getting_started/e800_series_drivers.md), [Run Guide](getting_started/run.md), [JSON Configuration Guide](getting_started/configuration_guide.md) of the sample application |
| [platforms/](platforms/) | Other places to run the library: [Windows build](platforms/build_WIN.md) and [Windows run](platforms/run_WIN.md), [Linux VM](platforms/vm.md), [Windows VM](platforms/vm_WIN.md), [AWS](platforms/aws.md), [Intel® SDM appliance](platforms/sdm_appliance.md) |
| [features/](features/) | Optional functions of the library: [color format convert](features/convert.md), [DMA for video RX](features/dma.md), [external frame API](features/external_frame.md), [GPU frame buffers](features/gpu.md), [plugins](features/plugin.md), [RTCP retransmission](features/rtcp.md) |
| [performance/](performance/) | [CPU isolation](performance/isolation.md), [lcore guide without the Manager](performance/shm_lcore.md), [ST 2110 compliance result](performance/compliance.md), [benchmark](performance/performance.md) |
| [debugging/](debugging/) | [Session statistics](debugging/stats_guide.md), [USDT probes](debugging/usdt.md), [AddressSanitizer](debugging/asan.md) |
| [experimental/](experimental/) | Other network backends and features: [kernel socket](experimental/kernel_socket.md), [native AF_XDP](experimental/xdp.md), [DPDK AF_XDP PMD](experimental/af_xdp.md), [DPDK AF_PACKET PMD](experimental/af_packet.md), [header split](experimental/header_split.md), [kernel network tuning](experimental/performance_optimizations.md) |
| [development/](development/) | For contributors: [coding standard](development/coding_standard.md), [acceptance tests: setup and run](development/acceptance_quickstart.md), [acceptance tests: design](development/acceptance-design.md), [CI runner setup](development/ci_runner_setup.md), [CI/CD proposal](development/cicd_setup_proposition.md), [Doxygen API documentation](development/doxygen.md) |

## Other Folders

- [`chunks/`](chunks/): parts that other documents include. Do not read them alone.
- [`doxygen/`](doxygen/): the pages of the API documentation.
- [`png/`](png/): the images of the documents.
- [`sphinx/`](sphinx/): the build of the documentation site. See the [Build Documentation Guide](sphinx/build_docs.md).

The plugins of FFmpeg, GStreamer and OBS have their documents in [ecosystem/](../ecosystem/).
