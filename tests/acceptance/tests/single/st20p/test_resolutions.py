# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import yuv_files_422p10le, yuv_files_422rfc10

pytestmark = pytest.mark.verified

_RESOLUTION_CASES = [
    *[
        pytest.param(
            "rxtxapp",
            media_file,
            marks=pytest.mark.smoke if name == "Penguin_1080p" else (),
            id=f"rxtxapp-rfc-{name}",
        )
        for name, media_file in yuv_files_422rfc10.items()
    ],
    *[
        pytest.param(application, media_file, id=f"{application}-planar-{name}")
        for application in ("rxtxapp", "ffmpeg", "gstreamer")
        for name, media_file in yuv_files_422p10le.items()
    ],
]


@pytest.mark.nightly
@pytest.mark.parametrize(
    ("application", "media_file"),
    _RESOLUTION_CASES,
    indirect=["media_file"],
)
def test_st20p_resolutions(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_config,
    test_time,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """Test different video resolutions.

    RFC 4175 assets exercise RxTxApp directly; planar assets exercise every
    adapter's conversion path.
    """
    media_file_info, media_file_path = media_file
    rx_output = output_files.register(f"{media_file_path}.out")
    host = list(hosts.values())[0]
    interfaces_list = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )

    config_params = {
        "session_type": "st20p",
        "nic_port_list": interfaces_list,
        "destination_ip": ip_pools.rx_multicast[0],
        "port": 20000,
        "width": media_file_info["width"],
        "height": media_file_info["height"],
        "framerate": f"p{media_file_info['fps']}",
        "pixel_format": media_file_info["file_format"],
        "transport_format": media_file_info["format"],
        "input_file": media_file_path,
        "output_file": rx_output,
        "test_mode": "multicast",
        "test_time": test_time,
    }

    app = app_factory(
        application,
        session_type="st20p",
        pixel_format=media_file_info["file_format"],
        transport_format=media_file_info["format"],
    )
    app.create_command(**config_params)

    height = media_file_info.get("height", 0)
    actual_test_time = test_time
    if height >= 2160:
        actual_test_time = max(test_time, 15)
    elif height >= 1080:
        actual_test_time = max(test_time, 10)
    else:
        actual_test_time = max(test_time, 8)

    app.execute_test(
        build=mtl_path,
        test_time=actual_test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
