# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

"""An st20p transfer under a MtlManager, checked in the log of the manager.

Each application runs one TX and one RX session through the manager. The
case passes when the transfer passes and the manager log shows that every
libmtl instance registered, gave back each lcore it took, and disconnected,
with no ERROR line.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import GstreamerApp, ip_pools
from mtl_engine.media_files import yuv_files_422p10le

pytestmark = pytest.mark.mtlmanager

# RxTxApp runs TX and RX in one process. FFmpeg and GStreamer run one process
# for TX and one for RX, so the manager serves two instances.
MIN_INSTANCES = {"rxtxapp": 1, "ffmpeg": 2, "gstreamer": 2}


def _run_gstreamer(host, mtl_path, interfaces_list, media_file, rx_output, test_time):
    media_file_info, media_file_path = media_file
    gst_format = GstreamerApp.video_format_change(media_file_info["file_format"])
    tx_command = GstreamerApp.setup_gstreamer_st20p_tx_pipeline(
        build=mtl_path,
        nic_port_list=interfaces_list[0],
        input_path=media_file_path,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=str(media_file_info["fps"]),
        format=gst_format,
        tx_payload_type=112,
        tx_queues=4,
    )
    rx_command = GstreamerApp.setup_gstreamer_st20p_rx_pipeline(
        build=mtl_path,
        nic_port_list=interfaces_list[1],
        output_path=rx_output,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=str(media_file_info["fps"]),
        format=gst_format,
        rx_payload_type=112,
        rx_queues=4,
    )
    passed = GstreamerApp.execute_test(
        build=mtl_path,
        tx_command=tx_command,
        rx_command=rx_command,
        input_file=media_file_path,
        output_file=rx_output,
        test_time=test_time,
        host=host,
        tx_first=False,
        sleep_interval=2,
    )
    assert passed, "GStreamer st20p transfer failed"


@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg", "gstreamer"])
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    indirect=["media_file"],
    ids=["Penguin_1080p"],
)
def test_mtl_manager_st20p(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    output_files,
    media_file,
    manager_log,
):
    """Run st20p under a MtlManager and check the log of the manager."""
    media_file_info, media_file_path = media_file
    rx_output = output_files.register(f"{media_file_path}.out")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    if application == "gstreamer":
        _run_gstreamer(
            host, mtl_path, interfaces_list, media_file, rx_output, test_time
        )
    else:
        app = app_factory(application)
        app.create_command(
            session_type="st20p",
            nic_port_list=interfaces_list,
            test_mode="multicast",
            destination_ip=ip_pools.rx_multicast[0],
            port=20000,
            width=media_file_info["width"],
            height=media_file_info["height"],
            framerate=f"p{media_file_info['fps']}",
            pixel_format=media_file_info["file_format"],
            transport_format=media_file_info["format"],
            input_file=media_file_path,
            output_file=rx_output,
            test_time=test_time,
        )
        app.execute_test(build=mtl_path, test_time=test_time, host=host)

    manager_log.check(min_instances=MIN_INSTANCES[application])
