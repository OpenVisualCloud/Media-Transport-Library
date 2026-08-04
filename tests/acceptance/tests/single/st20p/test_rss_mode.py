# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST20p shared-RSS and flow-director receive paths.

Load and replica count exercise queue distribution; the resolved-path oracle
proves that packets traversed shared RSS when requested. FFmpeg cannot select
this parameter.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly, pytest.mark.rx_side]

RSS_CASES = [
    pytest.param("l3_l4", 1, yuv_files["i1080p59"], id="l3_l4-x1-i1080p59"),
    pytest.param("l3", 1, yuv_files["i1080p59"], id="l3-x1-i1080p59"),
    pytest.param("none", 1, yuv_files["i1080p59"], id="none-x1-i1080p59"),
    pytest.param("l3_l4", 4, yuv_files["i2160p119"], id="l3_l4-x4-i2160p119"),
    pytest.param("l3", 4, yuv_files["i2160p119"], id="l3-x4-i2160p119"),
]


@pytest.mark.parametrize(
    "rss_mode, replicas, media_file",
    RSS_CASES,
    indirect=["media_file"],
)
def test_st20p_rss_mode(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    rss_mode,
    replicas,
    output_files,
    media_integrity,
    media_file,
):
    """The requested dispatch path must be the one that carries the stream."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    app = app_factory("rxtxapp")
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        rss_mode=rss_mode,
        replicas=replicas,
        test_time=test_time,
    )
    if replicas == 1:
        config_params["output_file"] = output_files.register(f"{media_path}.rss.out")
    else:
        media_integrity.skip("replicated receivers cannot share one integrity output")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        integrity=media_integrity,
    )
    app.assert_rss_mode(rss_mode)
