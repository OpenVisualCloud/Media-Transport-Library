# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST20p pacing implementations across their load-dependent arithmetic.

Every case asserts the resolved pacing way because MTL can fall back to TSC
without failing the session. FFmpeg cannot select this parameter.
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import parse_fps_to_pformat, yuv_files

pytestmark = [pytest.mark.verified, pytest.mark.nightly, pytest.mark.tx_side]

# LS is excluded from the load sweep: both SD assets are interlaced, so it
# would confound field handling with load, and tv_train_pacing() rejects SD for
# RL regardless. test_st20p_pacing_way_sd_downgrade covers it directly.
LOAD_KEYS = ("i720p59", "i1080p59", "i2160p59", "i2160p119", "i4320p29")
CORE_KEYS = ("i1080p59", "i2160p59", "i2160p119")
# Ways reachable on a VF.
VF_PACING_WAYS = [
    pytest.param("rl", id="rl"),
    pytest.param("tsc", id="tsc"),
    pytest.param(
        "tsn",
        marks=pytest.mark.skip(
            reason="tsn launch-time pacing requires a PF; VF has no HW PHC"
        ),
        id="tsn",
    ),
]


@pytest.mark.parametrize(
    "application",
    [
        "rxtxapp",
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg does not support pacing_way selection"
            ),
        ),
    ],
)
@pytest.mark.parametrize("pacing_way", VF_PACING_WAYS)
@pytest.mark.parametrize(
    "media_file", [yuv_files[key] for key in LOAD_KEYS], indirect=True, ids=LOAD_KEYS
)
def test_st20p_pacing_way_load(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pacing_way,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """Each pacing way across the load surface its arithmetic depends on."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        pacing_way=pacing_way,
        test_time=test_time,
    )
    app = app_factory(application)
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
    app.assert_pacing_way(pacing_way)


@pytest.mark.parametrize(
    "media_file", [yuv_files[key] for key in CORE_KEYS], indirect=True, ids=CORE_KEYS
)
def test_st20p_pacing_way_auto(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """The default must resolve to RL rather than silently fall back to TSC."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        pacing_way="auto",
        test_time=test_time,
    )
    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
    app.assert_pacing_way("rl")


# PTP and TSN pacing require the hardware PHC exposed by a PF.
@pytest.mark.parametrize(
    "pacing_way",
    [
        pytest.param("ptp", id="ptp"),
        pytest.param("tsn", marks=pytest.mark.requires_txpp, id="tsn"),
    ],
)
@pytest.mark.parametrize(
    "media_file", [yuv_files[key] for key in CORE_KEYS], indirect=True, ids=CORE_KEYS
)
def test_st20p_pacing_way_phc(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    pacing_way,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """PHC-paced ways, which require a PF."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_mixed_interfaces_list_single(
            tx_interface_type="PF", rx_interface_type="VF"
        ),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        pacing_way=pacing_way,
        enable_ptp=True,
        test_time=60,
    )
    # Allow the PF to relink and PTP to converge before capture.
    test_time = 60
    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
    app.assert_pacing_way(pacing_way)


@pytest.mark.requires_nic_family("e810")
@pytest.mark.parametrize(
    "media_file", [yuv_files["i1080p59"]], indirect=True, ids=["i1080p59"]
)
def test_st20p_pacing_way_tsn_rejected_without_txpp(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    output_files,
    media_file,
):
    """E810 has no TxPP engine, so mtl_init() must reject tsn outright rather
    than accept it and quietly pace some other way."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_mixed_interfaces_list_single(
            tx_interface_type="PF", rx_interface_type="VF"
        ),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        pacing_way="tsn",
        enable_ptp=True,
        test_time=60,
    )
    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=60,
        host=host,
        fail_on_error=False,
    )
    app.assert_tsn_unsupported()


# The vrx adjustment is computed per way -- -4 for RL, bulk=1 for tsc_narrow,
# -(bulk-1) otherwise (st_tx_video_session.c:579-596) -- and the shaping
# profile sets the budget that adjustment is spent against, so the two are
# genuinely coupled rather than independently sweepable.
@pytest.mark.parametrize(
    "pacing",
    [
        pytest.param("narrow", id="narrow"),
        pytest.param("wide", marks=pytest.mark.allow_wide_compliance, id="wide"),
        pytest.param("linear", id="linear"),
    ],
)
@pytest.mark.parametrize(
    "pacing_way",
    [
        pytest.param("rl", id="rl"),
        pytest.param("tsc", id="tsc"),
        pytest.param("tsc_narrow", id="tsc_narrow"),
    ],
)
@pytest.mark.parametrize(
    "media_file", [yuv_files["i2160p59"]], indirect=True, ids=["i2160p59"]
)
def test_st20p_pacing_way_x_pacing(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pacing_way,
    pacing,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """Pacing way against shaping profile, at the load where the budget bites."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        pacing_way=pacing_way,
        pacing=pacing,
        test_time=test_time,
    )
    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
    )
    app.assert_pacing_way(pacing_way)


@pytest.mark.allow_wide_compliance
@pytest.mark.parametrize(
    "media_file", [yuv_files["i576i50"]], indirect=True, ids=["i576i50"]
)
def test_st20p_pacing_way_sd_downgrade(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    pcap_capture,
    output_files,
    media_integrity,
    media_file,
):
    """SD must report its RL-to-TSC fallback and preserve content."""
    media_info, media_path = media_file
    host = list(hosts.values())[0]
    config_params = dict(
        session_type="st20p",
        nic_port_list=setup_interfaces.get_interfaces_list_single("VF"),
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx[0],
        port=20000,
        test_mode="multicast",
        input_file=media_path,
        output_file=output_files.register(f"{media_path}.out"),
        width=media_info["width"],
        height=media_info["height"],
        framerate=parse_fps_to_pformat(media_info["fps"]),
        transport_format=media_info["format"],
        pixel_format=media_info["file_format"],
        pacing_way="rl",
        interlaced=True,
        test_time=test_time,
    )
    app = app_factory("rxtxapp")
    app.create_command(**config_params)
    run_ok = app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=host,
        compliance=pcap_capture,
        integrity=media_integrity,
        fail_on_error=False,
    )
    app.assert_pacing_way("tsc")
    assert run_ok, "SD fallback failed compliance or integrity validation"
