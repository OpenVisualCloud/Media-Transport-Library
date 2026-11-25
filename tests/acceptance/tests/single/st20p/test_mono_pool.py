# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST20p with every TX and RX queue on its port's mono pool.

``--mono_pool`` sets MTL_FLAG_TX_MONO_POOL and MTL_FLAG_RX_MONO_POOL: the
queues of a port share one mbuf pool instead of a pool each. Every replica
gets its own RX queue and, with one session per scheduler, its own RX lcore,
so every replica's recording must match its source. FFmpeg and GStreamer
cannot select the mono pool.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.config.universal_params import UNIVERSAL_PARAMS
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly]

REPLICAS = 4


@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(reason="FFmpeg does not support mono_pool"),
        ),
        pytest.param(
            "gstreamer",
            marks=pytest.mark.skip(reason="GStreamer does not support mono_pool"),
        ),
    ],
)
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_st20p_mono_pool(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    output_files,
    media_integrity,
    media_file,
):
    """Every session must stream intact when all its queues share the mono pool."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    app = app_factory(application)
    app.create_command(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=f"{media_path}.mono.out",
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        mono_pool=True,
        replicas=REPLICAS,
        # One 1080p59 TX session's quota per scheduler: each session gets its own.
        sch_session_quota=1,
        test_time=test_time,
        # Each replica records its own file; split the default cap between them.
        rx_max_file_size=UNIVERSAL_PARAMS["rx_max_file_size"] // REPLICAS,
    )
    # Track the files RxTxApp writes: <url>_<k> for each replica.
    for path in app.rx_output_files():
        output_files.register(path)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        integrity=media_integrity,
    )
