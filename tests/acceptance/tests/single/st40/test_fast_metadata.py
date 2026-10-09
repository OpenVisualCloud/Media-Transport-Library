# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST2110-41 fast metadata, carried by an ST40 ancillary session that opts in.

RX passes only when every data item arrives at the expected rate with the
DIT and K-bit TX set, zero padding, and the bytes of the file TX sends. RX
also holds every packet to SMPTE ST 2110-41:2024: marker bit 0 and RTP
version 2 (§5.2), no zero-length data item and a UDP datagram within the
1460-byte limit (§5.4), and no more than 500 ms between packets (§5.1).
"""

import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools
from mtl_engine.media_files import anc_files

pytestmark = [
    pytest.mark.nightly,
    pytest.mark.parametrize(
        "media_file",
        [anc_files["text_p59"]],
        indirect=["media_file"],
        ids=["text_p59"],
    ),
]

DIT = 1234567

# §8: the edges of each Data Item Type range a sender may use.
ALLOCATABLE_DITS = {
    "smpte_min": 0x000000,
    "user_org_min": 0x100000,
    "private_max": 0x2FFFFF,
    "experimental_min": 0x3FF000,
    "experimental_max": 0x3FFFFF,
}
# §8.5: reserved for future use, so a sender shall not use them.
RESERVED_DITS = {"reserved_min": 0x300000, "reserved_max": 0x3FEFFF}
RESERVED_DIT_ERROR = "0x300000-0x3fefff is reserved"
STATIC_PAYLOAD_TYPE_ERROR = "fast metadata needs a dynamic one (96-127)"


def _run(
    app,
    hosts,
    mtl_path,
    interfaces,
    test_time,
    media_file,
    fail_on_error=True,
    **params,
):
    media_file_info, media_file_path = media_file
    params.setdefault("test_mode", "multicast")
    params.setdefault("type_mode", "frame")
    params.setdefault("payload_type", 115)
    params.setdefault("fastmetadata_data_item_type", DIT)
    params.setdefault("fastmetadata_k_bit", 0)
    params.setdefault("ancillary_fps", media_file_info["fps"])
    app.create_command(
        session_type="ancillary",
        fast_metadata=True,
        nic_port_list=interfaces,
        ancillary_url=media_file_path,
        test_time=test_time,
        **params,
    )
    return app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=list(hosts.values())[0],
        fail_on_error=fail_on_error,
    )


def _skip_unicast_on_one_kernel_port(test_config, test_mode):
    """A unicast session needs a peer; one kernel-socket port has none.

    On a single-port card (i225) both ends of a test share one kernel
    interface, and the unicast destination is an address no host owns.
    """
    if test_mode == "unicast" and test_config.get("interface_type") == "KERNEL":
        pytest.skip("unicast needs a peer, a single kernel-socket port has none")


@pytest.mark.smoke
@pytest.mark.parametrize(
    "application",
    [
        # The one fast metadata case of the low-bandwidth leg at frame level.
        pytest.param("rxtxapp", marks=pytest.mark.low_bandwidth),
        pytest.param(
            "ffmpeg",
            marks=pytest.mark.skip(
                reason="FFmpeg plugin has no ST40 fast metadata session"
            ),
        ),
        pytest.param(
            "gstreamer",
            marks=pytest.mark.skip(
                reason="GStreamer plugin has no ST40 fast metadata session"
            ),
        ),
    ],
)
def test_fast_metadata_basic(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
):
    """A multicast frame-level session delivers every data item intact."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory(application),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
    )


@pytest.mark.parametrize(
    "test_mode, type_mode",
    [
        ("unicast", "frame"),
        ("unicast", "rtp"),
        # The one fast metadata case of the low-bandwidth leg at RTP level.
        pytest.param("multicast", "rtp", marks=pytest.mark.low_bandwidth),
    ],
)
def test_fast_metadata_type_mode(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    test_mode,
    type_mode,
):
    """Frame- and RTP-level sessions deliver every data item intact."""
    _skip_unicast_on_one_kernel_port(test_config, test_mode)
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        test_mode=test_mode,
        type_mode=type_mode,
    )


