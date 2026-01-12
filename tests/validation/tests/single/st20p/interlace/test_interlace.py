# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2024-2025 Intel Corporation

import mtl_engine.RxTxApp as rxtxapp
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_interlace

pytestmark = pytest.mark.verified


@pytest.mark.nightly
@pytest.mark.parametrize(
    "media_file",
    list(yuv_files_interlace.values()),
    indirect=["media_file"],
    ids=list(yuv_files_interlace.keys()),
)
def test_interlace(
    hosts,
    build,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    prepare_ramdisk,
    media_file,
):
    """
    Validate unicast streaming of interlaced YUV media, ensuring ST20P
    handles interlaced frame signaling, field ordering, and session setup for
    each sample. This guards against regressions in interlace flags or pacing
    when handling non-progressive sources.

    :param hosts: Mapping of hosts available for the test run.
    :param build: Compiled Rx/Tx application artifact used for execution.
    :param setup_interfaces: Fixture configuring NIC interfaces per test settings.
    :param test_time: Duration to run the streaming pipeline.
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
        fps=f"p{media_file_info['fps']}",
        input_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
        output_format=media_file_info["file_format"],
        st20p_url=media_file_path,
        interlaced=True,
    )

    rxtxapp.execute_test(
        config=config,
        build=build,
        test_time=test_time,
        host=host,
    )
