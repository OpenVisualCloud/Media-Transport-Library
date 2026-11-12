.. Media Transport Library documentation master file, created by
   skolelis
   You can adapt this file completely to your liking, but it should at least
   contain the root `toctree` directive.

   SPDX-License-Identifier: BSD-3-Clause
   Copyright(©) 2025 Intel Corporation
   Media Transport Library


Media Transport Library
======================================================

.. image:: doc/png/mtl-banner.svg
   :alt: Media Transport Library: send and receive live video, audio and ancillary data over IP networks, in software, on standard Intel Xeon servers and Ethernet NICs
   :align: center

The demo below shows SMPTE ST 2022-7 seamless redundancy with the
:doc:`MTL with MXL <ecosystem/MTL_with_MXL/README>` bridge. MTL receives an
ST 2110-20 stream over a primary and a redundant path on Intel E835 NICs,
merges the two copies and hands the frames to the Media eXchange Layer (MXL)
over RDMA. While one path is cut or loses packets, or both paths drop
different packets, the merged output keeps playing.

.. image:: doc/png/mtl-st2022-7-redundancy-demo.gif
   :alt: ST 2022-7 seamless redundancy demo: the merged output keeps playing while the primary or the redundant path is cut or drops packets
   :align: center

.. toctree::
   :maxdepth: 1

   Introduction <README.md>
   security.md
   CHANGELOG.md
   Licensing <ATTRIBUTION.md>

Getting Started
----------------

.. toctree::
   :maxdepth: 1
   :caption: Getting Started:

   Documentation Map <doc/README.md>
   doc/getting_started/build.md
   doc/getting_started/e800_series_drivers.md
   doc/getting_started/run.md
   MTL Manager <manager/README.md>
   doc/design.md

Platforms
----------

.. toctree::
   :maxdepth: 1
   :caption: Platforms:

   Build Guide on Windows <doc/platforms/build_WIN.md>
   doc/platforms/run_WIN.md
   doc/platforms/vm.md
   doc/platforms/vm_WIN.md
   doc/platforms/aws.md
   doc/platforms/sdm_appliance.md

Features
---------

.. toctree::
   :maxdepth: 1
   :caption: Features:

   doc/features/convert.md
   doc/features/dma.md
   doc/features/external_frame.md
   doc/features/gpu.md
   gpu_direct/README.md
   doc/features/plugin.md
   doc/features/rtcp.md

Performance
------------

.. toctree::
   :maxdepth: 1
   :caption: Performance:

   doc/performance/rl_pacing.md
   doc/performance/isolation.md
   doc/performance/shm_lcore.md
   ST2110 Compliance Result <doc/performance/compliance.md>
   doc/performance/performance.md

Debugging
----------

.. toctree::
   :maxdepth: 1
   :caption: Debugging:

   doc/debugging/stats_guide.md
   doc/debugging/usdt.md
   doc/debugging/asan.md

Sample Application
-------------------

.. toctree::
   :maxdepth: 1
   :caption: Sample Application:

   app/sample/README.md
   app/sample/msvc/README.md
   app/v4l2_to_ip/README.md
   doc/getting_started/configuration_guide.md

Ecosystem
----------

.. toctree::
   :maxdepth: 1
   :caption: Ecosystem:

   Ffmpeg Plugin <ecosystem/ffmpeg_plugin/README.md>
   GStreamer Plugin <ecosystem/gstreamer_plugin/README.md>
   plugins/st22_avcodec/README.md
   OBS Studio <ecosystem/obs_mtl/README.md>
   MTL with MXL <ecosystem/MTL_with_MXL/README.md>

Development and Testing
------------------------

.. toctree::
   :maxdepth: 1
   :caption: Development and Testing:

   doc/development/coding_standard.md
   doc/development/acceptance_quickstart.md
   doc/development/acceptance-design.md
   doc/development/ci_runner_setup.md
   doc/development/cicd_setup_proposition.md

Doxygen
--------

.. toctree::
   :maxdepth: 1
   :caption: Doxygen:

   doc/development/doxygen.md
   doc/doxygen/solution_overview.md
   doc/doxygen/main_page.md
   doc/doxygen/api.md
   doc/doxygen/programmers_guide.md

Other
------

.. toctree::
   :maxdepth: 1
   :caption: Other:

   Python Support <python/README.md>
   Rust Support <rust/README.md>
   docker/README.md
   tools/ebpf/README.md
   tools/set_tai_offset/README.md
   doc/sphinx/build_docs.md


Experimental features
----------------------

.. toctree::
   :maxdepth: 1
   :caption: Experimental features:

   doc/experimental/kernel_socket.md
   doc/experimental/xdp.md
   doc/experimental/af_xdp.md
   doc/experimental/af_packet.md
   doc/experimental/header_split.md
   doc/experimental/performance_optimizations.md

