# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422rfc10

pytestmark = pytest.mark.verified


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422rfc10["ParkJoy_1080p"]],
    indirect=["media_file"],
    ids=["ParkJoy_1080p"],
)
@pytest.mark.parametrize(
    "fps",
    [
        "p23",
        "p24",
        "p25",
        pytest.param("p29", marks=pytest.mark.smoke),
        "p30",
        "p50",
        "p59",
        "p60",
        "p100",
        "p119",
        "p120",
    ],
)
def test_fps(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    fps,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Validate unicast ST20P streaming across a matrix of frame rates to
    ensure timing support and pacing align with the configured FPS values
    for 1080p YUV422 RFC4175. This catches regressions in rate configuration
    or jitter handling across low (23/24/25) and high (100/119/120) frame
    rates.

    :param hosts: Mapping of hosts available for the test run.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test settings.
    :param test_time: Duration to run the streaming pipeline.
    :param fps: Frame rate string (e.g., ``p23``) selected from the
        parametrized list.
    :param test_config: Test configuration dictionary (e.g., interface type).
    :param prepare_ramdisk: Fixture preparing RAM disk storage for media files.
    :param media_file: Tuple fixture containing media metadata and file path.
    """
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(test_config.get("interface_type", "VF"))

    config = rxtxapp.create_empty_config()
    config = rxtxapp.add_st20p_sessions(
        config=config,
        nic_port_list=interfaces_list,
        test_mode="unicast",
        width=media_file_info["width"],
        height=media_file_info["height"],
        fps=fps,
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
