# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

LOG_FOLDER = "logs"
PERF_LOG_FOLDER = f"{LOG_FOLDER}/performance"
TESTCMD_LVL = 24  # Custom logging level for test commands
FRAMES_CAPTURE = 4  # Number of frames to capture for compliance tests

# Intel NIC PCI device IDs (lowercased, vendor-less form of pci_device.device_id)
E810_DEVICE_ID = "1592"  # Intel E810 Series Ethernet Adapter
E830_DEVICE_ID = "12d2"  # Intel E830 Series Ethernet Adapter
E835_DEVICE_ID = "1249"  # Intel E835 Series Ethernet Adapter

PREFIX = ".local_install"
FFMPEG_PATH = f"{PREFIX}/ffmpeg/bin"
FFMPEG_EXE = f"{FFMPEG_PATH}/ffmpeg"
FFPROBE_EXE = f"{FFMPEG_PATH}/ffprobe"
RXTXAPP_PATH = f"{PREFIX}/mtl/bin"
RXTXAPP_EXE = f"{RXTXAPP_PATH}/RxTxApp"
MTL_MANAGER_EXE = f"{RXTXAPP_PATH}/MtlManager"
MTL_LIB_PATH = f"{PREFIX}/mtl/lib64"
MTL_LIB_PATH = f"{MTL_LIB_PATH}:{PREFIX}/mtl/lib/x86_64-linux-gnu"
DPDK_LIB_PATH = f"{PREFIX}/dpdk/lib/x86_64-linux-gnu"
FFMPEG_LIB_PATH = f"{PREFIX}/ffmpeg/lib"
GSTREAMER_LIB_PATH = f"{PREFIX}/gstreamer/gstreamer-1.0"
PLUGIN_PATH = f"{PREFIX}/plugins/lib/x86_64-linux-gnu"


LD_LIB_PATH_REL = f"{MTL_LIB_PATH}:{DPDK_LIB_PATH}:{FFMPEG_LIB_PATH}:{GSTREAMER_LIB_PATH}:{PLUGIN_PATH}"
