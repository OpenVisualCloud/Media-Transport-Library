# SPDX-License-Identifier: BSD-3-Clause

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine.media_files import yuv_files_422p10le
from mtl_engine.rxtxapp import RxTxApp


@pytest.mark.parametrize(
    "media_file",
    [yuv_files_422p10le["Penguin_1080p"]],
    ids=["Penguin_1080p"],
)
@pytest.mark.parametrize("quality", ["quality", "speed"])
@pytest.mark.nightly
def test_quality_refactored(
    hosts,
    build,
    media,
    setup_interfaces: InterfaceSetup,
    test_time,
    quality,
    test_config,
    prepare_ramdisk,
    pcap_capture,
    media_file,
):
    media_file_info, media_file_path = media_file
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    app = RxTxApp(app_path="./tests/tools/RxTxApp/build")
    # Note: kahawai.json handling should be done via remote host connection if needed
    # For now, assume it's already available on the remote host or not required
    app.create_command(
        session_type="st22p",
        test_mode="multicast",
        nic_port_list=interfaces_list,
        width=media_file_info["width"],
        height=media_file_info["height"],
        framerate=f"p{media_file_info['fps']}",
        codec="JPEG-XS",
        quality=quality,
        pixel_format=media_file_info["file_format"],
        input_file=media_file_path,
        codec_threads=2,
        test_time=test_time,
    )
    result = app.execute_test(
        build=build, test_time=test_time, host=host, netsniff=pcap_capture
    )
    # Enforce result to avoid silent pass when validation fails
    assert (
        result
    ), "Refactored st22p quality test failed validation (TX/RX outputs or return code)."