@pytest.mark.parametrize(
    "fps",
    ["p23", "p24", "p25", "p29", "p30", "p50", "p59", "p60", "p100", "p119", "p120"],
)
@pytest.mark.parametrize("type_mode", ["frame", "rtp"])
def test_fast_metadata_fps(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    fps,
    type_mode,
):
    """Data items are paced at every supported frame rate.

    p23 is the slowest, so it is the rate that comes closest to the 500 ms
    packet interval limit of §5.1.
    """
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        type_mode=type_mode,
        ancillary_fps=fps,
    )


@pytest.mark.parametrize(
    "dit", list(ALLOCATABLE_DITS.values()), ids=list(ALLOCATABLE_DITS)
)
@pytest.mark.parametrize("type_mode", ["frame", "rtp"])
def test_fast_metadata_data_item_type(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    dit,
    type_mode,
):
    """A Data Item Type from each range a sender may use reaches RX unchanged."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        type_mode=type_mode,
        fastmetadata_data_item_type=dit,
    )


@pytest.mark.parametrize("dit", list(RESERVED_DITS.values()), ids=list(RESERVED_DITS))
def test_fast_metadata_reserved_data_item_type_rejected(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    dit,
):
    """MTL refuses to send a Data Item Type from the reserved range."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory("rxtxapp")
    run_ok = _run(
        app,
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        fail_on_error=False,
        fastmetadata_data_item_type=dit,
    )
    assert not run_ok, f"a session with reserved DIT {dit:#x} ran"
    app.assert_rejected(RESERVED_DIT_ERROR)


@pytest.mark.parametrize("k_bit", [0, 1])
@pytest.mark.parametrize("type_mode", ["frame", "rtp"])
def test_fast_metadata_k_bit(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    k_bit,
    type_mode,
):
    """The K-bit reaches RX unchanged and does not disturb the DIT."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        type_mode=type_mode,
        fastmetadata_k_bit=k_bit,
    )


@pytest.mark.parametrize(
    "payload_type", [96, 115, 127], ids=["dynamic_min", "default", "dynamic_max"]
)
@pytest.mark.parametrize("type_mode", ["frame", "rtp"])
def test_fast_metadata_payload_type(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    payload_type,
    type_mode,
):
    """RX filters on the configured RTP payload type, across the dynamic range."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        type_mode=type_mode,
        payload_type=payload_type,
    )


@pytest.mark.parametrize("payload_type", [1, 95], ids=["static_min", "static_max"])
def test_fast_metadata_static_payload_type_rejected(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    payload_type,
):
    """MTL refuses a payload type outside the dynamic 96-127 range of §5.2."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    app = app_factory("rxtxapp")
    run_ok = _run(
        app,
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        fail_on_error=False,
        payload_type=payload_type,
    )
    assert not run_ok, f"a session with static payload type {payload_type} ran"
    app.assert_rejected(STATIC_PAYLOAD_TYPE_ERROR)


@pytest.mark.parametrize("type_mode", ["frame", "rtp"])
def test_fast_metadata_no_chain(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    media_file,
    type_mode,
):
    """The copy path builds the same packets as the mbuf-chain path."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        type_mode=type_mode,
        tx_no_chain=True,
    )


@pytest.mark.parametrize("type_mode", ["frame", "rtp"])
def test_fast_metadata_redundant(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    media_file,
    type_mode,
):
    """An ST 2022-7 session merges both legs into one intact stream."""
    interfaces = setup_interfaces.get_interfaces_list_single("VF", count=4)
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        media_file,
        type_mode=type_mode,
        test_mode="multicast",
        redundant=True,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        source_ip_r=ip_pools.tx_r[0],
        destination_ip_r=ip_pools.rx_multicast[1],
    )
