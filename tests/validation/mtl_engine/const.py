# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import os

LOG_FOLDER = "logs"
PERF_LOG_FOLDER = f"{LOG_FOLDER}/performance"
TESTCMD_LVL = 24  # Custom logging level for test commands
FRAMES_CAPTURE = 4  # Number of frames to capture for compliance tests
FFMPEG_PATH = os.environ.get("MTL_FFMPEG_PATH", "./.local_install/ffmpeg/bin")
FFMPEG_EXE = os.environ.get("MTL_FFMPEG_EXE", f"{FFMPEG_PATH}/ffmpeg")
RXTXAPP_PATH = os.environ.get("MTL_RXTXAPP_PATH", "./.local_install/mtl/bin")
RXTXAPP_EXE = os.environ.get("MTL_RXTXAPP_EXE", f"{RXTXAPP_PATH}/RxTxApp")
MTL_MANAGER_EXE = os.environ.get("MTL_MANAGER_EXE", f"{RXTXAPP_PATH}/MtlManager")
MTL_LIB_PATH = os.environ.get(
    "MTL_LIB_PATH", "./.local_install/mtl/lib/x86_64-linux-gnu"
)
DPDK_LIB_PATH = os.environ.get(
    "DPDK_LIB_PATH", "./.local_install/dpdk/lib/x86_64-linux-gnu"
)
FFMPEG_LIB_PATH = os.environ.get("FFMPEG_LIB_PATH", "./.local_install/ffmpeg/lib")
FFPROBE_EXE = os.environ.get("MTL_FFPROBE_EXE", f"{FFMPEG_PATH}/ffprobe")
GSTREAMER_LIB_PATH = os.environ.get("GSTREAMER_LIB_PATH", "./.local_install/gstreamer")


LD_LIB_PATH_REL = (
    f"{MTL_LIB_PATH}:{DPDK_LIB_PATH}:{FFMPEG_LIB_PATH}:{GSTREAMER_LIB_PATH}"
)
