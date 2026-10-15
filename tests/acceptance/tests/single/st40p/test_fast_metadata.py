# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026 Intel Corporation
"""ST2110-41 fast metadata, carried by an st40p (ST40 pipeline) session that opts in.

Each pipeline frame is one data item. RX passes only when every data item
arrives at the expected rate with the DIT and K-bit TX set, zero padding,
and the bytes of the file TX sends, in order. RX also holds every item to
SMPTE ST 2110-41:2024: marker bit 0 (§5.2) and no more than 500 ms between
items (§5.1). The lib parses the packets, so it never hands RX a
zero-length or truncated data item (§5.4).

With FFmpeg or GStreamer at RX, st40p_recording_errors() in
mtl_engine/integrity.py checks the bytes, the DIT, the K-bit and the marker.
Only RxTxApp checks the 500 ms limit, and FFmpeg does not check the rate.
"""

import os

import mtl_engine.media_creator as media_create
import pytest
from common.nicctl import InterfaceSetup
from mtl_engine import ip_pools

pytestmark = pytest.mark.nightly

DIT = 1234567
FPS = "p59"

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


@pytest.fixture
def fmd_payload(hosts, media_file, output_files):
    """A random payload, so no two data items TX cuts from it are alike.

    Three bytes past the KB boundary leave a final data item that is not a
    whole number of 32-bit words, which exercises the zero padding.
    """
    _, ramdisk = media_file
    host = list(hosts.values())[0]
    path = output_files.register(os.path.join(ramdisk, "fast_metadata.txt"))
    media_create.create_text_file(size_kb=64, output_path=path, host=host)
    host.connection.execute_command(f"printf 'end' >> {path}")
    return path


def _run(
    app, hosts, mtl_path, interfaces, test_time, payload, fail_on_error=True, **params
):
    params.setdefault("test_mode", "multicast")
    params.setdefault("payload_type", 115)
    params.setdefault("fastmetadata_data_item_type", DIT)
    params.setdefault("fastmetadata_k_bit", 0)
    params.setdefault("framerate", FPS)
    app.create_command(
        session_type="st40p",
        fast_metadata=True,
        nic_port_list=interfaces,
        input_file=payload,
        test_time=test_time,
        **params,
    )
    return app.execute_test(
        build=mtl_path,
        test_time=test_time,
        host=list(hosts.values())[0],
        fail_on_error=fail_on_error,
    )


@pytest.mark.smoke
@pytest.mark.parametrize("application", ["rxtxapp", "ffmpeg", "gstreamer"])
def test_fast_metadata_basic(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
):
    """A multicast session delivers every data item intact."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory(application),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        fmd_payload,
    )


@pytest.mark.parametrize("application", ["ffmpeg", "gstreamer"])
def test_fast_metadata_streamer_header(
    application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
):
    """A streamer sends and receives every bit of the DIT and the K-bit."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory(application),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        fmd_payload,
        fastmetadata_data_item_type=ALLOCATABLE_DITS["experimental_max"],
        fastmetadata_k_bit=1,
    )


@pytest.mark.parametrize(
    "tx_application, rx_application",
    [("rxtxapp", "gstreamer"), ("gstreamer", "rxtxapp")],
    ids=["rxtxapp_to_gstreamer", "gstreamer_to_rxtxapp"],
)
def test_fast_metadata_cross_app(
    tx_application,
    rx_application,
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
):
    """RxTxApp and GStreamer cut the same data items, so each checks the other."""
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("gstreamer"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        fmd_payload,
        tx_application=tx_application,
        rx_application=rx_application,
    )


def test_fast_metadata_unicast(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
):
    """A unicast session delivers every data item intact.

    On a single-port card (i225) both ends of a test share one kernel
    interface, and the unicast destination is an address no host owns.
    """
    if test_config.get("interface_type") == "KERNEL":
        pytest.skip("unicast needs a peer, a single kernel-socket port has none")
    interfaces = setup_interfaces.get_interfaces_list_single(
        test_config.get("interface_type", "VF")
    )
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        fmd_payload,
        test_mode="unicast",
    )


@pytest.mark.parametrize(
    "fps",
    ["p23", "p24", "p25", "p29", "p30", "p50", "p59", "p60", "p100", "p119", "p120"],
)
def test_fast_metadata_fps(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
    fps,
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
        fmd_payload,
        framerate=fps,
    )


@pytest.mark.parametrize(
    "dit", list(ALLOCATABLE_DITS.values()), ids=list(ALLOCATABLE_DITS)
)
def test_fast_metadata_data_item_type(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
    dit,
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
        fmd_payload,
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
    fmd_payload,
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
        fmd_payload,
        fail_on_error=False,
        fastmetadata_data_item_type=dit,
    )
    assert not run_ok, f"a session with reserved DIT {dit:#x} ran"
    app.assert_rejected(RESERVED_DIT_ERROR)


@pytest.mark.parametrize("k_bit", [0, 1])
def test_fast_metadata_k_bit(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
    k_bit,
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
        fmd_payload,
        fastmetadata_k_bit=k_bit,
    )


@pytest.mark.parametrize(
    "payload_type", [96, 115, 127], ids=["dynamic_min", "default", "dynamic_max"]
)
def test_fast_metadata_payload_type(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
    payload_type,
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
        fmd_payload,
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
    fmd_payload,
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
        fmd_payload,
        fail_on_error=False,
        payload_type=payload_type,
    )
    assert not run_ok, f"a session with static payload type {payload_type} ran"
    app.assert_rejected(STATIC_PAYLOAD_TYPE_ERROR)


def test_fast_metadata_no_chain(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    test_config,
    fmd_payload,
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
        fmd_payload,
        tx_no_chain=True,
    )


def test_fast_metadata_redundant(
    app_factory,
    hosts,
    mtl_path,
    setup_interfaces: InterfaceSetup,
    test_time,
    fmd_payload,
):
    """An ST 2022-7 session merges both legs into one intact stream."""
    interfaces = setup_interfaces.get_interfaces_list_single("VF", count=4)
    _run(
        app_factory("rxtxapp"),
        hosts,
        mtl_path,
        interfaces,
        test_time,
        fmd_payload,
        test_mode="multicast",
        redundant=True,
        source_ip=ip_pools.tx[0],
        destination_ip=ip_pools.rx_multicast[0],
        source_ip_r=ip_pools.tx_r[0],
        destination_ip_r=ip_pools.rx_multicast[1],
    )
